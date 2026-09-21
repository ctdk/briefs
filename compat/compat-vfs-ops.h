/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_VFS_OPS_H
#define _BRIEFS_COMPAT_VFS_OPS_H

/*
 * compat/compat-vfs-ops.h - i_op->create / i_op->mkdir table entries.
 *
 * v6.15 changed i_op->mkdir to return struct dentry * (ERR_PTR(err) on
 * failure, NULL on success); v7.3 dropped i_op->create's bool excl (the
 * VFS performs the EEXIST check before ->create, so BrieFS never used
 * it).  BrieFS adopts the newest prototypes and this header adapts
 * them back for older kernels, so dir.c and briefs.h are written once
 * in the new shape and the ops tables simply reference:
 *
 *	.create = briefs_create_op,
 *	.mkdir  = briefs_mkdir_op,
 *
 * The forward declarations below must stay identical to the prototypes
 * in briefs.h; both are visible in every translation unit, so the
 * compiler rejects any drift between them.
 */

#include <linux/fs.h>

int briefs_create(struct mnt_idmap *idmap, struct inode *dir,
		  struct dentry *dentry, umode_t mode);
struct dentry *briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
			    struct dentry *dentry, umode_t mode);

#if BRIEFS_HAS_CREATE_NO_EXCL
#define briefs_create_op briefs_create
#else
static inline int briefs_create_op(struct mnt_idmap *idmap,
				   struct inode *dir, struct dentry *dentry,
				   umode_t mode, bool __always_unused excl)
{
	return briefs_create(idmap, dir, dentry, mode);
}
#endif

#if BRIEFS_HAS_MKDIR_DENTRY
#define briefs_mkdir_op briefs_mkdir
#else
static inline int briefs_mkdir_op(struct mnt_idmap *idmap,
				  struct inode *dir, struct dentry *dentry,
				  umode_t mode)
{
	struct dentry *d = briefs_mkdir(idmap, dir, dentry, mode);

	return d ? PTR_ERR(d) : 0;
}
#endif

#endif /* _BRIEFS_COMPAT_VFS_OPS_H */