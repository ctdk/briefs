/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs inode lifecycle operations */

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

/* briefs_write_inode - persist VFS inode to disk */
int briefs_write_inode(struct inode *inode, struct writeback_control *wbc) {
	struct briefs_sb_info *bsi;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	u64 inodeTableBlock, inodeBlock, inodeOffset, inodeIndex;

	if (!inode)
		return -EINVAL;

	bsi = inode->i_sb->s_fs_info;
	binfo = briefs_i(inode);

	pr_debug("briefs: write_inode %lu\n", inode->i_ino);

	/* Calculate inode location */
	inodeTableBlock = briefs_inode_table_start(bsi->sb);
	inodeIndex = inode->i_ino - 1;
	inodeBlock = inodeIndex / (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	/* Read the block containing this inode */
	bh = sb_bread(inode->i_sb, inodeTableBlock + inodeBlock);
	if (!bh) {
		pr_err("briefs: failed to read inode block for write_inode %lu\n", inode->i_ino);
		return -EIO;
	}

	disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);

	/* Sync VFS timestamp fields into in-memory copy first */
	binfo->disk_inode.atime_sec = inode->i_atime_sec;
	binfo->disk_inode.atime_nsec = inode->i_atime_nsec;
	binfo->disk_inode.mtime_sec = inode->i_mtime_sec;
	binfo->disk_inode.mtime_nsec = inode->i_mtime_nsec;
	binfo->disk_inode.ctime_sec = inode->i_ctime_sec;
	binfo->disk_inode.ctime_nsec = inode->i_ctime_nsec;

	/*
	 * Copy in-memory disk_inode to the on-disk buffer.
	 * Use a seqcount retry loop to ensure we don't persist a
	 * partially-updated extent list if briefs_append_extent or
	 * another extent writer is in progress concurrently.
	 */
	{
		unsigned seq;
		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			memcpy(disk_inode, &binfo->disk_inode, sizeof(struct briefs_inode));
			/* Update VFS-derived fields after the copy */
			disk_inode->filemode = inode->i_mode;
			disk_inode->uid = from_kuid(&init_user_ns, inode->i_uid);
			disk_inode->gid = from_kgid(&init_user_ns, inode->i_gid);
			disk_inode->filesize = inode->i_size;
			disk_inode->nlinks = inode->i_nlink;
		} while (read_seqcount_retry(&binfo->extent_seq, seq));
	}

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}
/* briefs_alloc_inode - allocate a VFS inode (called by VFS inode cache) */
struct inode *briefs_alloc_vfs_inode(struct super_block *sb) {
	struct briefs_inode_info *binfo;

	binfo = alloc_inode_sb(sb, briefs_inode_cachep, GFP_KERNEL);
	if (!binfo)
		return NULL;

	seqcount_init(&binfo->extent_seq);
	return &binfo->vfs_inode;
}
/* briefs_free_inode - free a VFS inode (called by VFS inode cache) */
void briefs_free_inode(struct inode *inode) {
	struct briefs_inode_info *binfo = briefs_i(inode);
	kmem_cache_free(briefs_inode_cachep, binfo);
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
	inodeTableBlock = briefs_inode_table_start(bsi->sb);
	/* Inode table starts at inodeTableBlock. Each block holds 8 inodes (512 bytes each).
	 * inodeIndex is the 0-based index into the inode table.
	 * inodeBlock is the block offset within the inode table.
	 * inodeOffset is the byte offset within that block.
	 */
	u64 inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		/* Recompute inode location inside the locked block */
		u64 inodeTableBlock = briefs_inode_table_start(bsi->sb);
		u64 inodeIndex = ino - 1;
		inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
		inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
		pr_info("briefs: inode_table_offset=%llu, inodeIndex=%llu, inodeBlock=%llu, inodeOffset=%llu\n",
			bsi->sb->inode_table_offset, inodeIndex, inodeBlock, inodeOffset);
		pr_info("briefs: reading inode %llu from block %llu (inodeTableBlock=%llu, inodeBlock=%llu)\n",
			ino, inodeTableBlock + inodeBlock, inodeTableBlock, inodeBlock);
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
			pr_err("briefs: invalid inode magic for ino %llu: 0x%08llx\n", ino, disk_inode->magic);
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
		inode->i_blocks = briefs_compute_i_blocks(disk_inode);

		set_nlink(inode, disk_inode->nlinks);

		inode->i_atime_sec = disk_inode->atime_sec;
		inode->i_atime_nsec = disk_inode->atime_nsec;
		inode->i_mtime_sec = disk_inode->mtime_sec;
		inode->i_mtime_nsec = disk_inode->mtime_nsec;
		inode->i_ctime_sec = disk_inode->ctime_sec;
		inode->i_ctime_nsec = disk_inode->ctime_nsec;

		/* Set VFS operations based on inode type */
		if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &briefs_dir_inode_ops;
			inode->i_fop = &briefs_dir_operations;
		} else if (S_ISREG(inode->i_mode)) {
			inode->i_op = &briefs_file_inode_ops;
			inode->i_fop = &briefs_file_operations;
			inode->i_mapping->a_ops = &briefs_aops;
		} else if (S_ISLNK(inode->i_mode)) {
			inode->i_op = &briefs_symlink_inode_ops;
			/* no i_fop for symlinks */
		} else if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode) ||
			   S_ISFIFO(inode->i_mode) || S_ISSOCK(inode->i_mode)) {
			init_special_inode(inode, inode->i_mode, disk_inode->rdev);
		}

		pr_info("briefs: inode %llu: mode=0x%04x, uid=%u, gid=%u, size=%llu, nlink=%u\n",
			ino, inode->i_mode, from_kuid(&init_user_ns, inode->i_uid),
			from_kgid(&init_user_ns, inode->i_gid), inode->i_size, inode->i_nlink);

		brelse(bh);
		unlock_new_inode(inode);
	}

	return inode;
}
/* briefs_evict_inode - cleanup inode on eviction */
void briefs_evict_inode(struct inode *inode) {
	pr_debug("briefs: evict_inode inode %lu\n", inode->i_ino);
	/* When nlink drops to 0, free allocated blocks and the inode number */
	if (inode->i_nlink == 0) {
		briefs_free_inode_data(inode);
		briefs_free_inode_num(inode->i_sb, inode->i_ino);
	}
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}
/* briefs_alloc_inode - allocate a new inode number using the bitmap pyramid */
u64 briefs_alloc_inode(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 inum = briefs_alloc_block(&bsi->inode_alloc);
	if (inum == 0) {
		pr_err("briefs: no free inodes\n");
		return 0;
	}
	/* inum is 0-based index, convert to 1-based inode number */
	u64 ino = inum + 1;
	pr_debug("briefs: allocated inode %llu\n", ino);
	return ino;
}
void briefs_free_inode_num(struct super_block *sb, u64 ino) {
	struct briefs_sb_info *bsi = sb->s_fs_info;
	if (ino == 0)
		return;
	briefs_free_block(&bsi->inode_alloc, ino - 1);
	pr_debug("briefs: freed inode %llu\n", ino);
}
