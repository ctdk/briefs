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

/*
 * Read the on-disk inode block for a given inode number.
 * Returns a buffer_head with a pointer to the inode in *di, or an
 * ERR_PTR on failure.  The caller must brelse() the buffer head.
 */
struct buffer_head *briefs_read_inode_block(struct super_block *sb, u64 ino,
                                             struct briefs_inode **di)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 inodeTableBlock, inodeIndex, inodeBlock, inodeOffset;
	struct buffer_head *bh;

	if (!bsi || !bsi->sb)
		return ERR_PTR(-EIO);

	inodeTableBlock = briefs_inode_table_start(bsi->sb);
	inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	bh = sb_bread(sb, inodeTableBlock + inodeBlock);
	if (!bh) {
		pr_err("briefs: failed to read inode block for ino %llu\n", ino);
		return ERR_PTR(-EIO);
	}

	*di = (struct briefs_inode *)(bh->b_data + inodeOffset);
	return bh;
}

/*
 * Persist a complete struct briefs_inode to disk for the given inode number.
 * If sync is true, also sync the buffer to ensure durability (used during
 * journal replay).  Returns 0 on success, negative errno on error.
 */
int briefs_persist_disk_inode(struct super_block *sb, u64 ino,
                               const struct briefs_inode *src, bool sync)
{
	struct briefs_inode *di;
	struct buffer_head *bh;

	bh = briefs_read_inode_block(sb, ino, &di);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	memcpy(di, src, sizeof(struct briefs_inode));
	mark_buffer_dirty(bh);
	if (sync)
		sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/*
 * Allocate a writable buffer_head for an on-disk block and zero it.
 * Returns the buffer head on success (caller must brelse), or NULL on error.
 * The buffer is marked uptodate and dirty so callers can fill it and release.
 */
struct buffer_head *briefs_get_zero_block(struct super_block *sb, u64 block)
{
	struct buffer_head *bh;

	bh = sb_getblk(sb, block);
	if (!bh)
		return NULL;

	/* sb_getblk may return an unmapped buffer on loop devices */
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
	}

	memset(bh->b_data, 0, sb->s_blocksize);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	return bh;
}

/*
 * Update a parent directory after adding or removing an entry.
 * size_delta is signed bytes to add to dir->i_size; link_delta is signed
 * number of links to add (positive) or remove (negative).  Journals and
 * persists the parent inode.
 */
int briefs_update_parent_dir(struct inode *dir, struct briefs_sb_info *bsi,
                              ssize_t size_delta, int link_delta)
{
	struct briefs_inode_info *pbinfo = briefs_i(dir);
	int ret;

	if (size_delta < 0 && dir->i_size < (size_t)(-size_delta))
		dir->i_size = 0;
	else
		dir->i_size += size_delta;

	if (link_delta > 0)
		inc_nlink(dir);
	else if (link_delta < 0)
		drop_nlink(dir);

	pbinfo->disk_inode.filesize = dir->i_size;
	pbinfo->disk_inode.nlinks = dir->i_nlink;

	ret = briefs_journal_inode_update(bsi->journal, dir);
	if (ret)
		return ret;

	return briefs_persist_disk_inode(dir->i_sb, dir->i_ino, &pbinfo->disk_inode, false);
}

/*
 * Allocate a new inode number, journal the allocation and the future directory
 * entry, initialize the VFS and on-disk inode, and persist it.  Does not add
 * the directory entry or update the parent — the caller must finish with
 * briefs_finish_create().  Returns the new inode, or an ERR_PTR on failure.
 */
struct inode *briefs_new_inode(struct inode *dir, struct dentry *dentry,
                                umode_t mode, dev_t rdev)
{
	struct briefs_sb_info *bsi = dir->i_sb->s_fs_info;
	struct inode *inode;
	struct briefs_inode_info *binfo;
	struct timespec64 now;
	u64 ino;
	int ret;
	bool is_dir = S_ISDIR(mode);

	/* Allocate new inode */
	ino = briefs_alloc_inode(dir->i_sb);
	if (ino == 0)
		return ERR_PTR(-ENOSPC);

	/* Log inode allocation */
	ret = briefs_journal_inode_alloc(bsi->journal, ino, mode, is_dir ? 2 : 1);
	if (ret)
		goto fail_inode;

	/* Log directory entry */
	ret = briefs_journal_dir_add(bsi->journal, dir->i_ino, ino, dentry);
	if (ret)
		goto fail_inode;

	/* Create new inode — use iget_locked directly, skip disk read for new inodes */
	inode = iget_locked(dir->i_sb, ino);
	if (!inode) {
		ret = -ENOMEM;
		goto fail_inode;
	}

	if (!(inode->i_state & I_NEW)) {
		/* Inode already in cache — shouldn't happen for fresh alloc */
		ret = -EEXIST;
		goto fail_iget;
	}

	binfo = briefs_i(inode);
	memset(&binfo->disk_inode, 0, sizeof(struct briefs_inode));
	binfo->inode_number = ino;

	inode->i_mode = mode;
	inode->i_uid = current_fsuid();
	inode->i_gid = current_fsgid();
	inode->i_size = 0;
	inode->i_blocks = briefs_compute_i_blocks(&binfo->disk_inode);
	set_nlink(inode, is_dir ? 2 : 1);

	ktime_get_real_ts64(&now);
	inode->i_atime_sec = inode->i_mtime_sec = inode->i_ctime_sec = now.tv_sec;
	inode->i_atime_nsec = inode->i_mtime_nsec = inode->i_ctime_nsec = now.tv_nsec;

	/* Set up briefs_inode fields */
	binfo->disk_inode.inode_number = ino;
	binfo->disk_inode.magic = _BRIEFS_INODE_MAGIC;
	binfo->disk_inode.filemode = mode;
	binfo->disk_inode.uid = from_kuid(&init_user_ns, inode->i_uid);
	binfo->disk_inode.gid = from_kgid(&init_user_ns, inode->i_gid);
	binfo->disk_inode.filesize = 0;
	binfo->disk_inode.nlinks = is_dir ? 2 : 1;
	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
	briefs_sync_inode_times(inode, &binfo->disk_inode);

	if (is_dir) {
		inode->i_op = &briefs_dir_inode_ops;
		inode->i_fop = &briefs_dir_operations;
	} else if (S_ISREG(mode)) {
		inode->i_op = &briefs_file_inode_ops;
		inode->i_fop = &briefs_file_operations;
		inode->i_mapping->a_ops = &briefs_aops;
	} else if (S_ISLNK(mode)) {
		inode->i_op = &briefs_symlink_inode_ops;
	} else if (S_ISBLK(mode) || S_ISCHR(mode) ||
		   S_ISFIFO(mode) || S_ISSOCK(mode)) {
		init_special_inode(inode, mode, rdev);
		binfo->disk_inode.rdev = rdev;
	}

	unlock_new_inode(inode);

	/* Persist the on-disk inode */
	ret = briefs_persist_disk_inode(dir->i_sb, ino, &binfo->disk_inode, false);
	if (ret)
		goto fail_iget;

	return inode;

fail_iget:
	iput(inode);
fail_inode:
	briefs_free_inode_num(dir->i_sb, ino);
	return ERR_PTR(ret);
}

/*
 * Add a directory entry for a newly created inode and update the parent.
 * Returns 0 on success, negative errno on error.  The caller still owns the
 * inode reference and must iput() it on failure.
 */
int briefs_finish_create(struct inode *dir, struct dentry *dentry,
                          struct inode *inode, int link_delta)
{
	struct briefs_sb_info *bsi = dir->i_sb->s_fs_info;
	u8 ftype = (inode->i_mode & S_IFMT) >> 12;
	int ret;

	ret = briefs_add_dir_entry(dir, dentry->d_name.name, dentry->d_name.len,
				    inode->i_ino, ftype);
	if (ret) {
		pr_err("briefs: failed to add dir entry %pd: %d\n", dentry, ret);
		return ret;
	}

	ret = briefs_update_parent_dir(dir, bsi,
					sizeof(struct briefs_dir_entry) + 2 + dentry->d_name.len,
					link_delta);
	if (ret) {
		pr_err("briefs: failed to update parent dir after create: %d\n", ret);
		return ret;
	}

	return 0;
}

/* briefs_write_inode - persist VFS inode to disk */
int briefs_write_inode(struct inode *inode, struct writeback_control *wbc) {
	struct briefs_sb_info *bsi;
	struct briefs_inode_info *binfo;
	struct briefs_inode *disk_inode;
	struct buffer_head *bh;
	unsigned seq;

	if (!inode)
		return -EINVAL;

	bsi = inode->i_sb->s_fs_info;
	binfo = briefs_i(inode);

	pr_debug("briefs: write_inode %lu\n", inode->i_ino);

	bh = briefs_read_inode_block(inode->i_sb, inode->i_ino, &disk_inode);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	/* Sync VFS timestamp fields into in-memory copy first */
	briefs_sync_inode_times(inode, &binfo->disk_inode);

	/*
	 * Copy in-memory disk_inode to the on-disk buffer.
	 * Use a seqcount retry loop to ensure we don't persist a
	 * partially-updated extent list if briefs_append_extent or
	 * another extent writer is in progress concurrently.
	 */
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

	mark_buffer_dirty(bh);
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
	mutex_init(&binfo->trie_lock);
	mutex_init(&binfo->extent_lock);
	binfo->trie_gen = 0;
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

	pr_debug("briefs: iget inode %llu\n", ino);

	bsi = sb->s_fs_info;
	if (!bsi || !bsi->sb) {
		pr_err("briefs: no sb_info for ino %llu\n", ino);
		return ERR_PTR(-EIO);
	}

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		/* Read inode from disk */
		bh = briefs_read_inode_block(sb, ino, &disk_inode);
		if (IS_ERR(bh)) {
			pr_err("briefs: unable to read inode block for ino %llu\n", ino);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EIO);
		}

		if (disk_inode->magic != _BRIEFS_INODE_MAGIC) {
			pr_err("briefs: invalid inode magic for ino %llu: 0x%08llx\n", ino, disk_inode->magic);
			brelse(bh);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EINVAL);
		}

		/* Copy disk inode to VFS inode */
		binfo = briefs_i(inode);
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

	/*
	 * Truncate the page cache first — this releases all folios and
	 * their buffer_heads before we free the backing blocks.  The order
	 * matters: free_inode_data clears the block allocator's bits for
	 * the extents, and if buffer_heads still reference those blocks,
	 * a later reclaim of those stale folios will find
	 * use-after-freed buffer_head chains.
	 */
	truncate_inode_pages_final(&inode->i_data);

	/* When nlink drops to 0, free allocated blocks and the inode number */
	if (inode->i_nlink == 0) {
		briefs_free_inode_data(inode);
		briefs_free_inode_num(inode->i_sb, inode->i_ino);
	}
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
	struct briefs_inode zero_di = {0};

	if (ino == 0)
		return;

	/*
	 * Zero out the inode on disk so fsck doesn't see a "free" inode with
	 * valid magic.  This is metadata bookkeeping, not a durability boundary,
	 * so just mark the buffer dirty and let the block cache write it back.
	 */
	briefs_persist_disk_inode(sb, ino, &zero_di, false);

	briefs_free_block(&bsi->inode_alloc, ino - 1);
	pr_debug("briefs: freed inode %llu\n", ino);
}
