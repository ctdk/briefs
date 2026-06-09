/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_JOURNAL_H
#define _BRIEFS_JOURNAL_H

#include "briefs.h"
#include <linux/blkdev.h>

/* Journal constants */
#define JOURNAL_BLOCK_SIZE 4096
#define JRN_RECORD_MAX_SIZE 320  /* max record size (JRN_DIR_UPDATE) */

/* Journal context */
struct briefs_journal {
	struct briefs_superblock *sb;     /* on-disk superblock pointer */
	struct super_block *vfs_sb;       /* VFS super_block for buffer cache I/O */
	unsigned char *cur_block;         /* current journal block buffer (4096 bytes) */
	struct journal_block_header *cur_hdr; /* cast to block header */
	u64 write_offset;                 /* byte offset for next record in cur_block */
	u64 write_pos;                    /* current journal block number on disk */
	u64 journal_start;                /* first journal block */
	u64 journal_end;                  /* last journal block */
	u64 checkpoint_block;             /* checkpoint area (last journal block) */
	u64 checkpoint_seq;               /* current checkpoint sequence */
	u32 records_since_checkpoint;      /* records written since last checkpoint */
	bool dirty;                       /* has uncommitted changes */
};

/* Checkpoint threshold — perform a checkpoint after this many records */
#define JRN_CHECKPOINT_INTERVAL 64

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
                              const char *name, size_t name_len, u8 op);

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

#endif /* _BRIEFS_JOURNAL_H */
