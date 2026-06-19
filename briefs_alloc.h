/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_ALLOC_H
#define _BRIEFS_ALLOC_H

#include <linux/types.h>
#include <linux/mutex.h>

/* Forward declarations - defined in briefs.h */
struct briefs_superblock;
struct super_block;

/* Allocator pool header (first block of the pool) */
#define ALLOC_MAGIC 0x4249544D /* "BITM" */

struct alloc_pool_header {
	__le32 magic;          /* ALLOC_MAGIC */
	__le32 version;        /* 1 */
	__le64 l0_words;       /* number of u64 words in level 0 */
	__le64 l1_words;       /* number of u64 words in level 1 */
	__le64 l2_words;       /* number of u64 words in level 2 */
	__le64 block_count;    /* total data blocks tracked */
	__le64 free_count;     /* total free blocks */
};

/* Allocator context - 3-level bitmap pyramid.
 * Used for both data block allocation and inode allocation. */
struct briefs_alloc {
	struct super_block *sb;         /* VFS superblock for buffer cache I/O */
	__u64 *l0;                      /* level 0 summary words (vmalloc'd) */
	__u64 *l1;                      /* level 1 summary words */
	__u64 *l2;                      /* level 2 block bitmaps */
	__u64 l0_words;                 /* word count for level 0 */
	__u64 l1_words;                 /* word count for level 1 */
	__u64 l2_words;                 /* word count for level 2 */
	__u64 block_count;              /* total data blocks tracked */
	__u64 free_count;               /* total free blocks */
	u64 alloc_pool_start;           /* first block of allocator pool on disk */
	u64 rover_w0;                   /* next-fit hint: L0 word to start the next
					 * single-block scan at (in-memory only,
					 * not persisted; wraps around) */
	struct mutex lock;               /* protects bitmaps and free_count */
};

/* Initialize data block allocator from superblock */
/* Initialize allocator from an explicit pool start offset (used for inode allocator) */
int briefs_alloc_init_at(struct briefs_alloc *alloc, struct super_block *sb,
                          u64 pool_block);

int briefs_alloc_init(struct briefs_alloc *alloc, struct super_block *sb,
                      struct briefs_superblock *sb_disk);

/* Allocate a single free block (returns data-relative block number, or 0 on ENOSPC) */
u64 briefs_alloc_block(struct briefs_alloc *alloc);

/*
 * Allocate a contiguous run of @n free data blocks under one alloc->lock
 * acquisition.  Returns the starting data-relative block number, or 0 on
 * ENOSPC / no contiguous run of length @n / @n == 0.  Block 0 is the failure
 * sentinel (same convention as briefs_alloc_block); a run starting at block 0
 * cannot be distinguished from failure.  Updates the rover.
 */
u64 briefs_alloc_blocks(struct briefs_alloc *alloc, u64 n);

/*
 * Reserve a specific block (mark it as allocated without searching).
 * Used during journal replay to ensure bitmap matches recorded allocations.
 */
void briefs_reserve_block(struct briefs_alloc *alloc, u64 rel_block);

/* Free a single block (data-relative block number) */
void briefs_free_block(struct briefs_alloc *alloc, u64 rel_block);

/*
 * Free a contiguous run of @n blocks starting at @rel_start (data-relative)
 * under one alloc->lock acquisition — the mirror of briefs_alloc_blocks.
 * Idempotent (already-free bits are not re-counted); the run is clamped to
 * [0, block_count).  Used by briefs_free_blocks_range.
 */
void briefs_free_blocks(struct briefs_alloc *alloc, u64 rel_start, u64 n);

/* Sync the in-memory bitmap back to disk */
int briefs_alloc_sync(struct briefs_alloc *alloc);

/*
 * Recompute L1/L0 summary levels and free_count from the L2 leaf bitmap.
 * Used after journal replay to ensure on-disk allocator headers are consistent
 * with the replayed bitmap state.
 */
void briefs_alloc_recompute_summaries(struct briefs_alloc *alloc);

/* Cleanup allocator (free vmalloc'd arrays) */
void briefs_alloc_cleanup(struct briefs_alloc *alloc);

/* Compute allocator level word counts for a given number of tracked entries */
static inline void alloc_compute_levels(u64 data_blocks, u64 *l0w, u64 *l1w, u64 *l2w)
{
	u64 l2 = (data_blocks + 63) / 64;
	u64 l1 = (l2 + 63) / 64;
	u64 l0 = (l1 + 63) / 64;
	if (l0 < 1) l0 = 1;
	if (l1 < 1) l1 = 1;
	if (l2 < 1) l2 = 1;
	if (l0w) *l0w = l0;
	if (l1w) *l1w = l1;
	if (l2w) *l2w = l2;
}


/* Compute total pool blocks: 1 header + ceil(words/512) per level */
static inline u64 alloc_pool_blocks(u64 l0w, u64 l1w, u64 l2w)
{
	u64 wpb = 512;
	u64 blk = 1;
	blk += (l0w + wpb - 1) / wpb;
	blk += (l1w + wpb - 1) / wpb;
	blk += (l2w + wpb - 1) / wpb;
	return blk;
}
#endif /* _BRIEFS_ALLOC_H */