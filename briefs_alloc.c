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
 *
 * node_index 0 is valid (the first node in the first data block).
 * The caller (briefs_load_trie_recursive) treats 0 as "no child"
 * only when passed as a child pointer, not when loading the root.
 */
static struct trie_node *briefs_read_trie_node(struct super_block *sb, struct briefs_superblock *sb_disk, u64 node_index)
{
	struct buffer_head *bh;
	struct trie_node *node;
	u64 nodes_per_block = sb->s_blocksize / sizeof(struct trie_node);
	u64 block_offset;
	u64 byte_offset;

	/* Convert node index to block number and byte offset within the pool */
	block_offset = sb_disk->trie_node_pool_start + 1 + (node_index / nodes_per_block);
	byte_offset = (node_index % nodes_per_block) * sizeof(struct trie_node);

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

/* Recursively load trie nodes from disk by node index.
 * Returns 0 on success with *node_ptr set, or -errno on error.
 * A node_index of 0 is a valid node (it is the first node in
 * the trie data blocks). The caller guards against no-child
 * sentinels by checking left_child/right_child before calling.
 *
 * If node_by_index is non-NULL, stores the node pointer at
 * node_by_index[node_index] for later sync.
 */
static int briefs_load_trie_recursive(struct super_block *sb, struct briefs_superblock *sb_disk,
                                       struct trie_node **node_ptr, u64 node_index,
                                       struct trie_node **node_by_index)
{
	struct trie_node *node;
	int ret;

	node = briefs_read_trie_node(sb, sb_disk, node_index);
	if (IS_ERR(node))
		return PTR_ERR(node);
	if (!node) {
		*node_ptr = NULL;
		return 0;
	}

	/* Record this node in the index array */
	if (node_by_index)
		node_by_index[node_index] = node;

	/* Load children recursively (children are stored as node indices) */
	if (node->left_child) {
		ret = briefs_load_trie_recursive(sb, sb_disk, (struct trie_node **)&node->left_child,
		                                 node->left_child, node_by_index);
		if (ret) {
			kfree(node);
			return ret;
		}
	}

	if (node->right_child) {
		ret = briefs_load_trie_recursive(sb, sb_disk, (struct trie_node **)&node->right_child,
		                                 node->right_child, node_by_index);
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
 * Loads the on-disk allocation trie written by mkfs into memory.
 * The trie is a complete binary trie where leaves represent single
 * data blocks and internal nodes track free_count for their range.
 * Each node is 32 bytes, stored at 128 nodes per 4096-byte block
 * in the trie node pool.
 */
int briefs_alloc_init(struct briefs_alloc *alloc, struct super_block *sb, struct briefs_superblock *sb_disk)
{
	struct buffer_head *bh;
	struct briefs_trie_root *root_hdr;
	u64 root_node_index;
	int ret;

	if (!alloc || !sb || !sb_disk)
		return -EINVAL;

	alloc->sb = sb_disk;
	alloc->node_by_index = NULL;
	alloc->node_count = 0;

	/* Read trie root header block to get the root node index */
	bh = sb_bread(sb, sb_disk->trie_root_block);
	if (!bh) {
		pr_err("briefs: failed to read trie root header block %llu\n",
			sb_disk->trie_root_block);
		return -EIO;
	}

	root_hdr = (struct briefs_trie_root *)bh->b_data;
	if (root_hdr->magic != 0x54524945) {
		pr_err("briefs: invalid trie root magic: 0x%08x\n", root_hdr->magic);
		brelse(bh);
		return -EINVAL;
	}

	root_node_index = root_hdr->root_node;
	alloc->node_count = root_hdr->node_count;
	pr_debug("briefs: trie root header: magic=0x%08x, version=%u, root_node=%llu, node_count=%u\n",
		root_hdr->magic, root_hdr->version, root_node_index, root_hdr->node_count);
	brelse(bh);

	/* Allocate node index array for sync */
	if (alloc->node_count > 0) {
		alloc->node_by_index = kcalloc(alloc->node_count, sizeof(struct trie_node *), GFP_KERNEL);
		if (!alloc->node_by_index) {
			pr_err("briefs: failed to allocate trie index array (%llu entries)\n",
				alloc->node_count);
			return -ENOMEM;
		}
	}

	/* Recursively load the entire trie from disk */
	ret = briefs_load_trie_recursive(sb, sb_disk, &alloc->root_node, root_node_index,
	                                 alloc->node_by_index);
	if (ret) {
		pr_err("briefs: failed to load allocation trie (err=%d)\n", ret);
		kfree(alloc->node_by_index);
		alloc->node_by_index = NULL;
		return ret;
	}

	if (!alloc->root_node) {
		pr_err("briefs: no root node loaded from trie\n");
		kfree(alloc->node_by_index);
		alloc->node_by_index = NULL;
		return -EIO;
	}

	pr_info("briefs: allocator initialized from on-disk trie (data_blocks=%llu, free=%llu)\n",
		sb_disk->data_blocks, alloc->root_node->free_count);

	return 0;
}

/*
 * Find the leftmost range of contiguous free blocks in the trie.
 * Returns the starting block, sets *out_len to the size of the found range
 * (which may be less than needed if no contiguous range of needed size exists).
 */
u64 briefs_find_leftmost_contiguous(struct trie_node *node, u64 needed, u64 *out_len)
{
	if (!node || !out_len || node->free_count == 0 || needed == 0) {
		if (out_len)
			*out_len = 0;
		return 0;
	}

	/* Leaf node: return this block if free */
	if (node->range_len == 1) {
		if (node->free_count > 0) {
			*out_len = 1;
			return node->range_start;
		}
		*out_len = 0;
		return 0;
	}

	/* Internal node: try left child first for leftmost allocation */
	if (node->left_child) {
		struct trie_node *left = (struct trie_node *)node->left_child;

		if (left->free_count > 0) {
			/* Find the leftmost free range within the left child */
			u64 left_len = 0;
			u64 result = briefs_find_leftmost_contiguous(left, min(needed, left->free_count), &left_len);

			if (left_len >= needed) {
				*out_len = needed;
				return result;
			}

			/* Not enough in left alone — see if it spans into right */
			if (left_len > 0 && result + left_len == left->range_start + left->range_len) {
				/* The free range abuts the right boundary */
				if (node->right_child) {
					u64 right_len = 0;
					briefs_find_leftmost_contiguous((struct trie_node *)node->right_child, needed - left_len, &right_len);
					if (right_len > 0) {
						*out_len = left_len + right_len;
						return result;
					}
				}
			}

			*out_len = left_len;
			return result;
		}
	}

	/* Left had nothing, try right */
	if (node->right_child) {
		struct trie_node *right = (struct trie_node *)node->right_child;
		if (right->free_count > 0)
			return briefs_find_leftmost_contiguous(right, needed, out_len);
	}

	*out_len = 0;
	return 0;
}

/*
 * Find and allocate N contiguous free blocks.
 * Returns starting physical block, or 0 on failure.
 */
u64 briefs_alloc_contiguous(struct briefs_alloc *alloc, u64 nblocks)
{
	u64 len = 0;
	u64 start;

	if (!alloc || nblocks == 0)
		return 0;

	/* Check if we have enough total free blocks */
	if (!alloc->root_node || alloc->root_node->free_count < nblocks)
		return 0;

	/* Find leftmost contiguous range */
	start = briefs_find_leftmost_contiguous(alloc->root_node, nblocks, &len);

	if (len < nblocks)
		return 0;

	/* Mark the range as allocated */
	briefs_mark_allocated(alloc->root_node, start, nblocks);

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
 * Mark a range as allocated in the trie.
 * Handles ranges that span both children.
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

	/* Entirely in left child */
	if (offset + count <= half) {
		if (node->left_child)
			briefs_mark_allocated((struct trie_node *)node->left_child, offset, count);
		goto recount;
	}

	/* Entirely in right child */
	if (offset >= half) {
		if (node->right_child)
			briefs_mark_allocated((struct trie_node *)node->right_child, offset - half, count);
		goto recount;
	}

	/* Spans both children */
	u64 left_count = half - offset;
	u64 right_count = count - left_count;

	if (node->left_child)
		briefs_mark_allocated((struct trie_node *)node->left_child, offset, left_count);
	if (node->right_child)
		briefs_mark_allocated((struct trie_node *)node->right_child, 0, right_count);

recount:
	briefs_recount(node);
}

/*
 * Mark a range as free in the trie.
 * Handles ranges that span both children.
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

	/* Entirely in left child */
	if (offset + count <= half) {
		if (node->left_child)
			briefs_mark_free((struct trie_node *)node->left_child, offset, count);
		goto recount;
	}

	/* Entirely in right child */
	if (offset >= half) {
		if (node->right_child)
			briefs_mark_free((struct trie_node *)node->right_child, offset - half, count);
		goto recount;
	}

	/* Spans both children */
	u64 left_count = half - offset;
	u64 right_count = count - left_count;

	if (node->left_child)
		briefs_mark_free((struct trie_node *)node->left_child, offset, left_count);
	if (node->right_child)
		briefs_mark_free((struct trie_node *)node->right_child, 0, right_count);

recount:
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
 * Sync the in-memory trie back to disk.
 *
 * Walks all trie nodes by index and writes their free_count values
 * to the corresponding on-disk blocks. Only dirty blocks (where at
 * least one node's free_count changed) are written.
 *
 * The trie structure (node indices, child pointers) never changes
 * after mkfs — only free_count values are modified in memory.
 */
int briefs_alloc_sync(struct briefs_alloc *alloc, struct super_block *sb)
{
	struct briefs_superblock *sb_disk;
	u64 nodes_per_block;
	u64 total_blocks;
	u64 bi;

	if (!alloc || !sb || !alloc->root_node || !alloc->sb || !alloc->node_by_index)
		return -EINVAL;

	sb_disk = alloc->sb;
	nodes_per_block = sb->s_blocksize / sizeof(struct trie_node);
	total_blocks = (alloc->node_count + nodes_per_block - 1) / nodes_per_block;

	pr_debug("briefs: syncing %llu trie nodes across %llu blocks\n",
		alloc->node_count, total_blocks);

	for (bi = 0; bi < total_blocks; bi++) {
		u64 block_offset = sb_disk->trie_node_pool_start + 1 + bi;
		struct buffer_head *bh;
		u64 start_idx, end_idx, ni;
		bool dirty = false;

		bh = sb_bread(sb, block_offset);
		if (!bh) {
			pr_err("briefs: failed to read trie block %llu during sync\n", block_offset);
			return -EIO;
		}

		start_idx = bi * nodes_per_block;
		end_idx = start_idx + nodes_per_block;
		if (end_idx > alloc->node_count)
			end_idx = alloc->node_count;

		for (ni = start_idx; ni < end_idx; ni++) {
			struct trie_node *node = alloc->node_by_index[ni];
			u64 byte_offset;
			__u32 *disk_free_count;

			if (!node)
				continue;

			byte_offset = (ni % nodes_per_block) * sizeof(struct trie_node)
			              + offsetof(struct trie_node, free_count);
			disk_free_count = (__u32 *)(bh->b_data + byte_offset);

			if (*disk_free_count != node->free_count) {
				*disk_free_count = node->free_count;
				dirty = true;
			}
		}

		if (dirty) {
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
		}

		brelse(bh);
	}

	pr_debug("briefs: trie sync complete\n");
	return 0;
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

	kfree(alloc->node_by_index);
	alloc->node_by_index = NULL;
	alloc->node_count = 0;
	alloc->sb = NULL;
}
