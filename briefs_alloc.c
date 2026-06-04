/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/buffer_head.h>

#include "briefs.h"
#include "briefs_alloc.h"

/* Read a trie node from disk by node index within the trie pool.
 * The trie pool starts at sb_disk->trie_node_pool_start.
 * Block 0 of the pool is the trie root header.
 * Node data blocks start at pool_start + 1.
 * Each block holds 128 nodes (4096 / 32 = 128).
 */
static struct trie_node *briefs_read_trie_node(struct super_block *sb, struct briefs_superblock *sb_disk, u64 node_index)
{
	struct buffer_head *bh;
	struct trie_node *node;
	u64 nodes_per_block = sb->s_blocksize / sizeof(struct trie_node);
	u64 block_offset;
	u64 byte_offset;

	/* Node index 0 is invalid (reserved for "no child") */
	if (node_index == 0)
		return NULL;

	/* Convert node index to block number and byte offset within the pool */
	block_offset = sb_disk->trie_node_pool_start + 1 + ((node_index - 1) / nodes_per_block);
	byte_offset = ((node_index - 1) % nodes_per_block) * sizeof(struct trie_node);

	bh = sb_bread(sb, block_offset);
	if (!bh)
		return ERR_PTR(-EIO);

	node = kmalloc(sizeof(struct trie_node), GFP_KERNEL);
	if (!node) {
		brelse(bh);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(node, bh->b_data + byte_offset, sizeof(struct trie_node));
	brelse(bh);

	return node;
}

/* Recursively load trie nodes from disk by node index */
static int briefs_load_trie_recursive(struct super_block *sb, struct briefs_superblock *sb_disk, struct trie_node **node_ptr, u64 node_index)
{
	struct trie_node *node;
	int ret;

	if (node_index == 0) {
		*node_ptr = NULL;
		return 0;
	}

	node = briefs_read_trie_node(sb, sb_disk, node_index);
	if (IS_ERR(node))
		return PTR_ERR(node);
	if (!node) {
		*node_ptr = NULL;
		return 0;
	}

	/* Load children recursively (children are stored as node indices) */
	if (node->left_child) {
		ret = briefs_load_trie_recursive(sb, sb_disk, (struct trie_node **)&node->left_child, node->left_child);
		if (ret) {
			kfree(node);
			return ret;
		}
	}

	if (node->right_child) {
		ret = briefs_load_trie_recursive(sb, sb_disk, (struct trie_node **)&node->right_child, node->right_child);
		if (ret) {
			if (node->left_child)
				kfree((struct trie_node *)node->left_child);
			kfree(node);
			return ret;
		}
	}

	*node_ptr = node;
	return 0;
}

/* Recursively free trie nodes */
static void briefs_free_trie_recursive(struct trie_node *node)
{
	if (!node)
		return;

	if (node->left_child)
		briefs_free_trie_recursive((struct trie_node *)node->left_child);
	if (node->right_child)
		briefs_free_trie_recursive((struct trie_node *)node->right_child);

	kfree(node);
}

/*
 * Initialize allocator from superblock
 *
 * The on-disk trie is written by mkfs and used for validation/fsck, but
 * for mount we build a fresh in-memory trie from the superblock metadata.
 * This avoids loading potentially millions of nodes (the trie is 2*N-1
 * nodes for N data blocks, padded to next power of 2).
 */
int briefs_alloc_init(struct briefs_alloc *alloc, struct super_block *sb, struct briefs_superblock *sb_disk)
{
	if (!alloc || !sb || !sb_disk)
		return -EINVAL;

	alloc->sb = sb_disk;

	/* Create a fresh in-memory root node covering the entire data region */
	alloc->root_node = kzalloc(sizeof(struct trie_node), GFP_KERNEL);
	if (!alloc->root_node)
		return -ENOMEM;

	alloc->root_node->range_start = 0;
	alloc->root_node->range_len = sb_disk->data_blocks;
	alloc->root_node->free_count = sb_disk->free_data_blocks;
	alloc->root_node->left_child = 0;
	alloc->root_node->right_child = 0;

	pr_debug("briefs: allocator initialized (data_blocks=%llu, free=%llu)\n",
		sb_disk->data_blocks, sb_disk->free_data_blocks);

	return 0;
}

/*
 * Find leftmost contiguous range of free blocks
 * Returns the starting block, sets *out_len to the length found
 */
u64 briefs_find_leftmost_contiguous(struct briefs_alloc *alloc, u64 needed, u64 *out_len)
{
	if (!alloc || !alloc->root_node || !out_len || needed == 0) {
		if (out_len)
			*out_len = 0;
		return 0;
	}

	/* If root can't satisfy, return 0 */
	if (alloc->root_node->free_count < needed) {
		*out_len = 0;
		return 0;
	}

	/* For now, simplified: just return 0 as the leftmost range */
	/* Full implementation needs to walk the trie recursively */
	*out_len = needed;
	return 0;
}

/*
 * Find and allocate N contiguous free blocks
 * Returns starting physical block, or 0 on failure
 */
u64 briefs_alloc_contiguous(struct briefs_alloc *alloc, u64 nblocks)
{
	if (!alloc || nblocks == 0)
		return 0;

	/* Check if we have enough total free blocks */
	if (alloc->root_node && alloc->root_node->free_count < nblocks) {
		return 0;
	}

	/* Find leftmost contiguous range */
	u64 len = 0;
	u64 start = briefs_find_leftmost_contiguous(alloc, nblocks, &len);

	if (len < nblocks) {
		return 0;  /* Not enough contiguous blocks */
	}

	/* Mark the range as allocated */
	if (alloc->root_node) {
		briefs_mark_allocated(alloc->root_node, start, nblocks);
	}

	return start;
}

/*
 * Allocate a single free block
 */
u64 briefs_alloc_block(struct briefs_alloc *alloc)
{
	return briefs_alloc_contiguous(alloc, 1);
}

/*
 * Free N contiguous blocks
 */
void briefs_free_contiguous(struct briefs_alloc *alloc, u64 phys_block, u64 nblocks)
{
	if (!alloc || nblocks == 0)
		return;

	/* Mark the range as free */
	if (alloc->root_node) {
		briefs_mark_free(alloc->root_node, phys_block, nblocks);
	}
}

/*
 * Free a single block
 */
void briefs_free_block(struct briefs_alloc *alloc, u64 phys_block)
{
	briefs_free_contiguous(alloc, phys_block, 1);
}

/*
 * Mark a range as allocated in the trie
 */
void briefs_mark_allocated(struct trie_node *node, u64 offset, u64 count)
{
	if (!node || count == 0)
		return;

	/* Base case: leaf node */
	if (node->range_len == 1) {
		node->free_count = 0;
		return;
	}

	/* Recursive case: internal node */
	u64 half = node->range_len / 2;

	if (offset < half) {
		/* Go left */
		if (node->left_child) {
			briefs_mark_allocated((struct trie_node *)node->left_child, offset, count);
		}
	} else {
		/* Go right */
		u64 right_offset = offset - half;
		if (node->right_child) {
			briefs_mark_allocated((struct trie_node *)node->right_child, right_offset, count);
		}
	}

	/* Recount from children after recursive calls */
	briefs_recount(node);
}

/*
 * Mark a range as free in the trie
 */
void briefs_mark_free(struct trie_node *node, u64 offset, u64 count)
{
	if (!node || count == 0)
		return;

	/* Base case: leaf node */
	if (node->range_len == 1) {
		node->free_count = 1;
		return;
	}

	/* Recursive case: internal node */
	u64 half = node->range_len / 2;

	if (offset < half) {
		/* Go left */
		if (node->left_child) {
			briefs_mark_free((struct trie_node *)node->left_child, offset, count);
		}
	} else {
		/* Go right */
		u64 right_offset = offset - half;
		if (node->right_child) {
			briefs_mark_free((struct trie_node *)node->right_child, right_offset, count);
		}
	}

	/* Recount from children after recursive calls */
	briefs_recount(node);
}

/*
 * Rebuild free_count from children
 */
void briefs_recount(struct trie_node *node)
{
	if (!node)
		return;

	if (node->range_len == 1) {
		/* Leaf node - free_count is already correct */
		return;
	}

	/* Internal node: sum children's free_count */
	u64 left_free = 0, right_free = 0;

	if (node->left_child) {
		struct trie_node *left = (struct trie_node *)node->left_child;
		left_free = left->free_count;
	}

	if (node->right_child) {
		struct trie_node *right = (struct trie_node *)node->right_child;
		right_free = right->free_count;
	}

	node->free_count = left_free + right_free;
}

/*
 * Count trailing free blocks (rightmost free blocks in a node's range)
 */
u64 briefs_count_trailing_free(struct trie_node *node)
{
	if (!node || node->free_count == 0)
		return 0;
	if (node->range_len == 1)
		return node->free_count;

	/* Check right child first */
	if (node->right_child) {
		struct trie_node *right = (struct trie_node *)node->right_child;
		if (right->free_count == right->range_len) {
			/* Right child is fully free */
			return right->range_len;
		}
		/* Could be partially free - need recursive count */
	}

	return 0;
}

/*
 * Count leading free blocks (leftmost free blocks in a node's range)
 */
u64 briefs_count_leading_free(struct trie_node *node)
{
	if (!node || node->free_count == 0)
		return 0;
	if (node->range_len == 1)
		return node->free_count;

	/* Check left child first */
	if (node->left_child) {
		struct trie_node *left = (struct trie_node *)node->left_child;
		if (left->free_count == left->range_len) {
			/* Left child is fully free */
			return left->range_len;
		}
		/* Could be partially free - need recursive count */
	}

	return 0;
}

/*
 * Check if a range is entirely free
 */
bool briefs_range_is_free(struct trie_node *node, u64 offset, u64 count)
{
	if (!node)
		return false;
	if (count == 0)
		return true;

	/* Leaf node: check if range_len == count and free_count == count */
	if (node->range_len == 1) {
		return (count == 1 && node->free_count == 1);
	}

	/* Internal node: recursively check */
	u64 half = node->range_len / 2;

	if (offset + count <= half) {
		/* Entirely in left subtree */
		if (node->left_child) {
			return briefs_range_is_free((struct trie_node *)node->left_child, offset, count);
		}
	} else if (offset >= half) {
		/* Entirely in right subtree */
		u64 right_offset = offset - half;
		if (node->right_child) {
			return briefs_range_is_free((struct trie_node *)node->right_child, right_offset, count);
		}
	} else {
		/* Spans both subtrees */
		/* Check left part and right part separately */
		bool left_ok = false, right_ok = false;
		if (node->left_child) {
			left_ok = briefs_range_is_free((struct trie_node *)node->left_child, offset, half - offset);
		}
		if (node->right_child) {
			right_ok = briefs_range_is_free((struct trie_node *)node->right_child, 0, offset + count - half);
		}
		return left_ok && right_ok;
	}

	return false;
}

/*
 * Cleanup allocator
 */
void briefs_alloc_cleanup(struct briefs_alloc *alloc)
{
	if (!alloc)
		return;

	if (alloc->root_node) {
		briefs_free_trie_recursive(alloc->root_node);
		alloc->root_node = NULL;
	}

	alloc->sb = NULL;
}
