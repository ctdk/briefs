/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_JOURNAL_H
#define _BRIEFS_JOURNAL_H

#include "briefs.h"
#include <linux/blkdev.h>
#include <linux/mutex.h>

/* Journal constants */
#define JOURNAL_BLOCK_SIZE 4096

/* Journal context */
struct briefs_journal {
	struct briefs_superblock *sb;     /* on-disk superblock pointer */
	struct super_block *vfs_sb;       /* VFS super_block for buffer cache I/O */
	unsigned char *cur_block;         /* current journal block buffer (4096 bytes) */
	struct journal_block_header *cur_hdr; /* cast to block header */
	u64 write_offset;                 /* byte offset for next record in cur_block */
	u64 write_pos;                    /* current journal block number on disk */
	u64 synced_pos;                   /* blocks before this (since last sync) are durably on disk */
	u64 journal_start;                /* first journal block */
	u64 journal_end;                  /* last journal block */
	u64 checkpoint_block;             /* checkpoint area (last journal block) */
	u64 checkpoint_seq;               /* current checkpoint sequence */
	u32 records_since_checkpoint;      /* records written since last checkpoint */
	bool dirty;                       /* has uncommitted changes */
	/*
	 * Serializes all journal writers.  briefs_journal_write_record(),
	 * _sync() and _checkpoint() mutate shared ring state (write_pos,
	 * write_offset, cur_block, journal_log_start, records_since_checkpoint)
	 * that has no other protection: concurrent syscalls from separate
	 * directories (no shared VFS dir i_rwsem) used to race on write_pos and
	 * all hit the back-pressure checkpoint at once, each resetting
	 * log_start=log_end=write_pos while others advanced write_pos past it ->
	 * livelock (CPU-burning, no progress).  The lock is taken once at the
	 * outermost public entry; internal call chains (write_record ->
	 * checkpoint -> sync -> periodic checkpoint) use the __*_locked()
	 * helpers so the mutex is never re-acquired.  All callers are in
	 * sleepable context (the journal paths only ever read
	 * bsi->alloc.free_count, never acquire alloc->lock), so the lock order
	 * alloc->lock -> write_lock is never inverted.  Critical sections sleep
	 * (sync_dirty_buffer, sb_bread, GFP_KERNEL), so this is a mutex, not a
	 * spinlock.
	 */
	struct mutex write_lock;

	/*
	 * Replay context.  Set only while briefs_journal_replay() is re-deriving
	 * directory tries (pass-1 reservation + pass-2 apply), i.e. single-threaded
	 * at mount before the fs is writable.  Gating alloc/replay behaviour on
	 * this flag keeps the live (post-mount) hot path untouched.
	 */
	bool in_replay;

	/*
	 * Pool of data-relative block numbers collected from JRN_TRIE_ALLOC records
	 * during replay pass-1 (journal order: [log_start,S] synced blocks first,
	 * (S,log_end] unsynced blocks last).  Pass-2's briefs_trie_page_init() pops
	 * from here instead of calling briefs_alloc_block(), so re-derivation reuses
	 * the blocks the journal already recorded as trie pages rather than
	 * re-allocating fresh ones (which -ENOSPC's on a full fs -- generic/475).
	 *
	 * Consumed LIFO (tail-first): the unsynced-tail blocks are ORPHANS (page
	 * content synced by briefs_trie_page_init() but parent-slot link not, so
	 * unreferenced by the on-disk trie) and land last in journal order, so
	 * popping the tail reuses orphans.  Re-derivation only page_inits for
	 * unsynced records (synced ones hit -EEXIST), so it must reuse orphans, not
	 * the referenced [log_start,S] blocks (overwriting a referenced block would
	 * clobber a -EEXIST record's page).  Seeding + unbounded scan keep the
	 * page_init count M <= the orphan count, so we stop before reaching
	 * referenced blocks; if M exceeds that the pool empties and page_init falls
	 * back to briefs_alloc_block(), which can only pick non-referenced free
	 * blocks.  Block NUMBER need not match the live path's assignment because
	 * each re-derived trie is self-consistent (parent child-pointers reference
	 * whatever block page_init returned).  Unpopped entries are trie pages
	 * already on disk (correctly left reserved/allocated); only the list nodes
	 * are freed after pass-2.
	 */
	struct list_head replay_trie_blocks;
};

/*
 * One entry in the replay trie-block pool (above).  Lives only during replay.
 */
struct briefs_replay_trie_block {
	struct list_head list;
	u64 rel;		/* data-relative block number */
};

/*
 * Checkpoint threshold — perform a checkpoint after this many records.
 * Bumped from 64 to 1024 to reduce synchronous flush overhead during
 * metadata-heavy workloads.
 */
#define JRN_CHECKPOINT_INTERVAL 1024

/* Initialize journal from superblock */
int briefs_journal_init(struct briefs_journal *j, struct briefs_superblock *sb);

/* Read a journal block from disk (returns buffer_head, caller must brelse) */
struct buffer_head *briefs_journal_read_block(struct briefs_journal *j, u64 block_offset);

/* Write a journal block to disk */
int briefs_journal_write_block(struct briefs_journal *j, u64 block_offset, unsigned char *data);

/* Write a record to the journal */
int briefs_journal_write_record(struct briefs_journal *j, enum journal_record_type type,
                                 void *data, u32 data_len);

/* Write a checkpoint */
int briefs_journal_checkpoint(struct briefs_journal *j);

/* Mount/recovery: replay journal from last checkpoint */
int briefs_journal_replay(struct briefs_journal *j);

/*
 * Pop the next data-relative trie-block number from the replay pool (LIFO:
 * unsynced-tail orphans first).  Returns 0 and sets *@rel on success, -ENOENT
 * if the pool is empty (caller falls back to briefs_alloc_block).  Only
 * meaningful while in_replay is set.
 */
int briefs_journal_replay_pop_trie_block(struct briefs_journal *j, u64 *rel);

/* Cleanup journal */
void briefs_journal_cleanup(struct briefs_journal *j);

/* Open journal */
int briefs_journal_open(struct briefs_journal *j, struct briefs_superblock *sb, struct super_block *vfs_sb);

/* Sync dirty journal block to disk */
int briefs_journal_sync(struct briefs_journal *j);

/* Get next journal block */
u64 briefs_journal_next_block(struct briefs_journal *j, u64 cur);

/* Get previous journal block (for wrapping) */
u64 briefs_journal_prev_block(struct briefs_journal *j, u64 cur);

/* Write directory update to journal */
int briefs_journal_dir_update(struct briefs_journal *j, u64 parent_ino, u64 child_ino,
                              const char *name, size_t name_len, u8 op, u8 ftype);

/* Convenience wrappers for directory add/delete using a dentry */
static inline int briefs_journal_dir_add(struct briefs_journal *j, u64 parent_ino,
                                          u64 child_ino, struct dentry *dentry, u8 ftype)
{
	return briefs_journal_dir_update(j, parent_ino, child_ino,
					dentry->d_name.name, dentry->d_name.len, 0, ftype);
}

static inline int briefs_journal_dir_del(struct briefs_journal *j, u64 parent_ino,
                                          struct dentry *dentry)
{
	return briefs_journal_dir_update(j, parent_ino, 0,
					dentry->d_name.name, dentry->d_name.len, 1, 0);
}

/* Log a newly allocated inode */
int briefs_journal_inode_alloc(struct briefs_journal *j, u64 ino,
                                umode_t mode, u32 nlink);

/* Log freeing of an inode */
int briefs_journal_inode_free(struct briefs_journal *j, u64 ino);

/* Log extent allocation */
int briefs_journal_extent_alloc(struct briefs_journal *j, u64 ino,
				 u64 offset, u64 phys_start,
				 u64 length, int extent_index);

/* Log extent free */
int briefs_journal_extent_free(struct briefs_journal *j, u64 ino,
			       u64 offset, u64 phys_start, u64 length);

/* Log inode metadata update (nlink, mode, size, timestamps) */
int briefs_journal_inode_update(struct briefs_journal *j,
				struct inode *inode);

/* Log allocation/free of a packed directory-trie page */
int briefs_journal_trie_alloc(struct briefs_journal *j, u64 abs_block);
int briefs_journal_trie_free(struct briefs_journal *j, u64 abs_block);

/* Log a complete 512-byte on-disk inode snapshot */
int briefs_journal_inode_full(struct briefs_journal *j, u64 ino,
                              const struct briefs_disk_inode *di);

/* Log symlink target content inline */
int briefs_journal_symlink_data(struct briefs_journal *j, u64 ino,
                                u64 phys, const char *target,
                                size_t target_len);

/* Persist in-memory superblock fields back to the on-disk superblock */
int briefs_journal_sync_superblock(struct briefs_journal *j);

#endif /* _BRIEFS_JOURNAL_H */
