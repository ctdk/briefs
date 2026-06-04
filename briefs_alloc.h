/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_ALLOC_H
#define _BRIEFS_ALLOC_H

#include "briefs.h"

/* Allocator context - tracks the in-memory trie state */
struct briefs_alloc {
	struct briefs_superblock *sb;   /* on-disk superblock pointer */
	struct trie_node *root_node;    /* in-memory copy of trie root */
};

/* Initialize allocator from superblock */
int briefs_alloc_init(struct briefs_alloc *alloc, struct super_block *sb, struct briefs_superblock *sb_disk);

/* Find and allocate N contiguous free blocks */
u64 briefs_alloc_contiguous(struct briefs_alloc *alloc, u64 nblocks);

/* Free N blocks starting at physical block offset */
void briefs_free_contiguous(struct briefs_alloc *alloc, u64 phys_block, u64 nblocks);

/* Allocate a single free block */
u64 briefs_alloc_block(struct briefs_alloc *alloc);

/* Free a single block */
void briefs_free_block(struct briefs_alloc *alloc, u64 phys_block);

/* Cleanup allocator */
void briefs_alloc_cleanup(struct briefs_alloc *alloc);

/* Find leftmost contiguous range of free blocks starting from node */
u64 briefs_find_leftmost_contiguous(struct trie_node *node, u64 needed, u64 *out_len);

/* Count trailing free blocks in a node (for spanning check) */
u64 briefs_count_trailing_free(struct trie_node *node);

/* Count leading free blocks in a node (for spanning check) */
u64 briefs_count_leading_free(struct trie_node *node);

/* Mark a range as allocated in the trie */
void briefs_mark_allocated(struct trie_node *node, u64 offset, u64 count);

/* Mark a range as free in the trie */
void briefs_mark_free(struct trie_node *node, u64 offset, u64 count);

/* Rebuild free_count from children (after allocation/deallocation) */
void briefs_recount(struct trie_node *node);

/* Check if a range is entirely free */
bool briefs_range_is_free(struct trie_node *node, u64 offset, u64 count);

#endif /* _BRIEFS_ALLOC_H */
