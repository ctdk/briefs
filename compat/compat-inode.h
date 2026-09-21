/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_INODE_H
#define _BRIEFS_COMPAT_INODE_H

#include <linux/fs.h>

/*
 * 6.19 wrapped struct inode::i_state in struct inode_state_flags so that
 * plain bit tests ("inode->i_state & I_NEW") fail to compile; reads go
 * through inode_state_read()/inode_state_read_once() (the _once form is
 * the READ_ONCE variant without the i_lock assertion).  BrieFS only ever
 * reads I_NEW right after iget5_locked() returns, a lockless read, so
 * the _once helper is the exact match.  On older kernels the wrapper is
 * the plain load of the old unsigned long bitfield.
 */
static inline unsigned long briefs_compat_inode_state(struct inode *inode)
{
#if BRIEFS_HAS_INODE_STATE_HELPERS
	return inode_state_read_once(inode);
#else
	return inode->i_state;
#endif
}

#endif /* _BRIEFS_COMPAT_INODE_H */