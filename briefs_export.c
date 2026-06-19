// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * BrieFS NFS export operations.
 *
 * BrieFS file handles are generation-based: each handle carries the inode
 * number plus the inode's 32-bit generation (inode->i_generation), and for
 * directories the parent's ino+gen too. The generation lets the server reject
 * stale handles after an inode number is reused — see briefs_iget_with_gen().
 *
 * We reuse the generic exportfs helpers (generic_encode_ino32_fh,
 * generic_fh_to_dentry, generic_fh_to_parent) and supply only a get_inode
 * callback that validates the generation, plus get_parent (trivial: every
 * BrieFS inode records its parent on disk). commit_metadata is left to the
 * VFS default, which calls briefs_write_inode (journaling the inode via
 * JRN_INODE_FULL).
 *
 * Limitation: generic_encode_ino32_fh packs the inode number into 32 bits.
 * BrieFS inode numbers come from a bitmap pyramid and are well under 2^32
 * for any realistic filesystem, so this is safe.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/exportfs.h>
#include "briefs.h"

/*
 * get_inode callback for the generic fh helpers: resolve (ino, gen) to an
 * inode, validating the generation. gen == 0 (parent handle slot empty for a
 * 2-word handle) is treated as a wildcard so we never spuriously reject a
 * parent-less handle.
 */
static struct inode *briefs_get_inode(struct super_block *sb, u64 ino, u32 gen)
{
	return briefs_iget_with_gen(sb, ino, gen);
}

static struct dentry *briefs_fh_to_dentry(struct super_block *sb, struct fid *fid,
					   int fh_len, int fh_type)
{
	return generic_fh_to_dentry(sb, fid, fh_len, fh_type, briefs_get_inode);
}

static struct dentry *briefs_fh_to_parent(struct super_block *sb, struct fid *fid,
					   int fh_len, int fh_type)
{
	return generic_fh_to_parent(sb, fid, fh_len, fh_type, briefs_get_inode);
}

/*
 * get_parent - return a dentry for the parent directory of @child. BrieFS
 * stores the parent inode number on disk in every inode, so this is a direct
 * lookup. The root inode (ino 1) is its own parent (mkfs sets parent_inode = 1),
 * so resolving root's parent yields root itself, which is what NFS expects.
 */
static struct dentry *briefs_get_parent(struct dentry *child)
{
	struct inode *inode = d_inode(child);
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 parent_ino;
	struct inode *parent;

	parent_ino = binfo->disk_inode.parent_inode;
	if (parent_ino == 0) {
		pr_err("briefs: get_parent: ino %lu has no parent\n", inode->i_ino);
		return ERR_PTR(-ESTALE);
	}

	parent = briefs_iget_with_gen(inode->i_sb, parent_ino, 0);
	if (IS_ERR(parent))
		return ERR_CAST(parent);

	return d_obtain_alias(parent);
}

const struct export_operations briefs_export_ops = {
	.encode_fh	= generic_encode_ino32_fh,
	.fh_to_dentry	= briefs_fh_to_dentry,
	.fh_to_parent	= briefs_fh_to_parent,
	.get_parent	= briefs_get_parent,
};