/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include "briefs.h"

/* Inode operations for directories */
const struct inode_operations briefs_dir_inode_ops = {
	.create = briefs_create,
	.lookup = briefs_lookup,
	.mkdir = briefs_mkdir,
	.unlink = briefs_unlink,
	.rename = briefs_rename,
};

/* Inode operations for files */
const struct inode_operations briefs_file_inode_ops = {
	.setattr = simple_setattr,
	.getattr = simple_getattr,
};

/* File operations for directories */
const struct file_operations briefs_dir_operations = {
	.llseek = generic_file_llseek,
	.iterate_shared = briefs_readdir,
	.release = NULL,
};

/* File operations for regular files */
const struct file_operations briefs_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = briefs_read_iter,
	.write_iter = briefs_write_iter,
	.open = briefs_open,
	.release = briefs_release,
	.mmap = generic_file_mmap,
};

/* Superblock operations */
const struct super_operations briefs_super_ops = {
	.put_super = briefs_put_super,
	.statfs = briefs_statfs,
};

/* Create */
int briefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	pr_debug("briefs: create %pd (mode=%o)\n", dentry, mode);
	return -EROFS;
}

/* Lookup */
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
	pr_debug("briefs: lookup %pd\n", dentry);
	return ERR_PTR(-ENOENT);
}

/* Mkdir */
int briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
	pr_debug("briefs: mkdir %pd\n", dentry);
	return -EROFS;
}

/* Unlink */
int briefs_unlink(struct inode *dir, struct dentry *dentry) {
	pr_debug("briefs: unlink %pd\n", dentry);
	return -EROFS;
}

/* Rename */
int briefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
	pr_debug("briefs: rename %pd -> %pd\n", old_dentry, new_dentry);
	return -EROFS;
}

/* Readdir */
int briefs_readdir(struct file *file, struct dir_context *ctx) {
	pr_debug("briefs: readdir offset=%llu\n", ctx->pos);
	if (ctx->pos >= 2) return 0;

	if (!dir_emit_dots(file, ctx)) return -EIO;

	ctx->pos = 2;
	return 0;
}

/* Read file */
ssize_t briefs_read_iter(struct kiocb *iocb, struct iov_iter *to) {
	pr_debug("briefs: read_iter size=%zu\n", iov_iter_count(to));
	return 0;
}

/* Write file */
ssize_t briefs_write_iter(struct kiocb *iocb, struct iov_iter *from) {
	pr_debug("briefs: write_iter size=%zu\n", iov_iter_count(from));
	return -EROFS;
}

/* Open file */
int briefs_open(struct inode *inode, struct file *file) {
	pr_debug("briefs: open inode %lu\n", inode->i_ino);
	return 0;
}

/* Release file */
int briefs_release(struct inode *inode, struct file *file) {
	pr_debug("briefs: release inode %lu\n", inode->i_ino);
	return 0;
}

/* briefs_fill_super - entry point for mount */
/* Keeping things simple, at least at first. Cribbing off of xiafs_fill_super
 * since it's reasonably simple but gets the job done. */
int briefs_fill_super(struct super_block *sb, void *data, int flags) {
	struct buffer_head *bh;
	struct buffer_head **map;
	struct briefs_superblock *bsb;
	struct inode *root_inode;
	struct briefs_sb_info *bsi;
	int ret = -EINVAL;

	sbi = kzalloc(sizeof(struct briefs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;

	/* Keeping the same gotos like xiafs, at least for now. JUMP! */
	if (!sb_set_blocksize(sb, BLOCK_SIZE))
		goto out_bad_hblock;

	/* Read the superblock from the first block for now. Subject to change,
	 * though, because of possible conflicts with old-timey disks. */
	if (!(bh = sb_bread(sb, 0)))
		goto out_bad_sb;

	bsb = (struct briefs_superblock *) bh->b_data;

	sb->s_magic = bsb->magic;

	if (sb->s_magic != _BRIEFS_SUPER_MAGIC) {
		sb->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

	/* Setup sbi now. Since there's not a lot in that struct yet, there's
	 * not much here. Fill in relevant fields as they become available. */
	sbi->briefs_superblock = bsb;

	sb->s_op = &briefs_super_ops;

	root_inode = briefs_iget(sb, _BRIEFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		printk("BrieFS: error getting root inode.\n");
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

	ret = -ENOMEM;

	sb->s_root = d_make_root(root_inode);
	if (!sb->root)
		goto out_iput;

	if (!sb_readonly(sb))
		mark_buffer_dirty(bh);

	return 0;

	/* Error labels below */
out_no_root:
	printk("BrieFS: get root inode failed.\n");
	/* Once the various tries are being set up in memory, goto that label
	 * and *then* goto out_release. */
	goto out_release;

out_iput:
	/* Same as above */
	iput(root_inode);
	goto out_release;

out_no_fs:
	printk("VFS: Can't find a BrieFS filesystem on device %s.\n", sb->s_id);

out_release:
	brelse(bh);
	goto out;

out_bad_hblock:
	printk("BrieFS: blocksize too small for device.\n");
	goto out;

out_bad_sb:
	printk("BrieFS: unable to read superblock.\n");

out:
	sb->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

/* briefs_put_super - cleanup superblock */
void briefs_put_super(struct super_block *sb) {
	pr_info("briefs: put_super\n");
}

/* briefs_statfs - filesystem statistics */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	pr_info("briefs: statfs\n");
	return -EROFS;
}
