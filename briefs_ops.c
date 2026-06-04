/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

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

	bsi = kzalloc(sizeof(struct briefs_sb_info), GFP_KERNEL);
	if (!bsi)
		return -ENOMEM;
	sb->s_fs_info = bsi;

	if (!sb_set_blocksize(sb, 4096)) {
		pr_err("briefs: blocksize too small\n");
		goto out_bad_hblock;
	}

	if (!(bh = sb_bread(sb, 0))) {
		pr_err("briefs: unable to read superblock\n");
		goto out_bad_sb;
	}

	bsb = (struct briefs_superblock *) bh->b_data;

	pr_info("briefs: superblock magic=0x%016llx, data_bitmap_offset=%d, data_bitmap_blocks=%d\n", bsb->magic, bsb->data_bitmap_offset, bsb->data_bitmap_blocks);

	sb->s_magic = bsb->magic;

	pr_info("briefs: magic=0x%016llx\n", sb->s_magic);

	if (sb->s_magic != _BRIEFS_SUPER_MAGIC) {
		pr_err("briefs: invalid magic\n");
		sb->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

	bsi->sb = bsb;

	/* Initialize allocator with trie */
	ret = briefs_alloc_init(&bsi->alloc, sb, bsb);
	if (ret) {
		pr_err("briefs: failed to initialize allocator: %d\n", ret);
		goto out_no_root;
	}

	/* Initialize journal */
	bsi->journal = kzalloc(sizeof(struct briefs_journal), GFP_KERNEL);
	if (!bsi->journal) {
		ret = -ENOMEM;
		goto out_no_journal;
	}

	ret = briefs_journal_open(bsi->journal, bsb, sb->s_bdev);
	if (ret) {
		pr_err("briefs: failed to initialize journal: %d\n", ret);
		goto out_no_journal;
	}

	/* Replay journal on mount (if not clean) */
	ret = briefs_journal_replay(bsi->journal);
	if (ret) {
		pr_err("briefs: journal replay failed: %d\n", ret);
		goto out_no_journal;
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

	out_no_journal:
	pr_err("briefs: journal init failed.\n");
	goto out_release;

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
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	struct briefs_sb_info *bsi;
	u64 inodeTableBlock;
	u64 inodeBlock;
	u64 inodeOffset;

	pr_debug("briefs: iget inode %llu\n", ino);

	bsi = sb->s_fs_info;
	if (!bsi || !bsi->sb) {
		pr_err("briefs: no sb_info for ino %llu\n", ino);
		return ERR_PTR(-EIO);
	}

	/* Calculate inode location: inode table follows data bitmap */
	inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
	/* Inode table starts at inodeTableBlock, each block has 8 inodes (512 bytes each)
	 * Inode 1 (root) is at the first slot of inodeTableBlock (block 3)
	 * Inode index = (inodeTableBlock * 8) + (ino - 1)
	 * File offset = inode_index * 512
	 */
	u64 inodeIndex = (inodeTableBlock * (sb->s_blocksize / 512)) + (ino - 1);
	inodeBlock = 0;
	inodeOffset = (inodeIndex % 8) * 512;

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
		u64 inodeIndex = (inodeTableBlock * (sb->s_blocksize / 512)) + (ino - 1);
		inodeBlock = 0;
		inodeOffset = (inodeIndex % 8) * 512;
		pr_info("briefs: data_bitmap_offset=%d, data_bitmap_blocks=%d, inodeTableBlock=%d, inodeIndex=%d, inodeBlock=%d, inodeOffset=%d\n", bsi->sb->data_bitmap_offset, bsi->sb->data_bitmap_blocks, inodeTableBlock, inodeIndex, inodeBlock, inodeOffset);
	pr_info("briefs: reading inode %llu from block %d (inodeTableBlock=%d, inodeBlock=%d)\n", ino, inodeTableBlock + inodeBlock, inodeTableBlock, inodeBlock);
		/* Read inode from disk */
		bh = sb_bread(sb, inodeTableBlock + inodeBlock);
		if (!bh) {
			pr_err("briefs: unable to read inode block for ino %llu\n", ino);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EIO);
		}

		disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);

		if (disk_inode->magic != 0x494E4F44) {
			pr_err("briefs: invalid inode magic for ino %llu: 0x%08x\n", ino, disk_inode->magic);
			brelse(bh);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EINVAL);
		}

		/* Copy disk inode to VFS inode */
		binfo = (struct briefs_inode_info *)inode;
		memcpy(&binfo->disk_inode, disk_inode, sizeof(struct briefs_inode));
		binfo->inode_number = ino;

		/* Set VFS inode fields from disk inode */
		inode->i_mode = disk_inode->filemode;
		inode->i_uid = make_kuid(&init_user_ns, disk_inode->uid);
		inode->i_gid = make_kgid(&init_user_ns, disk_inode->gid);
		inode->i_size = disk_inode->filesize;
		inode->i_blocks = 0;
		set_nlink(inode, disk_inode->nlinks);

		/* Set VFS operations based on inode type */
		if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &briefs_dir_inode_ops;
			inode->i_fop = &briefs_dir_operations;
		} else if (S_ISREG(inode->i_mode)) {
			inode->i_op = &briefs_file_inode_ops;
			inode->i_fop = &briefs_file_operations;
		}

		pr_info("briefs: inode %llu: mode=0x%04x, uid=%u, gid=%u, size=%llu, nlink=%u\n",
			ino, inode->i_mode, from_kuid(&init_user_ns, inode->i_uid),
			from_kgid(&init_user_ns, inode->i_gid), inode->i_size, inode->i_nlink);

		brelse(bh);
		unlock_new_inode(inode);
	}

	return inode;
}

/* briefs_put_super - cleanup superblock */
void briefs_put_super(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;

	pr_info("briefs: put_super enter\n");

	if (bsi) {
		/* Sync journal before unmount */
		if (bsi->journal && bsi->journal->dirty) {
			briefs_journal_checkpoint(bsi->journal);
		}

		pr_info("briefs: cleaning up journal\n");
		if (bsi->journal) {
			briefs_journal_cleanup(bsi->journal);
			kfree(bsi->journal);
			bsi->journal = NULL;
		}

		pr_info("briefs: calling alloc_cleanup\n");
		briefs_alloc_cleanup(&bsi->alloc);
		pr_info("briefs: alloc_cleanup done\n");
	}

	pr_info("briefs: put_super\n");
}

/* briefs_statfs - filesystem statistics */
/* shamelessly yoinking from xiafs - incomplete */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct briefs_sb_info *sbi = briefs_sb(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_blocksize;

	buf->f_namelen = BRIEFS_NAME_LEN;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	return 0;
}

/* briefs_evict_inode - cleanup inode on eviction */
void briefs_evict_inode(struct inode *inode) {
	pr_debug("briefs: evict_inode inode %lu\n", inode->i_ino);
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/* briefs_umount_begin - called before unmount */
void briefs_umount_begin(struct super_block *sb) {
	pr_info("briefs: umount_begin\n");
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

/* briefs_alloc_trie_node - allocate a trie node block */
struct briefs_trie_node *briefs_alloc_trie_node(struct super_block *sb) {
	struct briefs_trie_node *node;
	struct buffer_head *bh;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 block;

	if (!bsi || !bsi->sb)
		return ERR_PTR(-EINVAL);

	/* Allocate from trie node pool */
	block = bsi->sb->trie_node_pool_start + bsi->sb->trie_blocks_used;

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	node = (struct briefs_trie_node *)bh->b_data;
	memset(node, 0, sizeof(struct briefs_trie_node));

	/* Initialize node */
	node->magic = 0x54524E20;  /* "TRN " */
	node->node_index = bsi->sb->trie_blocks_used++;

	mark_buffer_dirty(bh);
	brelse(bh);

	return node;
}

/* briefs_free_trie_node - free a trie node block */
void briefs_free_trie_node(struct super_block *sb, struct briefs_trie_node *node) {
	/* TODO: implement free list */
	kfree(node);
}

/* briefs_init_trie_root - initialize trie root node */
void briefs_init_trie_root(struct briefs_trie_node *root) {
	memset(root, 0, sizeof(struct briefs_trie_node));
	root->magic = 0x54524E20;  /* "TRN " */
	root->flags = NODE_FLAG_ROOT;
	root->node_type = NODE_TYPE_INTERM;
}

/* briefs_alloc_inode - allocate a new inode number */
static u64 briefs_alloc_inode(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_superblock *sbk = bsi->sb;
	
	/* Find first free inode by checking free_inodes bitmap */
	/* For now, return the next available inode number */
	u64 ino = sbk->free_inodes + 1;  /* Inode 1 is root */
	if (ino >= 0xFFFFFFFF) {
		return 0;  /* No more inodes available */
	}
	return ino;
}

/* briefs_lookup - find inode by name in directory */
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
	struct briefs_sb_info *bsi;
	struct briefs_superblock *sbk;
	const char *name;
	size_t name_len;
	
	if (!dir || !S_ISDIR(dir->i_mode)) {
		return ERR_PTR(-ENOTDIR);
	}
	
	bsi = dir->i_sb->s_fs_info;
	sbk = bsi->sb;
	
	pr_debug("briefs: lookup for %pd in dir inode %lu\n", dentry, dir->i_ino);
	
	name = dentry->d_name.name;
	name_len = dentry->d_name.len;
	
	/* Check for . and .. */
	if (name_len <= 2) {
		if (name[0] == '.' && name_len == 1) {
			/* . - return current directory */
			igrab(dir);
			return d_splice_alias(dir, dentry);
		}
		if (name[0] == '.' && name[1] == '.' && name_len == 2) {
			/* .. - return parent directory (root has no parent) */
			if (dir->i_ino == _BRIEFS_ROOT_INO) {
				igrab(dir);
			} else {
				return ERR_PTR(-EIO);  /* TODO: store parent in inode */
			}
			return d_splice_alias(dir, dentry);
		}
	}
	
	pr_debug("briefs: lookup - name not found: %.*s\n", (int)name_len, name);
	return ERR_PTR(-ENOENT);
}

/* briefs_readdir - enumerate directory contents */

/* briefs_create - create a new file */
int briefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	struct briefs_sb_info *bsi;
	struct briefs_superblock *sbk;
	struct inode *inode;
	u64 ino;
	int ret;
	
	pr_debug("briefs: create %pd (mode=%o) in dir %lu\n", dentry, mode, dir->i_ino);
	
	bsi = dir->i_sb->s_fs_info;
	sbk = bsi->sb;
	
	/* Allocate new inode */
	ino = briefs_alloc_inode(dir->i_sb);
	if (ino == 0) {
		return -ENOSPC;
	}
	
	/* Log inode allocation */
	if (bsi->journal) {
		struct jrn_inode_alloc irec;
		memset(&irec, 0, sizeof(irec));
		irec.ino = ino;
		irec.mode = mode & 07777;
		irec.nlink = 1;
		ret = briefs_journal_write_record(bsi->journal, JRN_INODE_ALLOC, &irec, sizeof(irec));
		if (ret) return ret;
	}
	
	/* Log directory entry */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, dir->i_ino, ino,
						dentry->d_name.name, dentry->d_name.len, 0);
		if (ret) return ret;
	}
	
	/* Create new inode */
	inode = briefs_iget(dir->i_sb, ino);
	if (IS_ERR(inode)) {
		return PTR_ERR(inode);
	}
	
	/* Initialize inode */
	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_size = 0;
	inode->i_blocks = 0;
	
	/* Set times */
	inode->i_mtime_sec = inode->i_ctime_sec = ktime_get_real_seconds();
	inode->i_mtime_nsec = inode->i_ctime_nsec = 0;
	
	iput(inode);
	
	/* Sync journal if we have pending writes */
	if (bsi->journal && bsi->journal->dirty) {
		briefs_journal_sync(bsi->journal);
	}
	
	pr_debug("briefs: created inode %llu\n", ino);
	return 0;
}

/* briefs_mkdir - create a new directory */
int briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
	pr_debug("briefs: mkdir %pd\n", dentry);
	return briefs_create(idmap, dir, dentry, (mode & ~07777) | S_IFDIR, false);
}

/* briefs_unlink - remove a directory entry */
int briefs_unlink(struct inode *dir, struct dentry *dentry) {
	struct briefs_sb_info *bsi;
	int ret;
	
	pr_debug("briefs: unlink %pd\n", dentry);
	
	bsi = dir->i_sb->s_fs_info;
	
	/* Log directory entry deletion */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, dir->i_ino, 0,
						dentry->d_name.name, dentry->d_name.len, 1);
		if (ret) return ret;
	}
	
	/* Sync journal */
	if (bsi->journal && bsi->journal->dirty) {
		briefs_journal_sync(bsi->journal);
	}
	
	return -EROFS;  /* TODO: implement actual unlink after dir entry tree is ready */
}

/* briefs_rename - rename a directory entry */
int briefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
	struct briefs_sb_info *bsi;
	int ret;
	
	pr_debug("briefs: rename %pd -> %pd\n", old_dentry, new_dentry);
	
	bsi = old_dir->i_sb->s_fs_info;
	
	/* Log old entry deletion */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, old_dir->i_ino, 0,
						old_dentry->d_name.name, old_dentry->d_name.len, 1);
		if (ret) return ret;
	}
	
	/* Log new entry creation */
	if (bsi->journal) {
		struct dentry *child = d_find_alias(new_dir);
		u64 child_ino = child ? child->d_inode->i_ino : 0;
		dput(child);
		
		ret = briefs_journal_dir_update(bsi->journal, new_dir->i_ino, child_ino,
						new_dentry->d_name.name, new_dentry->d_name.len, 0);
		if (ret) return ret;
	}
	
	/* Sync journal */
	if (bsi->journal && bsi->journal->dirty) {
		briefs_journal_sync(bsi->journal);
	}
	
	return -EROFS;  /* TODO: implement actual rename */
}
