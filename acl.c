// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * BrieFS POSIX ACL support.
 *
 * ACLs are stored as ordinary xattrs under the canonical names
 * XATTR_NAME_POSIX_ACL_ACCESS / XATTR_NAME_POSIX_ACL_DEFAULT, using the
 * standard uapi on-disk format (posix_acl_xattr_header/entry).  The 6.12 VFS
 * special-cases these two names in the xattr syscalls (is_posix_acl_xattr())
 * and routes get/set directly to the .get_inode_acl / .set_acl inode ops,
 * bypassing sb->s_xattr -- so no dedicated xattr handler is registered.
 *
 * The .get_inode_acl / .set_acl ops here read/write the ACL through the
 * existing per-inode xattr store (briefs_xattr_get / briefs_xattr_set), which
 * journals + persists the xattr block and the inode automatically.  This
 * mirrors the ext4 xattr-format pattern (ext2 uses a custom compact format;
 * BrieFS, like ext4, stores the raw uapi representation).
 */

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>
#include <linux/xattr.h>

#include "briefs.h"
#include "briefs_xattr.h"

/*
 * .get_inode_acl inode op: read an ACL from the xattr store and decode it.
 *
 * BrieFS xattr reads sleep on buffer I/O under xattr_sem, so the RCU-walk
 * path is not supported -- return -ECHILD to force the caller into refwalk,
 * exactly as ext2/ext4 do.
 */
struct posix_acl *briefs_get_acl(struct inode *inode, int type, bool rcu)
{
	const char *name;
	void *value = NULL;
	struct posix_acl *acl;
	int ret;

	if (rcu)
		return ERR_PTR(-ECHILD);

	name = posix_acl_xattr_name(type);

	ret = briefs_xattr_get(inode, name, NULL, 0);
	if (ret > 0) {
		value = kmalloc(ret, GFP_KERNEL);
		if (!value)
			return ERR_PTR(-ENOMEM);
		ret = briefs_xattr_get(inode, name, value, ret);
	}

	if (ret > 0)
		acl = posix_acl_from_xattr(inode->i_sb->s_user_ns, value, ret);
	else if (ret == -ENODATA || ret == 0)
		acl = NULL;
	else
		acl = ERR_PTR(ret);

	kfree(value);
	return acl;
}

/*
 * Internal set helper shared by briefs_set_acl (.set_acl op) and
 * briefs_init_acl (new-inode ACL inheritance).  Serializes @acl to the uapi
 * xattr format and stores it; passing NULL @acl removes the xattr.  On
 * success the inode's ACL cache is updated so the next permission check can
 * skip a disk read.
 */
static int __briefs_set_acl(struct inode *inode, struct posix_acl *acl,
			    int type)
{
	const char *name;
	void *value = NULL;
	size_t size = 0;
	int error;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name = XATTR_NAME_POSIX_ACL_ACCESS;
		break;

	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		name = XATTR_NAME_POSIX_ACL_DEFAULT;
		break;

	default:
		return -EINVAL;
	}

	if (acl) {
		size = posix_acl_to_xattr(inode->i_sb->s_user_ns, acl, NULL, 0);
		if (size < 0)
			return size;
		value = kmalloc(size, GFP_KERNEL);
		if (!value)
			return -ENOMEM;
		error = posix_acl_to_xattr(inode->i_sb->s_user_ns, acl,
					   value, size);
		if (error < 0) {
			kfree(value);
			return error;
		}
	}

	error = briefs_xattr_set(inode, name, value, size, 0);
	kfree(value);
	/* Removing an ACL that isn't stored is a no-op success, not an error.
	 * This arises when posix_acl_update_mode drops a mode-equivalent access
	 * ACL (it sets *acl=NULL and lets the mode carry the permissions);
	 * briefs_xattr_set then tries to remove an xattr that was never written
	 * and returns -ENODATA.  The removexattr syscall path needs that
	 * -ENODATA surfaced to userspace, but the ACL set op must treat
	 * "remove absent" as success so the caller still caches "no ACL" and
	 * persists the mode.  Without this, every chmod/chacl on a file with no
	 * stored ACL would fail with ENODATA and the mode change would never be
	 * persisted (briefs_xattr_set bails before its persist step). */
	if (error == -ENODATA && !acl)
		error = 0;
	if (!error)
		set_cached_acl(inode, type, acl);
	return error;
}

/*
 * .set_acl inode op.  For ACL_TYPE_ACCESS the ACL may imply a different
 * mode (the mask entry tracks the group class); posix_acl_update_mode
 * computes it and may clone or drop @acl.
 *
 * The new mode is applied to the VFS inode and mirrored to binfo->disk_inode
 * ONLY once the ACL xattr write succeeds.  If the write fails (e.g. -ENOSPC
 * when the xattr block can't be allocated), the mode must stay untouched --
 * generic/449 fills the device with xattrs then expects a failing setfacl to
 * leave the permissions unchanged.  Applying the mode before the write would
 * mutate the in-memory mode even on failure.  Because the mode is mirrored
 * only on success, briefs_xattr_set (inside __briefs_set_acl) persists the
 * inode with the OLD mode; the final mode is persisted here by
 * briefs_inode_sync once the ACL is stored.  For a mode-equivalent ACL that
 * posix_acl_update_mode dropped to NULL, __briefs_set_acl's remove path
 * returns before its persist step, so this sync is also what lands the mode
 * on disk (otherwise it would only be written at unmount writeback).
 */
int briefs_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
		   struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	umode_t mode = inode->i_mode;
	int update_mode = 0;
	int error;

	if (type == ACL_TYPE_ACCESS && acl) {
		error = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (error)
			return error;
		update_mode = 1;
	}

	error = __briefs_set_acl(inode, acl, type);
	if (!error && update_mode) {
		inode->i_mode = mode;
		briefs_sync_inode_fields(inode, &binfo->disk_inode);
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		error = briefs_inode_sync(inode);
	}
	return error;
}

/*
 * Initialize a new inode's ACLs from the parent directory's default ACL.
 * Called from briefs_new_inode after the inode is unlocked but before it is
 * persisted.  posix_acl_create may adjust inode->i_mode (applying the
 * default ACL's mask); the mirror below captures that adjustment so the
 * subsequent persist carries the final mode.  Setting i_acl / i_default_acl
 * to NULL (rather than leaving them ACL_NOT_CACHED) marks "definitely no
 * ACL" and avoids a disk fetch on the first permission check.
 */
int briefs_init_acl(struct inode *inode, struct inode *dir)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct posix_acl *default_acl, *acl;
	int error;

	error = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (error)
		return error;

	briefs_sync_inode_fields(inode, &binfo->disk_inode);

	if (default_acl) {
		error = __briefs_set_acl(inode, default_acl, ACL_TYPE_DEFAULT);
		posix_acl_release(default_acl);
	} else {
		inode->i_default_acl = NULL;
	}
	if (acl) {
		if (!error)
			error = __briefs_set_acl(inode, acl, ACL_TYPE_ACCESS);
		posix_acl_release(acl);
	} else {
		inode->i_acl = NULL;
	}
	return error;
}