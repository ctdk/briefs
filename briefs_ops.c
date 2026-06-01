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

/* briefs_fill_super - entry point for mount (not implemented yet) */
int briefs_fill_super(struct super_block *sb, void *data, int flags) {
	pr_info("briefs: mount not implemented yet\n");
	return -EROFS;
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

const struct super_operations briefs_super_ops = {
	.put_super = briefs_put_super,
	.statfs = briefs_statfs,
};
