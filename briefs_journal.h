/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_JOURNAL_H
#define _BRIEFS_JOURNAL_H

#include "briefs.h"

/* Journal context */
struct briefs_journal {
	struct briefs_superblock *sb;     /* superblock pointer */
	struct journal_block *cur_block;  /* current journal block buffer */
	__u64 write_pos;                  /* current write position in journal */
	__u64 journal_start;              /* first journal block */
	__u64 journal_end;                /* last journal block */
	__u64 checkpoint_block;           /* checkpoint area (last journal block) */
	__u64 checkpoint_seq;             /* current checkpoint sequence */
	bool dirty;                       /* has uncommitted changes */
};

/* Initialize journal from superblock */
int briefs_journal_init(struct briefs_journal *j, struct briefs_superblock *sb);

/* Write a record to the journal */
int briefs_journal_write_record(struct briefs_journal *j, enum journal_record_type type,
                                 void *data, __u32 data_len);

/* Write a checkpoint */
int briefs_journal_checkpoint(struct briefs_journal *j);

/* Mount/recovery: replay journal from last checkpoint */
int briefs_journal_replay(struct briefs_journal *j);

/* Cleanup journal */
void briefs_journal_cleanup(struct briefs_journal *j);

/* Get next journal block */
__u64 briefs_journal_next_block(struct briefs_journal *j, __u64 cur);

/* Get previous journal block (for wrapping) */
__u64 briefs_journal_prev_block(struct briefs_journal *j, __u64 cur);

#endif /* _BRIEFS_JOURNAL_H */
