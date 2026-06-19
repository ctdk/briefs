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
#include "briefs_debug.h"

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
	return briefs_alloc_init_at(alloc, sb,
		le64_to_cpu(sb_disk->trie_node_pool_start));
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
	if (le32_to_cpu(hdr->magic) != ALLOC_MAGIC) {
		pr_err("briefs: invalid allocator magic: 0x%08x (expected 0x%08x)\n",
			le32_to_cpu(hdr->magic), ALLOC_MAGIC);
		brelse(bh);
		return -EINVAL;
	}

	alloc->l0_words = le64_to_cpu(hdr->l0_words);
	alloc->l1_words = le64_to_cpu(hdr->l1_words);
	alloc->l2_words = le64_to_cpu(hdr->l2_words);
	alloc->block_count = le64_to_cpu(hdr->block_count);
	alloc->free_count = le64_to_cpu(hdr->free_count);

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
			alloc->l0[w + j] = le64_to_cpu(((u64 *)bh->b_data)[j]);
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
			alloc->l1[w + j] = le64_to_cpu(((u64 *)bh->b_data)[j]);
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
			alloc->l2[w + j] = le64_to_cpu(((u64 *)bh->b_data)[j]);
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
	u64 w0, b0, w1_idx, l1_word, b1, w2_idx, l2_word, b2, block, i;
	struct briefs_sb_info *bsi = briefs_sb(alloc->sb);

	/* Count only data-block allocs here, not inode-number allocs (which also
	 * flow through this function via bsi->inode_alloc and are counted in
	 * briefs_alloc_inode). The pointer compare is short-circuited behind the
	 * debug flag, so there is no cost when -o debug is off. */
	if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG) && alloc == &bsi->alloc)
		atomic64_inc(&bsi->stats.data_alloc_calls);

	mutex_lock(&alloc->lock);
	if (!alloc || alloc->free_count == 0 || !alloc->l0) {
		mutex_unlock(&alloc->lock);
		return 0;
	}

	/* Next-fit: start the L0 scan at the rover and wrap around, so a
	 * mostly-allocated device doesn't rescans exhausted low L0 words every
	 * call.  The wrap visits every L0 word exactly once, so a free block is
	 * found iff one exists (coverage identical to scanning from 0). */
	for (i = 0; i < alloc->l0_words; i++) {
		w0 = (alloc->rover_w0 + i) % alloc->l0_words;
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

		alloc->rover_w0 = w0;
		mutex_unlock(&alloc->lock);
		return block;
	}

	pr_err("briefs: allocator returned 0 despite free_count=%llu\n", alloc->free_count);
	mutex_unlock(&alloc->lock);
	return 0;
}

/*
 * Clear the L1/L0 summary bits for a single L2 word that just became all-zero.
 * Caller holds alloc->lock and has already verified alloc->l2[w2] == 0.
 * (briefs_alloc_block and briefs_reserve_block keep their own inline copies of
 * this propagation to avoid touching the hot single-block path; this helper is
 * used by briefs_alloc_blocks, which clears a run and may zero several words.)
 */
static void briefs_propagate_l2_zero(struct briefs_alloc *alloc, u64 w2)
{
	u64 w1 = w2 / 64, b1 = w2 % 64, w0, b0;

	alloc->l1[w1] &= ~(1ULL << b1);
	if (alloc->l1[w1] == 0) {
		w0 = w1 / 64;
		b0 = w1 % 64;
		alloc->l0[w0] &= ~(1ULL << b0);
	}
}

/*
 * briefs_alloc_blocks - allocate a contiguous run of @n free blocks under one
 * alloc->lock.  Returns the starting data-relative block, or 0 on ENOSPC / no
 * contiguous run of length @n / @n == 0.  Block 0 is the failure sentinel
 * (inherited from briefs_alloc_block's convention; not fixed here).
 *
 * First-fit from the start of the bitmap: scan L2 words for a maximal run of
 * set (free) bits at least @n long, possibly spanning word boundaries.  The
 * last L2 word is masked to block_count bits so a run cannot run past the end.
 */
u64 briefs_alloc_blocks(struct briefs_alloc *alloc, u64 n)
{
	u64 w2, run_start = 0, run_len = 0, i;

	if (!alloc || !alloc->l0 || n == 0)
		return 0;

	{
		struct briefs_sb_info *bsi = briefs_sb(alloc->sb);
		if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG) && alloc == &bsi->alloc)
			atomic64_inc(&bsi->stats.data_alloc_calls);
	}

	mutex_lock(&alloc->lock);
	if (n > alloc->free_count || n > alloc->block_count) {
		mutex_unlock(&alloc->lock);
		return 0;
	}

	for (w2 = 0; w2 < alloc->l2_words; w2++) {
		u64 word = alloc->l2[w2];
		u64 base = w2 * 64;
		u64 bits, b, s, cnt;

		/* mask trailing bits beyond block_count in the last word */
		if (w2 == alloc->l2_words - 1) {
			u64 rem = alloc->block_count % 64;
			if (rem != 0)
				word &= (1ULL << rem) - 1;
		}

		if (word == 0) {
			run_len = 0;
			continue;
		}

		/* walk each maximal run of set bits within this word */
		bits = word;
		while (bits) {
			u64 shifted, inv;
			b = __builtin_ctzll(bits);
			s = base + b;
			/* count consecutive set bits from b within this word;
			 * ~0 when the run reaches bit 63 would make ctzll(0) UB,
			 * so fall back to (64 - b) in that all-ones case. */
			shifted = bits >> b;
			inv = ~shifted;
			cnt = inv ? __builtin_ctzll(inv) : (64 - b);
			if (run_len > 0 && s == run_start + run_len)
				run_len += cnt;		/* contiguous with prev word's run */
			else {
				run_start = s;
				run_len = cnt;
			}
			if (run_len >= n)
				goto found;
			/* Clear the consumed run bits.  When cnt == 64 the whole
			 * word is one run from bit b; bits below b are already 0
			 * (b = ctzll(bits)), so clearing the entire word is
			 * equivalent and avoids the (1ULL << 64) UB that would
			 * otherwise leave `bits' unchanged and spin forever. */
			if (cnt >= 64)
				bits = 0;
			else
				bits &= ~(((1ULL << cnt) - 1) << b);
		}
	}

	/* no contiguous run of length n */
	mutex_unlock(&alloc->lock);
	return 0;

found:
	/* clear the n bits */
	for (i = 0; i < n; i++) {
		u64 blk = run_start + i;
		alloc->l2[blk / 64] &= ~(1ULL << (blk % 64));
	}
	alloc->free_count -= n;

	/* propagate L2 -> L1 -> L0 for every L2 word that became all-zero */
	{
		u64 w2_first = run_start / 64;
		u64 w2_last = (run_start + n - 1) / 64;
		for (w2 = w2_first; w2 <= w2_last; w2++)
			if (alloc->l2[w2] == 0)
				briefs_propagate_l2_zero(alloc, w2);
	}

	alloc->rover_w0 = run_start / (64 * 64 * 64);
	mutex_unlock(&alloc->lock);
	return run_start;
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
	struct briefs_sb_info *bsi = briefs_sb(alloc->sb);

	/* See briefs_alloc_block: count data frees only (inode frees are counted
	 * in briefs_free_inode_num). No cost when -o debug is off. */
	if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG) && alloc == &bsi->alloc)
		atomic64_inc(&bsi->stats.data_free_calls);

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
 * briefs_free_blocks - free a contiguous run of @n blocks starting at
 * @rel_start (data-relative) under one alloc->lock acquisition.  The mirror of
 * briefs_alloc_blocks: sets the L2 bits, bumps free_count, and propagates L1/L0
 * upward for every L2 word that transitioned from fully-allocated (0) to having
 * at least one free bit.  Idempotent like briefs_free_block: a bit already free
 * is left untouched and not counted.  The run is clamped to [0, block_count), so
 * a corrupt extent pointing past the end frees only the in-range portion instead
 * of looping forever (the per-block path's 1M-cap-to-1 guard existed only to
 * bound that loop; this bulk path has no loop to bound).
 */
void briefs_free_blocks(struct briefs_alloc *alloc, u64 rel_start, u64 n)
{
	u64 end, w2, w2_first, w2_last, freed = 0;

	if (!alloc || !alloc->l0 || n == 0)
		return;

	{
		struct briefs_sb_info *bsi = briefs_sb(alloc->sb);
		if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG) && alloc == &bsi->alloc)
			atomic64_inc(&bsi->stats.data_free_calls);
	}

	mutex_lock(&alloc->lock);

	if (rel_start >= alloc->block_count) {
		mutex_unlock(&alloc->lock);
		return;
	}
	end = rel_start + n;
	if (end > alloc->block_count || end < rel_start)	/* end < rel_start: overflow */
		end = alloc->block_count;
	if (end <= rel_start) {
		mutex_unlock(&alloc->lock);
		return;
	}

	w2_first = rel_start / 64;
	w2_last = (end - 1) / 64;

	for (w2 = w2_first; w2 <= w2_last; w2++) {
		u64 lo = w2 * 64;		/* first block in this word */
		u64 bit_lo = rel_start > lo ? rel_start - lo : 0;
		u64 bit_hi = end - lo;		/* exclusive; <= 64 */
		u64 mask, before;

		/* mask of bits [bit_lo, bit_hi) within this word */
		if (bit_hi >= 64)
			mask = ~0ULL << bit_lo;
		else
			mask = ((1ULL << bit_hi) - 1) & (~0ULL << bit_lo);

		before = alloc->l2[w2];

		/* count only blocks that were actually allocated (bit was 0) */
		freed += hweight64(~before & mask);

		alloc->l2[w2] |= mask;

		/* propagate upward iff this word was fully allocated and is now
		 * not (mirrors briefs_free_block's == (1<<b2) check generalized
		 * to a multi-bit set). */
		if (before == 0 && alloc->l2[w2] != 0) {
			u64 w1 = w2 / 64, b1 = w2 % 64;
			u64 w0 = w1 / 64, b0 = w1 % 64;
			alloc->l1[w1] |= (1ULL << b1);
			if (alloc->l1[w1] == (1ULL << b1))
				alloc->l0[w0] |= (1ULL << b0);
		}
	}

	alloc->free_count += freed;
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
			__le64 *disk_word = (__le64 *)bh->b_data + j;
			__u64 cur = le64_to_cpu(*disk_word);

			if (cur != array[start + j]) {
				*disk_word = cpu_to_le64(array[start + j]);
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
			hdr->free_count = cpu_to_le64(alloc->free_count);
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
 * Recompute L1/L0 summary levels and free_count from the L2 leaf bitmap.
 * Used after journal replay to ensure allocator headers are consistent with
 * the replayed leaf bitmap state.
 */
void briefs_alloc_recompute_summaries(struct briefs_alloc *alloc)
{
	u64 i;
	u64 l2_valid_bits;

	mutex_lock(&alloc->lock);
	if (!alloc || !alloc->l0 || !alloc->l1 || !alloc->l2) {
		mutex_unlock(&alloc->lock);
		return;
	}

	/* Clear summary levels; they will be rebuilt from L2. */
	memset(alloc->l0, 0, alloc->l0_words * sizeof(u64));
	memset(alloc->l1, 0, alloc->l1_words * sizeof(u64));

	/*
	 * Mask trailing bits in the last L2 word that are beyond block_count.
	 * These bits are outside the tracked range and must not be counted.
	 */
	l2_valid_bits = alloc->block_count % 64;
	if (l2_valid_bits == 0)
		l2_valid_bits = 64;
	if (alloc->l2_words > 0) {
		u64 mask = (l2_valid_bits == 64) ? ~0ULL : ((1ULL << l2_valid_bits) - 1);
		alloc->l2[alloc->l2_words - 1] &= mask;
	}

	alloc->free_count = 0;

	/* Rebuild L1/L0 and count free bits. */
	for (i = 0; i < alloc->l2_words; i++) {
		u64 word = alloc->l2[i];
		u64 l1_idx;

		alloc->free_count += hweight64(word);

		if (word == 0)
			continue;

		l1_idx = i / 64;
		if (l1_idx < alloc->l1_words)
			alloc->l1[l1_idx] |= (1ULL << (i % 64));
	}

	/* Rebuild L0 from L1. */
	for (i = 0; i < alloc->l1_words; i++) {
		u64 l0_idx;

		if (alloc->l1[i] == 0)
			continue;

		l0_idx = i / 64;
		if (l0_idx < alloc->l0_words)
			alloc->l0[l0_idx] |= (1ULL << (i % 64));
	}

	pr_debug("briefs: allocator recomputed: %llu free blocks\n", alloc->free_count);
	mutex_unlock(&alloc->lock);
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
