/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_BUFFER_H
#define _BRIEFS_COMPAT_BUFFER_H

/*
 * compat/compat-buffer.h - synchronous metadata buffer submission.
 *
 * v7.2 converted buffer_head I/O submission to bios: submit_bh() and
 * end_buffer_write_sync() are gone, replaced by bh_submit(bh, opf,
 * end_io) and bh_end_write(struct bio *).  The completion semantics
 * BrieFS relies on are unchanged on both sides (BH_Write_EIO is set
 * via mark_buffer_write_io_error() on a lost write, uptodate is
 * cleared, the buffer is unlocked), so the metadata write-error
 * quiesce (briefs_check_meta_write_error) and the wait stage of the
 * meta batch work identically on all kernels.
 *
 * One asymmetry is absorbed here: the old end_buffer_write_sync() ended
 * with put_bh(), while bh_end_write() does not (the bio owns the
 * reference).  On < 7.2 the caller-style submission needs the extra
 * get_bh() that __sync_dirty_buffer() takes; on >= 7.2 taking it would
 * LEAK one reference per metadata write (pinned buffers, eviction
 * failures).  The caller-transferred reference survives to the batch
 * wait stage's brelse() on every kernel either way:
 *
 *   < 7.2:  caller ref R1 + wrapper ref R2, completion drops R2,
 *           wait stage drops R1.
 *   >= 7.2: caller ref R1 only, completion drops nothing (the bio's
 *           reference dies with the bio), wait stage drops R1.
 */

#include <linux/buffer_head.h>
#include <linux/blk_types.h>

static inline void briefs_compat_bh_write_submit(struct buffer_head *bh)
{
#if BRIEFS_HAS_BH_SUBMIT
	bh_submit(bh, REQ_OP_WRITE | REQ_SYNC, bh_end_write);
#else
	get_bh(bh);
	bh->b_end_io = end_buffer_write_sync;
	submit_bh(REQ_OP_WRITE | REQ_SYNC, bh);
#endif
}

#endif /* _BRIEFS_COMPAT_BUFFER_H */