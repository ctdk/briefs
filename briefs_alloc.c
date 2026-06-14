/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/buffer_head.h>

#include "briefs.h"
#include "briefs_alloc.h"

/*
 * Initialize allocator from superblock.
 *
 * Reads the 3-level bitmap pyramid from disk into vmalloc'd arrays.
 * The on-disk layout is:
 *   Block 0:   struct alloc_pool_header
 *   Block 1..: Level 0 words (packed 512 per block)
 *   Block ..:  Level 1 words
 *   Block ..:  Level 2 words
 *
 * Each level is a flat array of u64 words. A set bit = "has free blocks".
 * A cleared bit = "fully allocated".
 */
int briefs_alloc_init(struct briefs_alloc *alloc, struct super_block *sb,
                      struct briefs_superblock *sb_disk)
{
	return briefs_alloc_init_at(alloc, sb, sb_disk->trie_node_pool_start);
}

/*
 * Initialize allocator from an explicit on-disk pool block offset.
 */
int briefs_alloc_init_at(struct briefs_alloc *alloc, struct super_block *sb,
                          u64 pool_block)
{
	struct buffer_head *bh;
	struct alloc_pool_header *hdr;
	u64 pool_start, l0_blocks, l1_blocks, l2_blocks;
	u64 pos, i, w;

	if (!alloc || !sb)
		return -EINVAL;

	memset(alloc, 0, sizeof(*alloc));
	mutex_init(&alloc->lock);
	alloc->sb = sb;
	alloc->alloc_pool_start = pool_block;

	pool_start = pool_block;

	/* Read header block */
	bh = sb_bread(sb, pool_start);
	if (!bh) {
		pr_err("briefs: failed to read allocator header block at %llu\n", pool_start);
		return -EIO;
	}

	hdr = (struct alloc_pool_header *)bh->b_data;
	if (hdr->magic != ALLOC_MAGIC) {
		pr_err("briefs: invalid allocator magic: 0x%08x (expected 0x%08x)\n",
			hdr->magic, ALLOC_MAGIC);
		brelse(bh);
		return -EINVAL;
	}

	alloc->l0_words = hdr->l0_words;
	alloc->l1_words = hdr->l1_words;
	alloc->l2_words = hdr->l2_words;
	alloc->block_count = hdr->block_count;
	alloc->free_count = hdr->free_count;

	brelse(bh);

	pr_info("briefs: allocator at block %llu: l0=%llu words, l1=%llu words, l2=%llu words, entries=%llu, free=%llu\n",
		pool_block, alloc->l0_words, alloc->l1_words, alloc->l2_words,
		alloc->block_count, alloc->free_count);

	/* Allocate arrays */
	alloc->l0 = __vmalloc(alloc->l0_words * sizeof(u64), GFP_KERNEL);
	alloc->l1 = __vmalloc(alloc->l1_words * sizeof(u64), GFP_KERNEL);
	alloc->l2 = __vmalloc(alloc->l2_words * sizeof(u64), GFP_KERNEL);

	if (!alloc->l0 || !alloc->l1 || !alloc->l2) {
		pr_err("briefs: failed to allocate allocator arrays\n");
		vfree(alloc->l0);
		vfree(alloc->l1);
		vfree(alloc->l2);
		return -ENOMEM;
	}

	/* Compute block counts per level */
	u64 words_per_block = sb->s_blocksize / sizeof(u64);

	l0_blocks = (alloc->l0_words + words_per_block - 1) / words_per_block;
	l1_blocks = (alloc->l1_words + words_per_block - 1) / words_per_block;
	l2_blocks = (alloc->l2_words + words_per_block - 1) / words_per_block;

	/* Read level 0 */
	pos = pool_start + 1;
	w = 0;
	for (i = 0; i < l0_blocks; i++) {
		u64 remaining = alloc->l0_words - w;
		u64 n = min_t(u64, remaining, words_per_block);
		u64 j;

		bh = sb_bread(sb, pos + i);
		if (!bh) {
			pr_err("briefs: failed to read allocator L0 block %llu\n", pos + i);
			vfree(alloc->l0);
			vfree(alloc->l1);
			vfree(alloc->l2);
			return -EIO;
		}
		for (j = 0; j < n; j++)
			alloc->l0[w + j] = ((u64 *)bh->b_data)[j];
		brelse(bh);
		w += n;
	}

	/* Read level 1 */
	pos += l0_blocks;
	w = 0;
	for (i = 0; i < l1_blocks; i++) {
		u64 remaining = alloc->l1_words - w;
		u64 n = min_t(u64, remaining, words_per_block);
		u64 j;

		bh = sb_bread(sb, pos + i);
		if (!bh) {
			pr_err("briefs: failed to read allocator L1 block %llu\n", pos + i);
			vfree(alloc->l0);
			vfree(alloc->l1);
			vfree(alloc->l2);
			return -EIO;
		}
		for (j = 0; j < n; j++)
			alloc->l1[w + j] = ((u64 *)bh->b_data)[j];
		brelse(bh);
		w += n;
	}

	/* Read level 2 */
	pos += l1_blocks;
	w = 0;
	for (i = 0; i < l2_blocks; i++) {
		u64 remaining = alloc->l2_words - w;
		u64 n = min_t(u64, remaining, words_per_block);
		u64 j;

		bh = sb_bread(sb, pos + i);
		if (!bh) {
			pr_err("briefs: failed to read allocator L2 block %llu\n", pos + i);
			vfree(alloc->l0);
			vfree(alloc->l1);
			vfree(alloc->l2);
			return -EIO;
		}
		for (j = 0; j < n; j++)
			alloc->l2[w + j] = ((u64 *)bh->b_data)[j];
		brelse(bh);
		w += n;
	}

	pr_info("briefs: allocator at block %llu initialized from disk (%llu entries, %llu free)\n",
		pool_block, alloc->block_count, alloc->free_count);

	return 0;
}

/*
 * Allocate a single free block.
 * Returns data-relative block number, or 0 if no space.
 */
u64 briefs_alloc_block(struct briefs_alloc *alloc)
{
	u64 w0, b0, w1_idx, l1_word, b1, w2_idx, l2_word, b2, block;

	mutex_lock(&alloc->lock);
	if (!alloc || alloc->free_count == 0 || !alloc->l0) {
		mutex_unlock(&alloc->lock);
		return 0;
	}

	for (w0 = 0; w0 < alloc->l0_words; w0++) {
		if (alloc->l0[w0] == 0)
			continue;
		b0 = __builtin_ctzll(alloc->l0[w0]);

		w1_idx = w0 * 64 + b0;
		if (w1_idx >= alloc->l1_words) {
			mutex_unlock(&alloc->lock);
			return 0;
		}
		l1_word = alloc->l1[w1_idx];
		if (l1_word == 0) {
			alloc->l0[w0] &= ~(1ULL << b0);
			continue;
		}
		b1 = __builtin_ctzll(l1_word);

		w2_idx = w1_idx * 64 + b1;
		if (w2_idx >= alloc->l2_words) {
			pr_err("briefs: alloc: w2_idx=%llu out of range (max=%llu)\n",
				w2_idx, alloc->l2_words);
			mutex_unlock(&alloc->lock);
			return 0;
		}
		l2_word = alloc->l2[w2_idx];
		if (l2_word == 0) {
			alloc->l1[w1_idx] &= ~(1ULL << b1);
			if (alloc->l1[w1_idx] == 0)
				alloc->l0[w0] &= ~(1ULL << b0);
			continue;
		}
		b2 = __builtin_ctzll(l2_word);

		block = w2_idx * 64 + b2;
		if (block >= alloc->block_count) {
			pr_err("briefs: alloc: block %llu out of range (max=%llu)\n",
				block, alloc->block_count);
			mutex_unlock(&alloc->lock);
			return 0;
		}

		/* Clear the bit */
		alloc->l2[w2_idx] &= ~(1ULL << b2);
		alloc->free_count--;

		/* Propagate upward if word went to zero */
		if (alloc->l2[w2_idx] == 0) {
			alloc->l1[w1_idx] &= ~(1ULL << b1);
			if (alloc->l1[w1_idx] == 0)
				alloc->l0[w0] &= ~(1ULL << b0);
		}

		mutex_unlock(&alloc->lock);
		return block;
	}

	pr_err("briefs: allocator returned 0 despite free_count=%llu\n", alloc->free_count);
	mutex_unlock(&alloc->lock);
	return 0;
}

/*
 * briefs_reserve_block - mark a specific block as allocated in the bitmap.
 * Used during journal replay to ensure bitmap consistency.
 */
void briefs_reserve_block(struct briefs_alloc *alloc, u64 rel_block)
{
	u64 w2, b2, w1, b1, w0, b0;

	mutex_lock(&alloc->lock);
	if (!alloc || !alloc->l0 || rel_block >= alloc->block_count) {
		mutex_unlock(&alloc->lock);
		return;
	}

	w2 = rel_block / 64;
	b2 = rel_block % 64;
	w1 = w2 / 64;
	b1 = w2 % 64;
	w0 = w1 / 64;
	b0 = w1 % 64;

	/* If already allocated (bit already 0), nothing to do */
	if (!(alloc->l2[w2] & (1ULL << b2))) {
		mutex_unlock(&alloc->lock);
		return;
	}

	/* Clear the bit = mark allocated, update free count */
	alloc->l2[w2] &= ~(1ULL << b2);
	alloc->free_count--;

	/* Propagate upward if word becomes all-zero */
	if (alloc->l2[w2] == 0) {
		alloc->l1[w1] &= ~(1ULL << b1);
		if (alloc->l1[w1] == 0)
			alloc->l0[w0] &= ~(1ULL << b0);
	}
	mutex_unlock(&alloc->lock);
}

void briefs_free_block(struct briefs_alloc *alloc, u64 rel_block)
{
	u64 w2, b2, w1, b1, w0, b0;

	mutex_lock(&alloc->lock);
	if (!alloc || !alloc->l0 || rel_block >= alloc->block_count) {
		mutex_unlock(&alloc->lock);
		return;
	}

	w2 = rel_block / 64;
	b2 = rel_block % 64;
	w1 = w2 / 64;
	b1 = w2 % 64;
	w0 = w1 / 64;
	b0 = w1 % 64;

	/* If already free, nothing to do */
	if (alloc->l2[w2] & (1ULL << b2)) {
		mutex_unlock(&alloc->lock);
		return;
	}

	alloc->l2[w2] |= (1ULL << b2);
	alloc->free_count++;

	/* Propagate upward if word was all-zero */
	if (alloc->l2[w2] == (1ULL << b2)) {
		alloc->l1[w1] |= (1ULL << b1);
		if (alloc->l1[w1] == (1ULL << b1))
			alloc->l0[w0] |= (1ULL << b0);
	}
	mutex_unlock(&alloc->lock);
}

/*
 * Compute the on-disk block offset for a given level's nth block.
 */
static u64 alloc_level_block_offset(struct briefs_alloc *alloc, u64 words_per_block,
                                     u64 l0_blocks, u64 l1_blocks, int level, u64 block_idx)
{
	if (level == 0)
		return alloc->alloc_pool_start + 1 + block_idx;
	if (level == 1)
		return alloc->alloc_pool_start + 1 + l0_blocks + block_idx;
	return alloc->alloc_pool_start + 1 + l0_blocks + l1_blocks + block_idx;
}

/*
 * Sync one level of the in-memory bitmap pyramid back to disk.
 * array: level array to sync
 * words: number of u64 words in this level
 * level: 0, 1, or 2
 * Returns 0 on success, -EIO if a block read fails.
 */
static int briefs_alloc_sync_level(struct briefs_alloc *alloc, u64 *array,
                                    u64 words, int level,
                                    u64 words_per_block,
                                    u64 l0_blocks, u64 l1_blocks)
{
	u64 level_blocks = (words + words_per_block - 1) / words_per_block;
	u64 i, j;

	for (i = 0; i < level_blocks; i++) {
		struct buffer_head *bh;
		u64 offset = alloc_level_block_offset(alloc, words_per_block, l0_blocks, l1_blocks, level, i);
		u64 start = i * words_per_block;
		u64 n = min_t(u64, words - start, words_per_block);
		bool dirty = false;

		bh = sb_bread(alloc->sb, offset);
		if (!bh) {
			pr_err("briefs: failed to read L%d block %llu during sync\n",
			       level, offset);
			return -EIO;
		}

		for (j = 0; j < n; j++) {
			if (((u64 *)bh->b_data)[j] != array[start + j]) {
				((u64 *)bh->b_data)[j] = array[start + j];
				dirty = true;
			}
		}

		if (dirty) {
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
		}
		brelse(bh);
	}
	return 0;
}

/*
 * Sync the in-memory bitmap back to disk.
 * Only writes blocks that have changed.
 */
int briefs_alloc_sync(struct briefs_alloc *alloc)
{
	u64 words_per_block;
	u64 l0_blocks, l1_blocks, l2_blocks;
	int ret;

	mutex_lock(&alloc->lock);
	if (!alloc || !alloc->sb || !alloc->l0) {
		mutex_unlock(&alloc->lock);
		return -EINVAL;
	}

	words_per_block = alloc->sb->s_blocksize / sizeof(u64);
	l0_blocks = (alloc->l0_words + words_per_block - 1) / words_per_block;
	l1_blocks = (alloc->l1_words + words_per_block - 1) / words_per_block;
	l2_blocks = (alloc->l2_words + words_per_block - 1) / words_per_block;

	pr_debug("briefs: syncing allocator: %llu+%llu+%llu blocks\n",
		l0_blocks, l1_blocks, l2_blocks);

	/* Sync level 0 */
	ret = briefs_alloc_sync_level(alloc, alloc->l0, alloc->l0_words, 0,
	                               words_per_block, l0_blocks, l1_blocks);
	if (ret) {
		mutex_unlock(&alloc->lock);
		return ret;
	}

	/* Sync level 1 */
	ret = briefs_alloc_sync_level(alloc, alloc->l1, alloc->l1_words, 1,
	                               words_per_block, l0_blocks, l1_blocks);
	if (ret) {
		mutex_unlock(&alloc->lock);
		return ret;
	}

	/* Sync level 2 */
	ret = briefs_alloc_sync_level(alloc, alloc->l2, alloc->l2_words, 2,
	                               words_per_block, l0_blocks, l1_blocks);
	if (ret) {
		mutex_unlock(&alloc->lock);
		return ret;
	}

	/* Update the header block's free_count */
	{
		struct buffer_head *bh = sb_bread(alloc->sb, alloc->alloc_pool_start);
		if (bh) {
			struct alloc_pool_header *hdr = (struct alloc_pool_header *)bh->b_data;
			hdr->free_count = alloc->free_count;
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
		}
	}

	pr_debug("briefs: allocator sync complete\n");
	mutex_unlock(&alloc->lock);
	return 0;
}

/*
 * Cleanup allocator - free vmalloc'd arrays.
 */
void briefs_alloc_cleanup(struct briefs_alloc *alloc)
{
	if (!alloc)
		return;

	vfree(alloc->l0);
	vfree(alloc->l1);
	vfree(alloc->l2);
	alloc->l0 = NULL;
	alloc->l1 = NULL;
	alloc->l2 = NULL;
	alloc->l0_words = 0;
	alloc->l1_words = 0;
	alloc->l2_words = 0;
	alloc->block_count = 0;
	alloc->free_count = 0;
}
