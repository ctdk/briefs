/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_XATTR_H
#define _BRIEFS_XATTR_H

struct dentry;
struct inode;

/*
 * BrieFS extended attributes.  Each inode may own one or more chained external
 * 4096-byte xattr blocks (see briefs.h: briefs_xattr_header /
 * briefs_xattr_entry) referenced by the inode's xattr_offset / xattr_size
 * fields.  Large values are split across continuation blocks linked by
 * briefs_xattr_header.next_block.  The three namespace handlers (user /
 * trusted / security) are published via sb->s_xattr; the VFS resolves get/set
 * by prefix and auto-sets IOP_XATTR on every inode.  listxattr(2) uses the
 * .listxattr inode op (briefs_xattr_list) which reads the on-disk chain.
 */

/* Handlers array (NULL-terminated) for sb->s_xattr. */
extern const struct xattr_handler * const briefs_xattr_handlers[];

/* .listxattr inode-op target: list an inode's xattr names. */
ssize_t briefs_xattr_list(struct dentry *dentry, char *buffer, size_t size);

/* Handler .get/.set targets (name is the full prefixed name). */
int briefs_xattr_get(struct inode *inode, const char *name,
		     void *value, size_t size);
int briefs_xattr_set(struct inode *inode, const char *name,
		     const void *value, size_t size, int flags);

/*
 * Free an inode's xattr block (clears the pointer first, then frees).  Called
 * from briefs_free_inode_data on eviction; no-op when xattr_offset == 0.
 */
void briefs_xattr_free(struct inode *inode);

#endif /* _BRIEFS_XATTR_H */
