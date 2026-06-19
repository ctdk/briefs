/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_DEBUG_H
#define _BRIEFS_DEBUG_H

#include "briefs.h"

/* Global list of live per-sb info structs; iterated by /proc/fs/briefs/mounts.
 * Defined in briefs_debug.c. Manipulated under briefs_sb_list_lock. */
extern struct list_head briefs_sb_list;
extern spinlock_t briefs_sb_list_lock;

/*
 * Increment a per-sb stat counter. A single skipped branch on non-debug
 * mounts (BRIEFS_MF_DEBUG unset); the atomic op only runs under -o debug.
 * @bsi may be NULL (callers that only have a super_block can pass
 * briefs_sb(sb), which is NULL before fill_super sets s_fs_info).
 */
#define briefs_stat_inc(bsi, field) do {				\
	struct briefs_sb_info *_bsi = (bsi);				\
	if (_bsi && (_bsi->mount_flags & BRIEFS_MF_DEBUG))		\
		atomic64_inc(&_bsi->stats.field);			\
} while (0)

#endif /* _BRIEFS_DEBUG_H */