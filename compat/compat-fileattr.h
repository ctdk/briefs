/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_FILEATTR_H
#define _BRIEFS_COMPAT_FILEATTR_H

/*
 * compat/compat-fileattr.h - struct fileattr / struct file_kattr.
 *
 * v6.17 renamed struct fileattr to struct file_kattr tree-wide.  The
 * field set and the fileattr_fill_flags()/fileattr_fill_xflags()
 * helpers are identical across the rename (only the struct tag moved),
 * so a typedef is the whole compatibility story: BrieFS declarations
 * and definitions use briefs_fileattr and the bodies stay untouched.
 */

#include <linux/types.h>
#include <uapi/linux/fs.h>

/* Forward declarations for the prototypes in <linux/fileattr.h>, which
 * expects its includer to have <linux/fs.h> already (not guaranteed in
 * force-include position). */
struct dentry;
struct file;
struct mnt_idmap;

#include <linux/fileattr.h>

#if BRIEFS_HAS_FILE_KATTR
typedef struct file_kattr briefs_fileattr;
#else
typedef struct fileattr briefs_fileattr;
#endif

#endif /* _BRIEFS_COMPAT_FILEATTR_H */