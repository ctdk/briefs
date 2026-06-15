/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations 2014 op structure definitions */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/seqlock.h>
#include <linux/pagemap.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

/* Inode operations for directories */
const struct inode_operations briefs_dir_inode_ops = {
	.create = briefs_create,
	.lookup = briefs_lookup,
	.link = briefs_link,
	.mkdir = briefs_mkdir,
	.symlink = briefs_symlink,
	.mknod = briefs_mknod,
	.unlink = briefs_unlink,
	.rmdir = briefs_rmdir,
	.rename = briefs_rename,
};

/* Inode operations for files */
const struct inode_operations briefs_file_inode_ops = {
	.setattr = briefs_setattr,
	.getattr = briefs_getattr,
};

/* Inode operations for symlinks */
const struct inode_operations briefs_symlink_inode_ops = {
	.get_link = briefs_get_link,
};

/* File operations for directories */
const struct file_operations briefs_dir_operations = {
	.llseek = generic_file_llseek,
	.iterate_shared = briefs_readdir,
	.open = briefs_dir_open,
	.release = briefs_dir_release,
	.fsync = briefs_fsync,
};

/* File operations for regular files */
const struct file_operations briefs_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = briefs_read_iter,
	.write_iter = briefs_write_iter,
	.open = briefs_open,
	.release = briefs_release,
	.mmap = generic_file_mmap,
	.fsync = briefs_fsync,
};

/* Superblock operations */
const struct super_operations briefs_super_ops = {
	.put_super = briefs_put_super,
	.sync_fs = briefs_sync_fs,
	.statfs = briefs_statfs,
	.write_inode = briefs_write_inode,
	.evict_inode = briefs_evict_inode,
	.umount_begin = briefs_umount_begin,
	.alloc_inode = briefs_alloc_vfs_inode,
	.free_inode = briefs_free_inode,
};

