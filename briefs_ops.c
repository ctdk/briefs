/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include "briefs.h"
#include "briefs_alloc.h"

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
	.evict_inode = briefs_evict_inode,
	.umount_begin = briefs_umount_begin,
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
	struct briefs_superblock *bsb;
	struct inode *root_inode;
	struct briefs_sb_info *bsi;
	int ret = -EINVAL;

	pr_info("briefs: fill_super enter\n");
	pr_info("briefs: sb=%p, blocksize=%d\n", sb, sb->s_blocksize);

	bsi = kzalloc(sizeof(struct briefs_sb_info), GFP_KERNEL);
	if (!bsi) {
		pr_err("briefs: failed to allocate sb_info\n");
		return -ENOMEM;
	}
	sb->s_fs_info = bsi;

	/* Keeping the same gotos like xiafs, at least for now. JUMP! */
	if (!sb_set_blocksize(sb, 4096)) {
		pr_err("briefs: blocksize too small\n");
		goto out_bad_hblock;
	}

	/* Read the superblock from the first block for now. Subject to change,
	 * though, because of possible conflicts with old-timey disks. */
	if (!(bh = sb_bread(sb, 0))) {
		pr_err("briefs: unable to read superblock\n");
		goto out_bad_sb;
	}

	bsb = (struct briefs_superblock *) bh->b_data;

	sb->s_magic = bsb->magic;

	pr_info("briefs: magic=0x%016llx\n", sb->s_magic);

	if (sb->s_magic != _BRIEFS_SUPER_MAGIC) {
		pr_err("briefs: invalid magic\n");
		sb->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

	/* Setup bsi now. Since there's not a lot in that struct yet, there's
	 * not much here. Fill in relevant fields as they become available. */
	bsi->sb = bsb;

	/* Initialize allocator with trie */
	ret = briefs_alloc_init(&bsi->alloc, sb, bsb);
	if (ret) {
		pr_err("briefs: failed to initialize allocator: %d\n", ret);
		goto out_no_root;
	}

	sb->s_op = &briefs_super_ops;

	root_inode = briefs_iget(sb, _BRIEFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		pr_err("BrieFS: error getting root inode.\n");
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

	ret = -ENOMEM;

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("briefs: d_make_root failed\n");
		goto out_iput;
	}

	pr_info("briefs: superblock loaded, mounting successful\n");

	if (!sb_rdonly(sb))
		mark_buffer_dirty(bh);

	return 0;

	/* Error labels below */
out_no_root:
	pr_err("briefs: get root inode failed.\n");
	goto out_release;

out_iput:
	iput(root_inode);
	goto out_release;

out_no_fs:
	pr_err("VFS: Can't find a BrieFS filesystem on device %s.\n", sb->s_id);

out_release:
	brelse(bh);
	goto out;

out_bad_hblock:
	pr_err("BrieFS: blocksize too small for device.\n");
	goto out;

out_bad_sb:
	pr_err("BrieFS: unable to read superblock.\n");

out:
	sb->s_fs_info = NULL;
	kfree(bsi);
	pr_info("briefs: fill_super returning %d\n", ret);
	return ret;
}

/* briefs_iget - get an inode by number */
struct inode *briefs_iget(struct super_block *sb, u64 ino) {
	struct inode *inode;

	pr_debug("briefs: iget inode %llu\n", ino);

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		/* For now, just set basic defaults */
		if (ino == _BRIEFS_ROOT_INO) {
			inode->i_mode = S_IFDIR | 0755;
			inode->i_uid = current_fsuid();
			inode->i_gid = current_fsgid();
			inode->i_size = 0;
			inode->i_blocks = 0;
			set_nlink(inode, 2);
		} else {
			/* For other inodes, just return -ENOENT for now */
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-ENOENT);
		}

		unlock_new_inode(inode);
	}

	return inode;
}


/* briefs_evict_inode - cleanup inode on eviction */
void briefs_evict_inode(struct inode *inode) {
	pr_debug("briefs: evict_inode inode %lu\n", inode->i_ino);
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}



void briefs_put_super(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;

	pr_info("briefs: put_super enter\n");

	if (bsi) {
		pr_info("briefs: calling alloc_cleanup\n");
		briefs_alloc_cleanup(&bsi->alloc);
		pr_info("briefs: alloc_cleanup done\n");
	}

	pr_info("briefs: put_super\n");
}

/* briefs_umount_begin - called before unmount */
void briefs_umount_begin(struct super_block *sb) {
	pr_info("briefs: umount_begin\n");
}

/* briefs_statfs - filesystem statistics */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	pr_info("briefs: statfs\n");
	return -EROFS;
}

/* briefs_kill_sb - called when sb is being destroyed */
void briefs_kill_sb(struct super_block *sb) {
	pr_info("briefs: kill_sb\n");
	if (sb) {
		pr_info("briefs: killing sb %p\n", sb);
	}
	kill_block_super(sb);
}


/* briefs_mount - mount callback */
struct dentry *briefs_mount(struct file_system_type *fs_type, int flags,
                           const char *dev_name, void *data) {
	pr_info("briefs: mount callback\n");
	return mount_bdev(fs_type, flags, dev_name, data, briefs_fill_super);
}


