/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/fsnotify.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/sched/signal.h>
#include <linux/buffer_head.h>
#include <linux/seqlock.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/migrate.h>
#include <linux/fileattr.h>
#include <linux/posix_acl.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"
#include "briefs_debug.h"
#include "briefs_iomap.h"
#include "briefs_exchange.h"

/* Not declared in the headers on 6.12. */
extern void generic_fill_statx_attr(struct inode *inode, struct kstat *stat);

/*
 * File-range exchange engine (defined later in this file; briefs_ioctl at the
 * top of the file dispatches to these entry points).  They mirror the XFS
 * XFS_IOC_EXCHANGE_RANGE / START_COMMIT / COMMIT_RANGE / SWAPEXT ioctls using
 * the copied UAPI layouts in briefs_exchange.h.
 */
static long briefs_ioc_exchange_range(struct file *file,
				      struct briefs_exchange_range __user *argp);
static long briefs_ioc_start_commit(struct file *file,
				     struct briefs_commit_range __user *argp);
static long briefs_ioc_commit_range(struct file *file,
				    struct briefs_commit_range __user *argp);
static long briefs_ioc_swapext(struct file *file,
			       struct briefs_swapext __user *argp);

/* address_space_operations wrappers (kernel 6.12 folio-based APIs). */

/*
 * BrieFS regular-file data path: iomap.  Buffered read/write, writeback, and
 * folio reclaim all go through iomap (briefs_iomap_aops); no buffer_head ever
 * attaches to a data folio, so there is no iomap-read / buffer_head-write bio
 * mix on shared pages (the combination that crashed the kernel when only read
 * was moved to iomap).  Metadata stays on buffer_head regardless.
 */
const struct address_space_operations *briefs_get_file_aops(void)
{
	return &briefs_iomap_aops;
}


/*
 * briefs_zero_eof_tail - zero [eof, block_end) of the block containing @eof.
 *
 * Called when i_size is about to grow past a mid-block EOF (truncate-up, or a
 * write that jumps past EOF).  The tail [eof, block_end) is beyond the file's
 * valid data but inside the EOF block, which may carry mmap pollution (fsx -e
 * pollute_eofpage writes the fsx pattern into the page tail beyond EOF) or
 * stale data left over from a prior, smaller i_size.  Left unzeroed, that tail
 * leaks as valid data once i_size later grows past the block (generic/363: fsx
 * READ BAD DATA at the old EOF).
 *
 * Zero the pagecache folio's tail directly and leave it dirty; iomap writeback
 * (briefs_iomap_writepages -> briefs_writeback_ops) allocates the block, if not
 * already mapped, and persists the zeroed folio.  Assumes bs == PAGE_SIZE
 * (BrieFS 4K blocks on 4K pages); a cached folio is always uptodate.
 */
static void briefs_zero_eof_tail(struct address_space *mapping, loff_t eof)
{
	unsigned int bs = i_blocksize(mapping->host);
	struct folio *folio;
	unsigned int off;

	if ((eof & (bs - 1)) == 0)
		return;		/* block-aligned EOF: no tail to zero */

	folio = __filemap_get_folio(mapping, eof >> PAGE_SHIFT, FGP_LOCK, GFP_NOFS);
	if (IS_ERR_OR_NULL(folio))
		return;		/* not cached: nothing dirty to persist.
				 * __filemap_get_folio returns ERR_PTR(-ENOENT), not
				 * NULL, when FGP_LOCK is set and the folio is absent. */
	if (folio_test_uptodate(folio)) {
		off = eof & (bs - 1);
		folio_zero_segment(folio, off, folio_size(folio));
		folio_mark_dirty(folio);
	}
	folio_unlock(folio);
	folio_put(folio);
}

/* briefs_fsync - sync file data and metadata to disk */
int briefs_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
	struct inode *inode = file->f_mapping->host;
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	int ret;

	/* A forced shutdown (XFS_IOC_GOINGDOWN, metadata write error, or block
	 * device removal) is a terminal error state, not a deliberate read-only
	 * transition.  fsync must return -EIO, matching XFS's xfs_file_fsync; the
	 * SB_RDONLY bit that briefs_force_shutdown also sets would otherwise let
	 * the journal-write path surface -EROFS (generic/623). */
	if (briefs_sb_shutdown(inode->i_sb))
		return -EIO;

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	ret = sync_inode_metadata(inode, 1);
	if (ret)
		return ret;

	/* Phase 3a: Capture and write a fresh inode snapshot after data writeback
	 * completes. This ensures the JRN_INODE_FULL record reflects the current
	 * state (extent list + size) after writeback, not any stale pending state.
	 * We write directly instead of using the pending mechanism to avoid races
	 * with concurrent operations.
	 */
	{
		struct briefs_disk_inode disk_di;
		struct briefs_inode_info *binfo = briefs_i(inode);
		struct jrn_inode_full rec;

		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);

		/* Drain any B-tree index blocks before writing the snapshot. */
		if (le32_to_cpu(disk_di.flags) & InodeFlagIndexed) {
			u64 base = le64_to_cpu(disk_di.extent_inline_base);
			u64 total = le64_to_cpu(disk_di.num_extents_total);
			u64 cap = total + 16;
			if (cap > (1ull << 20))
				cap = 1ull << 20;
			if (base != 0) {
				ret = briefs_btree_drain(inode->i_sb, base, cap);
				if (ret)
					return ret;
			}
		}

		memset(&rec, 0, sizeof(rec));
		rec.ino = cpu_to_le64(inode->i_ino);
		memcpy(rec.inode_data, &disk_di, sizeof(struct briefs_disk_inode));

		ret = briefs_journal_write_record(bsi->journal, JRN_INODE_FULL, &rec, sizeof(rec));
		if (ret)
			return ret;
	}

	/* Flush journal to disk on explicit fsync */
	if (bsi->journal && bsi->journal->dirty) {
		ret = briefs_journal_sync(bsi->journal);
		if (ret)
			return ret;
	}

	/*
	 * Issue a drive-level flush.  In older kernels sync_blockdev() also did
	 * this, but since Linux 6.12 it only flushes the block device's page-cache
	 * mapping and no longer submits a REQ_PREFLUSH bio.  Without this explicit
	 * flush, volatile-cache devices (and the dm-log-writes test target, which
	 * only logs writes once a flush/FUA moves them out of its unflushed queue)
	 * can reorder or omit the data/metadata writes that fsync(2) is supposed
	 * to make durable, causing generic/455 crash-replay md5 mismatches.
	 */
	ret = blkdev_issue_flush(inode->i_sb->s_bdev);
	if (ret)
		return ret;

	return 0;
}
/* Open file */
int briefs_open(struct inode *inode, struct file *file) {
	pr_debug("briefs: open inode %lu\n", inode->i_ino);

	/* Direct I/O is handled by the iter functions (briefs_read_iter /
	 * briefs_write_iter route IOCB_DIRECT to iomap_dio_rw); nothing to do
	 * here.  The aops .direct_IO is noop_direct_IO, only there so dentry_open()
	 * allows O_DIRECT opens; the iomap DIO path bypasses the aops
	 * write_begin/end.
	 */
	return 0;
}
/* Release file */
int briefs_release(struct inode *inode, struct file *file) {
	pr_debug("briefs: release inode %lu\n", inode->i_ino);
	return 0;
}

/*
 * briefs_ioctl - file and directory ioctl handler.
 *
 * BrieFS carries no xfs-style extended inode attributes (xflags, extsize,
 * projid, CoW extent size), so the one ioctl userspace routinely issues that
 * we must answer is FS_IOC_FSGETXATTR: xfs_io(1)'s `stat` command issues it on
 * every stat call, and on ENOTTY prints "FS_IOC_GETXATTR: Inappropriate ioctl
 * for device" to stderr.  That stderr is not stripped by tests that filter
 * only stdout (generic/169's _show_wrote_and_stat_only, generic/420's
 * `grep -F stat.size`), so the error line leaks into the compared output and
 * fails tests whose data is otherwise correct.  Return a zeroed fsxattr so
 * xfs_io prints the (stdout, filtered) fsx fields and emits no error line.
 *
 * FS_IOC_{GET,SET}FLAGS and FS_IOC_FS{GET,SET}XATTR are now handled by the VFS
 * through inode_operations::fileattr_get / fileattr_set, so this handler also
 * answers BRIEFS_IOC_GOINGDOWN, FITRIM (no super_op trim hook; handled here),
 * and FS_IOC_{GET,SET}FSLABEL (the on-disk label lives in the superblock),
 * rejecting anything else with -ENOTTY.
 */
long briefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	u32 flags;
	int ret;

	switch (cmd) {
	case BRIEFS_IOC_GOINGDOWN:
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (get_user(flags, (__u32 __user *)arg))
			return -EFAULT;
		/*
		 * mnt_want_write_file() returns -EROFS on a read-only mount
		 * WITHOUT taking a writer ref.  Pair mnt_drop_write_file() only
		 * with a successful want_write -- an unbalanced drop corrupts
		 * the mount writer count and triggers WARN_ON(mnt_get_writers)
		 * at umount (generic/599).  Allow an idempotent shutdown on an
		 * already read-only filesystem by proceeding past the -EROFS.
		 */
		ret = mnt_want_write_file(file);
		if (ret && ret != -EROFS)
			return ret;
		if (!ret) {
			int r2 = briefs_shutdown(inode->i_sb, flags);
			mnt_drop_write_file(file);
			return r2;
		}
		return briefs_shutdown(inode->i_sb, flags);

	case FITRIM:
	{
		struct super_block *sb = inode->i_sb;
		struct briefs_sb_info *bsi = sb->s_fs_info;
		struct fstrim_range range;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (!bdev_max_discard_sectors(sb->s_bdev))
			return -EOPNOTSUPP;
		/*
		 * norecovery mounts skip journal replay and present possibly
		 * stale free-space metadata; discarding from that would free
		 * blocks still referenced by the unreplayed journal.  Refuse,
		 * mirroring ext4/xfs (generic/537).
		 */
		if (bsi->mount_flags & BRIEFS_MF_NORECOVERY)
			return -EROFS;
		if (copy_from_user(&range, (struct fstrim_range __user *)arg,
				   sizeof(range)))
			return -EFAULT;
		ret = briefs_trim_fs(sb, &range);
		if (ret)
			return ret;
		if (copy_to_user((struct fstrim_range __user *)arg, &range,
				 sizeof(range)))
			return -EFAULT;
		return 0;
	}

	case FS_IOC_GETFSLABEL:
	{
		struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
		char label[FSLABEL_MAX] = {};

		lock_buffer(bsi->sb_bh);
		memcpy(label, bsi->sb->label, sizeof(bsi->sb->label));
		unlock_buffer(bsi->sb_bh);
		/* label[64] is null-padded, not terminated; the zeroed 256-byte
		 * buffer guarantees termination for userspace. */
		if (copy_to_user((char __user *)arg, label, sizeof(label)))
			return -EFAULT;
		return 0;
	}

	case FS_IOC_SETFSLABEL:
	{
		struct super_block *sb = inode->i_sb;
		struct briefs_sb_info *bsi = sb->s_fs_info;
		char label[FSLABEL_MAX];
		size_t len;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(label, (char __user *)arg, sizeof(label)))
			return -EFAULT;
		len = strnlen(label, sizeof(label));
		/* BrieFS stores the label in a fixed 64-byte, null-padded field
		 * (not null-terminated), so 64 chars is the most it can hold. */
		if (len > sizeof(bsi->sb->label))
			return -EINVAL;
		ret = mnt_want_write_file(file);
		if (ret)
			return ret;
		memset(label + len, 0, sizeof(label) - len);
		lock_buffer(bsi->sb_bh);
		memcpy(bsi->sb->label, label, sizeof(bsi->sb->label));
		unlock_buffer(bsi->sb_bh);
		mark_buffer_dirty(bsi->sb_bh);
		sync_dirty_buffer(bsi->sb_bh);
		mnt_drop_write_file(file);
		return 0;
	}

	case BRIEFS_IOC_EXCHANGE_RANGE:
		return briefs_ioc_exchange_range(file,
				(struct briefs_exchange_range __user *)arg);

	case BRIEFS_IOC_START_COMMIT:
		return briefs_ioc_start_commit(file,
				(struct briefs_commit_range __user *)arg);

	case BRIEFS_IOC_COMMIT_RANGE:
		return briefs_ioc_commit_range(file,
				(struct briefs_commit_range __user *)arg);

	case BRIEFS_IOC_SWAPEXT:
		return briefs_ioc_swapext(file,
				(struct briefs_swapext __user *)arg);

	default:
		return -ENOTTY;
	}
}

/*
 * briefs_fileattr_get - get user-visible inode flags (chattr/lsattr/statx).
 */
int briefs_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	u32 flags = binfo->disk_inode.user_flags & FS_COMMON_FL;

	if (!S_ISDIR(inode->i_mode))
		flags &= ~FS_DIRSYNC_FL;

	fileattr_fill_flags(fa, flags);
	fileattr_fill_xflags(fa, briefs_user_flags_to_xflags(flags));
	fa->fsx_extsize = 0;
	fa->fsx_nextents = binfo->disk_inode.num_extents_total;
	fa->fsx_projid = 0;
	fa->fsx_cowextsize = 0;
	return 0;
}

/*
 * briefs_fileattr_set - set user-visible inode flags (chattr/lsattr/statx).
 */
int briefs_fileattr_set(struct mnt_idmap *idmap, struct dentry *dentry,
                        struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_disk_inode disk_di;
	u32 old_flags, new_flags, changed;
	unsigned int new_iflags;
	int ret;

	old_flags = binfo->disk_inode.user_flags;

	if (fa->flags_valid) {
		if (fa->flags & ~FS_COMMON_FL)
			return -EOPNOTSUPP;
		new_flags = (old_flags & ~FS_COMMON_FL) | (fa->flags & FS_COMMON_FL);
	} else if (fa->fsx_valid) {
		if (fa->fsx_xflags & ~BRIEFS_XFLAG_SUPPORTED)
			return -EOPNOTSUPP;
		if (fa->fsx_extsize != 0 || fa->fsx_projid != 0 ||
		    fa->fsx_cowextsize != 0)
			return -EOPNOTSUPP;
		new_flags = (old_flags & ~FS_COMMON_FL) |
			    briefs_xflags_to_user_flags(fa->fsx_xflags);
	} else {
		return -EOPNOTSUPP;
	}

	/* DIRSYNC only valid on directories. */
	if ((new_flags & FS_DIRSYNC_FL) && !S_ISDIR(inode->i_mode))
		return -EINVAL;

	/* When setting immutable on a regular file, flush dirty data and DIO
	 * first so mmap pages become readonly and cannot be written around the
	 * flag. */
	if (S_ISREG(inode->i_mode) && !(old_flags & FS_IMMUTABLE_FL) &&
	    (new_flags & FS_IMMUTABLE_FL)) {
		inode_dio_wait(inode);
		ret = filemap_write_and_wait(inode->i_mapping);
		if (ret)
			return ret;
	}

	changed = old_flags ^ new_flags;
	if (!changed)
		return 0;

	binfo->disk_inode.user_flags = new_flags;

	new_iflags = briefs_user_flags_to_iflags(new_flags);
	inode_set_flags(inode, new_iflags,
			S_SYNC | S_APPEND | S_IMMUTABLE | S_NOATIME | S_DIRSYNC);

	inode_set_ctime_to_ts(inode, current_time(inode));
	mark_inode_dirty(inode);

	/* Journal a full snapshot so the change survives crash+replay. */
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	ret = briefs_journal_inode_full(bsi->journal, inode, &disk_di);
	if (ret) {
		/* Roll back on journal failure so in-memory and on-disk match. */
		binfo->disk_inode.user_flags = old_flags;
		briefs_apply_inode_flags(inode);
		return ret;
	}

	ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * briefs_inode_sync - flush metadata + journal + drive for IS_SYNC inodes.
 *
 * Phase 3a: Also flushes the inode's pending journal snapshot (if any) before
 * syncing the journal, ensuring deferred JRN_INODE_FULL records are written.
 */
int briefs_inode_sync(struct inode *inode)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	int ret;

	ret = sync_inode_metadata(inode, 1);
	if (ret)
		return ret;

	if (!IS_SYNC(inode) && !IS_DIRSYNC(inode))
		return 0;

	/* Phase 3a: Flush this inode's pending journal snapshot before syncing. */
	ret = briefs_flush_inode_pending_journal_snapshot(bsi->journal, inode);
	if (ret)
		return ret;

	ret = briefs_journal_sync(bsi->journal);
	if (ret)
		return ret;

	return blkdev_issue_flush(inode->i_sb->s_bdev);
}

/*
 * briefs_promote_inline_data - convert an inline-data inode to extent-backed.
 *
 * Allocates a single data block, copies the existing inline content into it,
 * and updates the inode to reference the block with an inline extent.  The
 * caller must hold inode_lock and should invalidate the page cache if any
 * inline folios may be present.
 */
static int briefs_promote_inline_data(struct inode *inode)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 old_size = inode->i_size;
	u64 rel, phys;
	int err;
	struct buffer_head *bh;

	if (!(binfo->disk_inode.flags & InodeFlagInlineData))
		return 0;

	/* Empty inline file: just clear the flag, let the write path allocate. */
	if (old_size == 0) {
		briefs_extent_write_begin(binfo);
		binfo->disk_inode.flags &= ~InodeFlagInlineData;
		briefs_extent_write_end(binfo);
		return 0;
	}

	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return -ENOSPC;
	phys = data_to_abs(bsi->sb, rel);

	bh = sb_bread(inode->i_sb, phys);
	if (!bh) {
		briefs_free_block(&bsi->alloc, rel);
		return -EIO;
	}
	memset(bh->b_data, 0, inode->i_sb->s_blocksize);
	memcpy(bh->b_data, binfo->disk_inode.inline_data, old_size);
	briefs_mark_buffer_dirty(bh, inode->i_sb);
	err = briefs_sync_dirty_buffer(bh, inode->i_sb, "promote inline data");
	brelse(bh);
	if (err) {
		briefs_free_block(&bsi->alloc, rel);
		return err;
	}

	briefs_extent_write_begin(binfo);
	binfo->disk_inode.flags &= ~InodeFlagInlineData;
	memset(binfo->disk_inode.inline_extents, 0,
	       sizeof(binfo->disk_inode.inline_extents));
	binfo->disk_inode.inline_extents[0].offset = 0;
	binfo->disk_inode.inline_extents[0].phys = phys;
	binfo->disk_inode.inline_extents[0].len = 1;
	binfo->disk_inode.inline_extents[0].flags = 0;
	binfo->disk_inode.num_extents_inline = 1;
	binfo->disk_inode.num_extents_total = 1;
	binfo->disk_inode.extent_inline_base = 0;
	/* Promotion bypasses briefs_btree_insert_locked, so update the tail cache
	 * here: the promoted extent is {offset 0, len 1} -> end 1. */
	if (binfo->cached_max_end < 1)
		binfo->cached_max_end = 1;
	briefs_extent_write_end(binfo);

	inode->i_blocks = (BRIEFS_BLOCK_SIZE / 512);

	briefs_journal_extent_alloc(bsi->journal, inode->i_ino, 0, phys, 1, 0);
	{
		struct briefs_disk_inode disk_di;
		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_journal_inode_full(bsi->journal, inode, &disk_di);
	}
	briefs_persist_disk_inode(inode->i_sb, inode->i_ino, &binfo->disk_inode, false);

	pr_debug("briefs: promoted inline inode %lu to block %llu\n",
		 inode->i_ino, phys);
	return 0;
}

/*
 * briefs_read_iter - read from a regular file.
 *
 * Inline-data inodes are served directly from the inode without touching the
 * page cache.  Extent-backed files fall back to the generic page-cache path.
 */
ssize_t briefs_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct briefs_inode_info *binfo = briefs_i(inode);

	/* After a forced shutdown (e.g. the block device was removed under the
	 * mount) reads must return -EIO even when the page is still cached,
	 * mirroring ext4_file_read_iter; the SB_RDONLY bit alone would let the
	 * cached page be served (generic/730). */
	if (unlikely(briefs_sb_shutdown(inode->i_sb)))
		return -EIO;

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		size_t count = iov_iter_count(to);
		loff_t pos = iocb->ki_pos;
		size_t available;
		size_t copied;
		u8 tmp[BRIEFS_INODE_INLINE_DATA_SIZE];

		inode_lock(inode);
		if (pos >= inode->i_size) {
			inode_unlock(inode);
			return 0;
		}
		available = inode->i_size - pos;
		copied = min(count, available);

		memcpy(tmp, binfo->disk_inode.inline_data + pos, copied);
		if (copy_to_iter(tmp, copied, to) != copied) {
			inode_unlock(inode);
			return -EFAULT;
		}
		iocb->ki_pos += copied;
		inode_unlock(inode);
		/* Inline reads bypass generic_file_read_iter, which would otherwise
		 * call file_accessed() for us.  Without this the VFS i_atime is never
		 * updated for inline-data reads (generic/003 atime check failures). */
		file_accessed(iocb->ki_filp);
		return copied;
	}

	/* Direct reads bypass the page cache.  Take the inode lock shared to
	 * exclude concurrent writers (which hold it exclusive); iomap_dio_rw
	 * manages the inode_dio count itself.  No dops: reads need no completion
	 * work.  Inline-data inodes stay on the inline bypass above (their data
	 * lives in the inode block, so a "direct" read is just the memcpy there).
	 */
	if (iocb->ki_flags & IOCB_DIRECT) {
		ssize_t ret;

		inode_lock_shared(inode);
		ret = iomap_dio_rw(iocb, to, &briefs_iomap_ops, NULL, 0, NULL, 0);
		inode_unlock_shared(inode);
		return ret;
	}

	/*
	 * Buffered reads go through the page cache.  Take the inode lock shared
	 * to exclude concurrent writers, which hold it exclusive
	 * (briefs_dio_write and briefs_iomap_buffered_write): without this a
	 * buffered read can race a DIO write to the same range, readahead a
	 * folio into the write range after the iomap core's best-effort
	 * post-write invalidate, and later serve pre-write bytes (generic/209:
	 * aio-dio-invalidate-readahead).  The iomap core's post-write invalidate
	 * is explicitly best-effort ("if it fails, tough"), so coherence with a
	 * racing DIO write rests on this lock, not on that invalidation.  This
	 * mirrors the direct-read branch above and completes BrieFS's
	 * writers-exclusive / readers-shared inode_lock model.
	 */
	{
		ssize_t ret;

		inode_lock_shared(inode);
		ret = generic_file_read_iter(iocb, to);
		inode_unlock_shared(inode);
		return ret;
	}
}

/*
 * briefs_iomap_buffered_write - extent-backed buffered write via iomap.
 *
 * Mirrors generic_file_write_iter / __generic_file_write_iter but drives the
 * page-cache copy through iomap_file_buffered_write (briefs_write_iomap_ops,
 * which allocates on a miss and converts unwritten extents in place) instead of
 * generic_perform_write -> aops.write_begin.  generic_write_checks handles
 * O_APPEND / O_TRUNC / RLIMIT_FSIZE (SIGXFSZ) / s_maxbytes exactly as the
 * buffer_head path does through generic_file_write_iter; file_update_time +
 * file_remove_privs supply the mtime/ctime/priv updates that the buffer_head
 * path got from __generic_file_write_iter (generic/003/313/423/755).  iomap
 * updates i_size itself; generic_write_sync honours IOCB_SYNC.  The inode lock
 * is taken here (the inline section above already released it).
 */
static ssize_t briefs_iomap_buffered_write(struct kiocb *iocb,
					    struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t old_size;
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;
	/* Zero the tail of the old EOF block before a write that jumps past EOF
	 * (pos > i_size).  That tail [i_size, block_end) is beyond the file's
	 * valid data but inside the already-allocated EOF block, so it may hold
	 * bytes written by an mmap store past EOF (fsx -e pollute_eofpage) or
	 * stale content from a prior smaller i_size.  iomap's __iomap_write_begin
	 * only zeroes IOMAP_F_NEW / unwritten / hole blocks, and this block is
	 * none of those, so the stale tail would leak as valid data once iomap
	 * extends i_size past it (generic/363: fsx READ BAD DATA at the old EOF).
	 * briefs_zero_eof_tail zeroes the cached folio's tail directly
	 * (path-agnostic pagecache manipulation, no buffer_heads).  Only the
	 * partial old-EOF block needs this; the write itself covers the new EOF
	 * block (iomap zeroes an IOMAP_F_NEW tail), and whole-block gaps in
	 * [old_size, pos) read as holes.  Inline-data inodes never reach here.
	 */
	if (iocb->ki_pos > i_size_read(inode))
		briefs_zero_eof_tail(inode->i_mapping, i_size_read(inode));
	ret = file_remove_privs(iocb->ki_filp);
	if (ret)
		goto out_unlock;
	ret = file_update_time(iocb->ki_filp);
	if (ret)
		goto out_unlock;
	old_size = i_size_read(inode);
	ret = iomap_file_buffered_write(iocb, from, &briefs_write_iomap_ops,
					 NULL);
	/*
	 * iomap_file_buffered_write grows i_size but does not always mark the
	 * inode dirty (e.g., when the write lands in an already-allocated run and
	 * file_update_time decided no cmtime update was needed).  If the write
	 * extended the file, force a dirty so briefs_write_inode() will persist
	 * the new size for sync+shutdown durability (generic/048).
	 */
	if (ret > 0 && i_size_read(inode) > old_size)
		mark_inode_dirty(inode);
out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/*
 * briefs_dio_write - extent-backed direct-I/O write.
 *
 * Mirrors briefs_iomap_buffered_write's checks (generic_write_checks for
 * O_APPEND / O_TRUNC / RLIMIT_FSIZE / s_maxbytes; file_remove_privs +
 * file_update_time for the metadata the buffer_head path got from
 * __generic_file_write_iter) but drives iomap_dio_rw instead of the page-cache
 * copy.  briefs_write_iomap_ops is reused: begin allocates on a write miss
 * (returning IOMAP_F_NEW, whose head/tail iomap_dio zeroes) and converts an
 * unwritten extent in place, so a DIO write that extends the file allocates the
 * new blocks directly written (no delalloc, no unwritten conversion needed in
 * end_io).  briefs_dio_write_ops.end_io updates i_size on a file-extending
 * write.  -ENOTBLK means the page cache could not be invalidated (the range is
 * mmap'd); the caller falls back to briefs_iomap_buffered_write.  inline-data
 * inodes never reach here (the inline section in briefs_write_iter handles
 * them, and they are already effectively direct).
 */
static ssize_t briefs_dio_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;
	/* Unaligned direct writes (a sub-block head or tail) must be serialized
	 * against concurrent direct writes.  iomap zeroes the IOMAP_F_NEW head/tail
	 * of a partial block (need_zeroout) and, for a write at/ past i_size, the
	 * tail beyond the write; that zero bio is submitted as part of this DIO.
	 * If a concurrent writer maps the same block first, the first writer sees
	 * IOMAP_F_NEW and zeroes the partial edge while the second (hitting the
	 * now-existing extent) does a sub-block bio with no zero -- and the first
	 * writer's zero bio can land AFTER the second's full write, clobbering it
	 * (generic/551: overlapping AIO DIO writes, zero window exactly at a
	 * concurrent unaligned write's end-of-block).  Drain outstanding DIO first
	 * so unaligned DIO cannot overlap another in-flight DIO, as XFS does for
	 * unaligned_io.  Aligned full-block DIO need not drain: briefs sizes each
	 * allocated extent to the write's full-block count, so no IOMAP_F_NEW tail
	 * extends past the write, and full-block writes have no edge to zero (last
	 * writer wins per block).  NOWAIT cannot block to drain; the race is then
	 * inherent to non-blocking overlapping DIO.
	 */
	if (!(iocb->ki_flags & IOCB_NOWAIT) &&
	    (iocb->ki_pos & (BRIEFS_BLOCK_SIZE - 1) ||
	     iov_iter_count(from) & (BRIEFS_BLOCK_SIZE - 1)))
		inode_dio_wait(inode);
	ret = file_remove_privs(iocb->ki_filp);
	if (ret)
		goto out_unlock;
	ret = file_update_time(iocb->ki_filp);
	if (ret)
		goto out_unlock;
	/*
	 * DIO write coherence with concurrent buffered reads is handled on the
	 * read side: briefs_read_iter takes inode_lock shared, so a buffered
	 * read (and its readahead) cannot run against this range while we hold
	 * inode_lock exclusive here.  The iomap core's own pre/post-write
	 * pagecache invalidate is best-effort by design, so the lock -- not that
	 * invalidation -- is what makes generic/209 deterministic.
	 */
	ret = iomap_dio_rw(iocb, from, &briefs_write_iomap_ops,
			   &briefs_dio_write_ops, 0, NULL, 0);
out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}

/*
 * briefs_write_iter - write to a regular file.
 *
 * Small writes that fit inside the 256-byte inline region are stored directly
 * in the inode.  Larger writes promote the inode to extent-backed and then use
 * the generic page-cache path.
 */
ssize_t briefs_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct briefs_inode_info *binfo = briefs_i(inode);
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(from);
	size_t total_size;
	int ret;

	if (count == 0)
		return 0;

	/* O_APPEND writes start at the current end of file. */
	if (iocb->ki_flags & IOCB_APPEND)
		pos = inode->i_size;

	total_size = pos + count;

	inode_lock(inode);

	/* The inline-data path bypasses briefs_iomap_buffered_write (which
	 * calls file_remove_privs for the extent-backed path), so strip
	 * suid/sgid and clear security.capability here.  Without this an
	 * append to a small inline file carrying capabilities leaves the cap
	 * xattr in place (generic/093: append must clear capabilities).
	 * Under inode_lock, as file_remove_privs' notify_change requires;
	 * the promote/extent path below re-calls it as a harmless no-op.
	 */
	ret = file_remove_privs(iocb->ki_filp);
	if (ret) {
		inode_unlock(inode);
		return ret;
	}

	if ((binfo->disk_inode.flags & InodeFlagInlineData) || inode->i_size == 0) {
		if (total_size <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			u8 tmp[BRIEFS_INODE_INLINE_DATA_SIZE];

			if (copy_from_iter(tmp, count, from) != count) {
				inode_unlock(inode);
				return -EFAULT;
			}
			memcpy(binfo->disk_inode.inline_data + pos, tmp, count);

			briefs_extent_write_begin(binfo);
			binfo->disk_inode.flags |= InodeFlagInlineData;
			if (total_size > inode->i_size) {
				inode->i_size = total_size;
				binfo->disk_inode.filesize = total_size;
			}
			briefs_extent_write_end(binfo);

			inode->i_blocks = 0;
			/* Inline writes bypass generic_file_write_iter, whose
			 * file_update_time() would otherwise set i_mtime/i_ctime.  Set
			 * them here so a following write_inode persists the change via
			 * briefs_sync_inode_times() (generic/003 mtime/ctime checks). */
			{
				struct timespec64 now;

				now = current_time(inode);
				inode->i_mtime_sec = now.tv_sec;
				inode->i_mtime_nsec = now.tv_nsec;
				inode->i_ctime_sec = now.tv_sec;
				inode->i_ctime_nsec = now.tv_nsec;
			}
			mark_inode_dirty(inode);

			iocb->ki_pos += count;
			inode_unlock(inode);
			return generic_write_sync(iocb, count);
		}

		/* Write exceeds inline capacity: promote the inode first. */
		ret = briefs_promote_inline_data(inode);
		if (ret) {
			inode_unlock(inode);
			return ret;
		}
		truncate_inode_pages(inode->i_mapping, 0);
	}

	inode_unlock(inode);
	/* Direct writes bypass the page cache.  On -ENOTBLK (page cache could not
	 * be invalidated, e.g. the range is mmap'd) fall back to the buffered path,
	 * as the iomap DIO callers (zonefs/gfs2) do.  generic_write_checks truncates
	 * `from` rather than advancing it, so a -ENOTBLK fallback to
	 * briefs_iomap_buffered_write re-checks idempotently and writes the same
	 * (truncated) range.
	 */
	if (iocb->ki_flags & IOCB_DIRECT) {
		ssize_t ret = briefs_dio_write(iocb, from);

		if (ret != -ENOTBLK)
			return ret;
		/* Fall back to buffered I/O. */
	}
	return briefs_iomap_buffered_write(iocb, from);
}

/*
 * Collect+rebuild helpers, shared by truncate and punch hole (the P2
 * stand-ins for the P4 tree-range operations).
 *
 * briefs_collect_all_extents: walk the index in offset order (inline array or
 * B+ tree leaves via next_leaf) into a freshly kvmalloc'd array. Returns 0
 * with exts/n set (exts is NULL iff n == 0), or -ENOMEM/-EIO.
 */
static int briefs_count_cb(const struct briefs_extent *ext, void *ctx)
{
	(*(int *)ctx)++;
	return 0;
}

struct briefs_fill_ctx {
	struct briefs_extent *arr;
	int n;
};

static int briefs_fill_cb(const struct briefs_extent *ext, void *ctx)
{
	struct briefs_fill_ctx *c = ctx;
	c->arr[c->n++] = *ext;
	return 0;
}

static int briefs_collect_all_extents(struct super_block *sb,
				      struct briefs_inode *di,
				      struct briefs_extent **exts, int *n)
{
	int count = 0, ret;
	struct briefs_fill_ctx ctx;

	*exts = NULL;
	*n = 0;

	ret = briefs_btree_for_each_extent(sb, di, briefs_count_cb, &count);
	if (ret)
		return ret;
	if (count == 0)
		return 0;

	ctx.arr = kvmalloc_array(count, sizeof(struct briefs_extent),
				 GFP_KERNEL);
	if (!ctx.arr)
		return -ENOMEM;
	ctx.n = 0;

	ret = briefs_btree_for_each_extent(sb, di, briefs_fill_cb, &ctx);
	if (ret) {
		kvfree(ctx.arr);
		return ret;
	}
	*exts = ctx.arr;
	*n = ctx.n;
	return 0;
}

/*
 * briefs_rebuild_extent_list - rebuild an inode's extent index from @exts[0..n-1]
 * (which MUST be sorted by offset and non-overlapping). Caller holds
 * extent_lock. Frees the old tree NODE blocks (the data blocks referenced by
 * kept extents are left allocated and re-inserted; the caller frees removed
 * data separately), resets the inode to empty inline-only, then re-inserts each
 * kept extent via the tree/inline mutator (which restores InodeFlagIndexed on
 * spill and raises cached_max_end). Returns 0 or -errno.
 */
static int briefs_rebuild_extent_list(struct super_block *sb,
				       struct briefs_inode_info *binfo,
				       struct briefs_extent *exts, int n)
{
	struct briefs_inode *di = &binfo->disk_inode;
	int i, ret;

	/* Free old tree nodes (no-op for inline-only). Data blocks survive. */
	briefs_btree_free_nodes_only(sb, di);

	/* Reset to empty inline-only. */
	briefs_extent_write_begin(binfo);
	di->flags &= ~InodeFlagIndexed;
	di->extent_inline_base = 0;
	di->num_extents_inline = 0;
	di->num_extents_total = 0;
	memset(di->inline_extents, 0, sizeof(di->inline_extents));
	binfo->cached_max_end = 0;
	briefs_extent_write_end(binfo);

	/* Re-insert kept extents in offset order. */
	for (i = 0; i < n; i++) {
		ret = briefs_btree_insert_locked(sb, di, &exts[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* briefs_setattr - set file attributes (truncate support).
 * Frees data blocks on truncation, updates the inode on disk.
 */
int briefs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	u64 new_size, old_size, trunc_block;
	int ret;

	/*
	 * Validate the attribute change.  notify_change() calls this
	 * ->setattr directly and does NOT run setattr_prepare() for us, so the
	 * permission checks other filesystems inherit from it are ours to do:
	 *
	 *  - utime(2) semantics (generic/087): setting a *specific* timestamp
	 *    (ATTR_ATIME_SET / ATTR_MTIME_SET) requires the caller be the owner
	 *    or hold CAP_FOWNER; only setting the *current* time
	 *    (ATTR_ATIME / ATTR_MTIME) is permitted by write permission alone.
	 *    Without this check a non-owner with write access wrongly succeeds
	 *    at setting an arbitrary timestamp.
	 *  - size limits (generic/394): setattr_prepare()->inode_newsize_ok()
	 *    enforces RLIMIT_FSIZE / s_maxbytes on a grow, sending SIGXFSZ +
	 *    -EFBIG (this supersedes the explicit inode_newsize_ok() that was
	 *    here before).
	 *  - chown/chmod/setgid-strip POSIX permission rules.
	 *
	 * setattr_prepare() only validates; it applies nothing, so the size and
	 * out_copy paths below remain unchanged.  notify_change() holds
	 * inode_lock across ->setattr (do_truncate() for size), which
	 * inode_permission() inside inode_change_ok() is safe under.
	 */
	ret = setattr_prepare(idmap, dentry, attr);
	if (ret)
		return ret;

	/* Only handle size changes here. */
	if (!(attr->ia_valid & ATTR_SIZE))
		goto out_copy;

	new_size = attr->ia_size;
	old_size = inode->i_size;

	if (new_size == old_size) {
		/*
		 * Truncate to the current i_size.  ext4 still runs ext4_truncate()
		 * in this case (ia_size == oldsize) to release any preallocated
		 * blocks past i_size; do the same so a fallocate-then-truncate-to-
		 * i_size trims the preallocated tail (generic/092).  cached_max_end
		 * is an overstatement of the highest extent end (>= the real max,
		 * never under), so if it does not exceed trunc_block there is
		 * definitely nothing past i_size and this is a true no-op -- take
		 * the lightweight out_copy path.  Otherwise fall through to the
		 * shrink free-path below; its pagecache steps are guarded by
		 * (new_size < old_size) and so are skipped for the equal case,
		 * leaving just the extent-free past trunc_block.
		 */
		trunc_block = (new_size + BRIEFS_BLOCK_SIZE - 1) / BRIEFS_BLOCK_SIZE;
		if (binfo->disk_inode.num_extents_total == 0 ||
		    binfo->cached_max_end <= trunc_block)
			goto out_copy;
	}

	/* notify_change() translates the ATTR_KILL_SUID / ATTR_KILL_SGID bits
	 * that do_truncate() adds (via dentry_needs_remove_privs) into ATTR_MODE
	 * with the setid bits cleared before calling ->setattr.  The size-change
	 * paths below all return directly and never reach the out_copy
	 * setattr_copy() epilogue, so apply the stripped mode here and mirror it
	 * into the disk inode -- every truncate path persists/journals
	 * &binfo->disk_inode, so they all pick up the new mode.  Without this a
	 * truncate of a setid file by an unprivileged (non-CAP_FSETID) caller
	 * leaves the setid bits set (generic/193).  For a privileged caller
	 * dentry_needs_remove_privs() returns 0, ATTR_MODE is absent, and the
	 * mode is untouched.  Capability clearing on truncate is handled earlier
	 * by notify_change()'s security_inode_killpriv() (ATTR_KILL_PRIV), not
	 * here.  The equal-size no-op case above jumped to out_copy, whose
	 * setattr_copy() applies ATTR_MODE itself.
	 */
	if (attr->ia_valid & ATTR_MODE) {
		inode->i_mode = attr->ia_mode;
		binfo->disk_inode.filemode = inode->i_mode;
	}

	/* A size change updates mtime and ctime.  notify_change() populates
	 * attr->ia_mtime/ia_ctime but, for a plain truncate, does NOT set the
	 * ATTR_MTIME/ATTR_CTIME bits in ia_valid, so setattr_copy() (in the
	 * out_copy epilogue, which these truncate paths bypass) would skip
	 * them.  Set both to the same current_time() so ctime==mtime (the
	 * ctime>=mtime invariant generic/423 checks) and mirror them into the
	 * disk inode once here — every truncate path below persists/journals
	 * &binfo->disk_inode, so they all pick up the new times.  This is the
	 * generic/313 "update timestamps on truncate" expectation (cf. btrfs
	 * 3972f26). */
	{
		struct timespec64 now = current_time(inode);
		inode_set_mtime_to_ts(inode, now);
		inode_set_ctime_to_ts(inode, now);
		briefs_sync_inode_times(inode, &binfo->disk_inode);
	}

	if (new_size < old_size)
		briefs_stat_inc(bsi, truncate_calls);

	/*
	 * Truncate the pagecache beyond the new size BEFORE taking extent_lock.
	 * truncate_setsize() -> truncate_pagecache() waits for any in-flight
	 * writeback on the pages it drops; meanwhile ->writepages
	 * (briefs_iomap_writepages -> briefs_writeback_ops ->
	 * briefs_append_extent_nojournal) acquires extent_lock to map blocks.
	 * Holding extent_lock across that wait deadlocks writeback against
	 * truncate (AB-BA): the writeback of the very pages truncate is waiting
	 * on blocks on the lock truncate holds.  This is the generic/074 mmap
	 * hang (kworker flush wedged in briefs_append_extent_nojournal, fstest
	 * wedged in truncate_pagecache).
	 *
	 * Doing this first is also the correct block-freeing order: by the time
	 * we drop extent_lock below and free the backing extents' data blocks,
	 * their in-flight writeback has already completed, so no bio targets a
	 * block we are about to free.  Inline-data truncates manage their bytes
	 * directly (no pagecache wait under the lock), so they stay under
	 * extent_lock below as before and skip this pre-lock step.
	 */
	if (new_size < old_size &&
	    !(binfo->disk_inode.flags & InodeFlagInlineData)) {
		/* Zero and dirty the tail of the block containing new_size so the
		 * stale bytes [new_size, end-of-block) persist as zeros on disk.
		 * truncate_setsize() below zeroes that tail in the pagecache but
		 * does NOT mark it dirty, so without this the retained tail block
		 * keeps its pre-truncate data on disk; a later file extension that
		 * spans the old EOF then reads back that stale content
		 * (generic/363 EOF-pollution: TRUNCATE DOWN then WRITE-HOLE leaves
		 * the gap non-zero).  iomap_truncate_page handles this without
		 * attaching buffer_heads to the data folio; a buffer_head on a data
		 * folio is fatal here (iomap's iomap_set_range_uptodate reads
		 * folio->private as a struct iomap_folio_state and spins on that
		 * memory as ifs->state_lock -> soft lockup + IPI freeze, generic/075
		 * fsx truncate-then-write).  Called before extent_lock and without
		 * waiting on writeback (unlike truncate_setsize's truncate_pagecache),
		 * so there is no AB-BA with the writeback path described below. */
		{
			bool did_zero = false;

			/* Use the READ ops, not the write ops.  A write begin would
			 * CONVERT an unwritten EOF block to mapped (it clears
			 * BRIEFS_EXT_UNWRITTEN and splits the extent) yet still
			 * report the block to the iomap core as IOMAP_UNWRITTEN (the
			 * stale pre-conversion extent copy).  iomap_zero_iter treats
			 * IOMAP_UNWRITTEN as "already zero" and skips the zero-write
			 * entirely, so the conversion would flip the block to mapped
			 * on disk WITHOUT ever writing the zeros -- exposing the
			 * un-zeroed fallocate-time stale data on the next read
			 * (generic/363: punch -> fallocate-unwritten -> truncate-down
			 * into the unwritten EOF block -> mapread returns stale).
			 *
			 * The read ops report an unwritten block truthfully, so
			 * iomap_zero_iter no-ops (correct: an unwritten block reads
			 * back as zeros) and the block stays unwritten -- no stale
			 * disk is ever exposed.  A mapped EOF block still reports
			 * IOMAP_MAPPED and takes the zero-write path exactly as before
			 * (the tail is zeroed and dirtied for writeback); a hole
			 * no-ops.  The read ops never enter locked_create, so no
			 * extent_lock is taken here, preserving the no-AB-BA ordering
			 * with the writeback path explained above.
			 */
			ret = iomap_truncate_page(inode, new_size, &did_zero,
						  &briefs_iomap_ops);
			if (ret)
				return ret;
		}
		truncate_setsize(inode, new_size);
	}

	/* Symmetric zeroing for a GROW (truncate-up): zero+dirty the tail of the
	 * block containing old_size, [old_size, block_end).  That tail is beyond
	 * the old EOF but inside the EOF block, so it may hold bytes written by an
	 * mmap store past EOF (fsx -e EOF pollution) or stale content from a prior
	 * smaller i_size; once truncate_setsize grows i_size past the block,
	 * writeback's [i_size,block_end) zeroing no longer covers it and the stale
	 * tail leaks as valid data (generic/363).  briefs_zero_eof_tail zeroes the
	 * cached folio's tail directly (path-agnostic pagecache manipulation, no
	 * buffer_heads).  Inline-data grows manage their bytes directly under
	 * extent_lock below, so they are excluded.  Pre-extent_lock: this helper
	 * takes no locks and waits on no writeback.  Only the partial old-EOF block
	 * needs this; whole-block gaps in [old_size,new_size) read as zeros. */
	if (new_size > old_size &&
	    !(binfo->disk_inode.flags & InodeFlagInlineData) &&
	    (old_size & (BRIEFS_BLOCK_SIZE - 1)))
		briefs_zero_eof_tail(inode->i_mapping, old_size);

	/*
	 * From this point on we may manipulate the extent list or the chain
	 * blocks that back it.  Hold the per-inode extent lock to serialize
	 * with concurrent appends (briefs_append_extent) and with writeback
	 * that maps blocks through the extent list.
	 */
	mutex_lock(&binfo->extent_lock);

	/*
	 * Growing an inline-data inode: zero-fill the gap and, if the new
	 * size exceeds the inline region, promote to an extent-backed file.
	 */
	if (new_size > old_size && (binfo->disk_inode.flags & InodeFlagInlineData)) {
		if (new_size <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			briefs_extent_write_begin(binfo);
			memset(binfo->disk_inode.inline_data + old_size, 0,
			       new_size - old_size);
			binfo->disk_inode.filesize = new_size;
			inode->i_size = new_size;
			inode->i_blocks = 0;
			briefs_extent_write_end(binfo);

			briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
					&binfo->disk_inode);
		} else {
			ret = briefs_promote_inline_data(inode);
			if (ret)
				goto out_unlock;

			/* Promotion allocated one block; set the new size. */
			binfo->disk_inode.filesize = new_size;
			inode->i_size = new_size;
			inode->i_blocks = (BRIEFS_BLOCK_SIZE / 512);
			briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
					&binfo->disk_inode);
		}
		mutex_unlock(&binfo->extent_lock);
		ret = briefs_inode_sync(inode);
		return ret;
	}

	/* Extending an extent-backed file: the new range [old_size, new_size) is
	 * a hole that reads as zeros — no extents are allocated.  Grow i_size,
	 * persist the new size, and journal an inode snapshot so a crash after a
	 * following fsync (which advances journal_log_end past this record)
	 * recovers the extended size.  This mirrors what the shrinking path below
	 * does.  Without it, the old code fell through to out_copy, whose
	 * setattr_copy() does NOT set i_size (by kernel contract size must be set
	 * via truncate_setsize()), so i_size stayed at the pre-extend value and the
	 * fsync journaled the stale size — generic/101 (truncate-down then -up then
	 * fsync) recovered the pre-extend size.
	 *
	 * truncate_setsize() for a grow does not wait on pagecache writeback (no
	 * pages beyond the new size to drop), so unlike the shrinking
	 * truncate_setsize() above (which runs before extent_lock to avoid the
	 * writeback AB-BA of generic/074) this is safe under the lock.
	 */
	if (new_size > old_size) {
		truncate_setsize(inode, new_size);
		binfo->disk_inode.filesize = new_size;
		briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
				&binfo->disk_inode);
		mutex_unlock(&binfo->extent_lock);
		mark_inode_dirty(inode);
		ret = briefs_inode_sync(inode);
		return ret;
	}

	pr_debug("briefs: setattr truncate ino=%lu %llu -> %llu\n",
		inode->i_ino, old_size, new_size);

	/* Inline-data truncate is handled separately. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		briefs_extent_write_begin(binfo);
		if (new_size == 0) {
			binfo->disk_inode.flags &= ~InodeFlagInlineData;
			memset(binfo->disk_inode.inline_data, 0,
			       sizeof(binfo->disk_inode.inline_data));
		} else {
			memset(binfo->disk_inode.inline_data + new_size, 0,
			       old_size - new_size);
		}
		binfo->disk_inode.filesize = new_size;
		inode->i_size = new_size;
		inode->i_blocks = 0;
		briefs_extent_write_end(binfo);

		briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
				&binfo->disk_inode);
		mutex_unlock(&binfo->extent_lock);
		ret = briefs_inode_sync(inode);
		return ret;
	}

	trunc_block = (new_size + BRIEFS_BLOCK_SIZE - 1) / BRIEFS_BLOCK_SIZE;

	/* Nothing to do if the inode has no extents (just update size below). */
	if (binfo->disk_inode.num_extents_total != 0) {
		/*
		 * Tree-backed inodes: truncate is a range delete of [trunc_block,
		 * +inf). briefs_btree_delete_range frees the data blocks beyond
		 * trunc_block, shortens the straddler in place, drops emptied
		 * leaves, and recomputes cached_max_end — no collect+rebuild, no
		 * full tree teardown. If every extent is removed (truncate to 0)
		 * it clears InodeFlagIndexed / extent_inline_base / counts itself.
		 */
		if (binfo->disk_inode.flags & InodeFlagIndexed) {
			bool trunc_modified;

			ret = briefs_btree_delete_range(inode->i_sb,
							&binfo->disk_inode,
							trunc_block, U64_MAX,
							&trunc_modified);
			if (ret)
				goto out_unlock;
		} else {
			/*
			 * Inline-only truncate: walk extents in offset order, build
			 * the kept list (extents entirely before trunc_block kept
			 * whole; the straddler shortened; extents at/after
			 * trunc_block dropped), free the data blocks beyond
			 * trunc_block, then rebuild the inline index from the kept
			 * extents.
			 */
			struct briefs_extent *old, *kept;
			int n_ext, n_kept = 0, j;
			u64 unwritten_freed = 0;

			ret = briefs_collect_all_extents(inode->i_sb,
							 &binfo->disk_inode,
							 &old, &n_ext);
			if (ret)
				goto out_unlock;

			kept = kvmalloc_array(n_ext ? n_ext : 1, sizeof(*kept),
					      GFP_KERNEL);
			if (!kept) {
				kvfree(old);
				ret = -ENOMEM;
				goto out_unlock;
			}

			for (j = 0; j < n_ext; j++) {
				struct briefs_extent *e = &old[j];
				u64 ext_start = e->offset;
				u64 ext_end = e->offset + e->len;

				if (ext_end <= trunc_block) {
					/* Entirely before truncation - keep whole. */
					kept[n_kept++] = *e;
				} else if (ext_start < trunc_block) {
					/* Straddler - keep the head, free the tail. */
					u64 keep_len = trunc_block - ext_start;
					u64 free_phys = e->phys + keep_len;
					u64 free_len = e->len - keep_len;

					if (e->flags & BRIEFS_EXT_UNWRITTEN)
						unwritten_freed += free_len;
					briefs_journal_extent_free(bsi->journal,
								   inode->i_ino,
								   e->offset + keep_len,
								   free_phys, free_len);
					briefs_free_blocks_range(bsi, free_phys, free_len);

					kept[n_kept].offset = e->offset;
					kept[n_kept].phys = e->phys;
					kept[n_kept].len = keep_len;
					kept[n_kept].flags = e->flags;
					n_kept++;
				} else {
					/* Entirely at/after truncation - drop, free data. */
					if (e->flags & BRIEFS_EXT_UNWRITTEN)
						unwritten_freed += e->len;
					briefs_journal_extent_free(bsi->journal,
								   inode->i_ino,
								   e->offset, e->phys,
								   e->len);
					briefs_free_blocks_range(bsi, e->phys, e->len);
				}
			}

			/* Release the metadata reservation held for the unwritten
			 * blocks just freed.  No-op when none of the truncated
			 * extents were unwritten. */
			briefs_release_unwritten_reserve(inode, unwritten_freed);

			/* Rebuild the index from the kept extents. Frees old tree
			 * nodes (none for inline-only), resets the inode, re-inserts
			 * kept (raising cached_max_end). */
			ret = briefs_rebuild_extent_list(inode->i_sb, binfo,
							 kept, n_kept);
			kvfree(kept);
			kvfree(old);
			if (ret)
				goto out_unlock;
		}
	}

	/* Update inode metadata */
	inode->i_size = new_size;
	binfo->disk_inode.filesize = new_size;
	inode->i_blocks = briefs_compute_i_blocks(inode->i_sb,
						  &binfo->disk_inode);

	/* Persist the inode to disk */
	/* Persist + journal: replay restores the exact extent list and size,
	 * not just the freed bitmap bits. */
	briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
			&binfo->disk_inode);

	mutex_unlock(&binfo->extent_lock);
	ret = briefs_inode_sync(inode);
	return ret;

out_unlock:
	mutex_unlock(&binfo->extent_lock);
out_copy:
	/* For non-size changes (chown/chmod/etc), copy attributes into the VFS
	 * inode AND mirror the VFS-derived fields into the in-memory disk inode.
	 * briefs_write_inode syncs uid/gid/mode/nlink into the on-disk buffer, but
	 * it does NOT write them back to binfo->disk_inode, so without this mirror
	 * any later operation that journals from binfo->disk_inode (a directory
	 * create/unlink/rename inside this inode, a punch, etc.) would emit a STALE
	 * uid/gid/mode/nlink -- and the last such journal record before a crash
	 * wins on replay, losing the chown (generic/547: a chown'd directory's
	 * uid/gid reverted to 0 after crash+replay because a later child create
	 * re-journaled the parent from the stale in-memory disk inode).
	 */
	setattr_copy(idmap, inode, attr);
	/* A chmod updates the mode; if the inode has an access ACL, its mask
	 * entry must be recomputed to match the new group class.  No-op when
	 * there is no ACL.  posix_acl_chmod -> .set_acl (briefs_set_acl)
	 * persists the updated ACL xattr and inode.  Mirrors ext2_setattr. */
	if (attr->ia_valid & ATTR_MODE) {
		ret = posix_acl_chmod(idmap, dentry, inode->i_mode);
		if (ret)
			return ret;
	}
	briefs_sync_inode_fields(inode, &binfo->disk_inode);
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	mark_inode_dirty(inode);
	ret = briefs_inode_sync(inode);
	return ret;
}
/* briefs_getattr - get file attributes */
int briefs_getattr(struct mnt_idmap *idmap, const struct path *path,

                   struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 i_blocks;

	generic_fillattr(idmap, request_mask, inode, stat);
	generic_fill_statx_attr(inode, stat);

	if (binfo->disk_inode.user_flags & BRIEFS_USER_FLAG_NODUMP)
		stat->attributes |= STATX_ATTR_NODUMP;
	stat->attributes_mask |= STATX_ATTR_NODUMP;

	stat->btime.tv_sec = binfo->disk_inode.creation_time_sec;
	stat->btime.tv_nsec = binfo->disk_inode.creation_time_nsec;
	stat->result_mask |= STATX_BTIME;

	/* Inline-data files consume no data blocks. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		i_blocks = 0;
	} else {
		/* Recompute i_blocks from all extents in a single in-order walk. */
		i_blocks = briefs_compute_i_blocks(inode->i_sb, &binfo->disk_inode);
	}
	inode->i_blocks = i_blocks;
	stat->blocks = i_blocks;
	return 0;
}

/* briefs_fiemap - report the file extent map (FS_IOC_FIEMAP).
 *
 * Delegates to iomap_fiemap, which walks briefs_iomap_ops and emits one
 * fiemap extent per iomap mapping, skipping holes (BrieFS has no hole extents:
 * punch-hole removes extents and frees their blocks, so holes are gaps in
 * logical coverage, implied by the gaps between emitted extents per the fiemap
 * spec).  iomap maps IOMAP_INLINE -> FIEMAP_EXTENT_DATA_INLINE and
 * IOMAP_UNWRITTEN -> FIEMAP_EXTENT_UNWRITTEN; FIEMAP_EXTENT_LAST is set by
 * iomap_fiemap itself on the final extent.
 *
 * The range flush before iomap_fiemap is the delayed-allocation case: written-
 * but-not-yet-written-back blocks are BH_Delay in the page cache and absent
 * from the extent list.  BrieFS has no extent-status tree (like ext4_es) to
 * synthesize DELALLOC extents, so we force writeback of the queried range
 * first and then report the now-allocated physical extents.  iomap_fiemap
 * re-runs fiemap_prep internally; that second call is harmless (len is already
 * clipped, and the incompat-flag check is idempotent).
 */
int briefs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		  u64 start, u64 len)
{
	int ret;

	ret = fiemap_prep(inode, fieinfo, start, &len, 0);
	if (ret)
		return ret;

	ret = filemap_write_and_wait_range(inode->i_mapping, start,
					  start + len - 1);
	if (ret)
		return ret;

	return iomap_fiemap(inode, fieinfo, start, len, &briefs_iomap_ops);
}

/*
 * briefs_block_mapped - return true if logical block @iblock already has a
 * physical mapping in the inode's extent index. O(log E) tree lookup (or inline
 * scan for inline-only inodes); verifies CRCs since this runs unlocked under
 * fallocate's inode_lock (not extent_lock). A torn read surfaces as "not
 * mapped", which is conservative (fallocate may re-zero an already-mapped
 * block — harmless) rather than a false positive.
 */
static bool briefs_block_mapped(struct inode *inode, u64 iblock)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;

	return briefs_inode_lookup_iblock(inode->i_sb, binfo, iblock, &ext,
					   false) == 0;
}

/*
 * briefs_zero_block_range - zero bytes [start, end) inside the physical data
 * block at @abs_block.  Used by punch-hole to zero the portion of a
 * partially-holed block that remains allocated.
 *
 * The zeroing goes through the block device with a synchronous READ + zero +
 * WRITE bio and never touches the bdev buffer cache.  This matters under the
 * iomap data path: file data lives in the inode page cache and direct-I/O
 * writes reach the disk via bios that bypass the buffer cache.  A data block
 * once sb_bread'd (by an earlier punch, or by the buffer_head fallback path)
 * stays cached in the bdev buffer cache with possibly-stale content; a later
 * DIO write to the reused block changes the disk under that cached buffer, so a
 * subsequent sb_bread here would hand back the stale buffer and
 * sync_dirty_buffer would write it back, clobbering the DIO-written data
 * (generic/091 and the rest of the fsx direct-I/O group).  Using our own bio
 * reads the current on-disk contents and writes the zeroed range back directly,
 * keeping data blocks out of the buffer cache entirely.
 *
 * The caller (punch-hole) is responsible for page-cache invalidation of the
 * logical range; this helper only touches the disk, mirroring the sb_bread
 * variant it replaces.
 */
static int briefs_zero_block_range(struct super_block *sb, u64 abs_block,
				   u32 start, u32 end)
{
	struct block_device *bdev = sb->s_bdev;
	u32 blocksize = sb->s_blocksize;
	sector_t sector = abs_block << (sb->s_blocksize_bits - SECTOR_SHIFT);
	struct page *page;
	struct bio *bio;
	int ret;

	if (start >= end)
		return 0;
	if (end > blocksize)
		return -EINVAL;

	page = alloc_page(GFP_NOFS);
	if (!page)
		return -ENOMEM;

	/* Read the current block contents fresh from disk. */
	bio = bio_alloc(bdev, 1, REQ_OP_READ, GFP_NOFS);
	if (!bio) {
		ret = -ENOMEM;
		goto out_page;
	}
	bio->bi_iter.bi_sector = sector;
	__bio_add_page(bio, page, blocksize, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);
	if (ret)
		goto out_page;

	/* Zero the requested sub-range and write the whole block back. */
	memset(page_address(page) + start, 0, end - start);

	bio = bio_alloc(bdev, 1, REQ_OP_WRITE, GFP_NOFS);
	if (!bio) {
		ret = -ENOMEM;
		goto out_page;
	}
	bio->bi_iter.bi_sector = sector;
	__bio_add_page(bio, page, blocksize, 0);
	ret = submit_bio_wait(bio);
	bio_put(bio);

out_page:
	put_page(page);
	return ret;
}

/*
 * briefs_do_punch_hole - create a hole in a regular file.
 *
 * Caller must hold inode_lock.  The file size must not change.
 */
static long briefs_do_punch_hole(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct timespec64 now;
	loff_t end = offset + len;
	u64 start_blk = offset >> BRIEFS_BLOCK_SHIFT;
	u64 end_blk = (end + BRIEFS_BLOCK_SIZE - 1) >> BRIEFS_BLOCK_SHIFT;
	struct briefs_extent *old_exts = NULL;
	struct briefs_extent *new_exts = NULL;
	int old_count, new_count = 0;
	bool changed = false;
	bool need_partial_start, need_partial_end;
	bool same_boundary_block;
	u32 partial_start_off, partial_end_off;
	int ret = 0;
	int i;


	need_partial_start = (offset & (BRIEFS_BLOCK_SIZE - 1)) != 0;
	need_partial_end = (end & (BRIEFS_BLOCK_SIZE - 1)) != 0;
	partial_start_off = offset & (BRIEFS_BLOCK_SIZE - 1);
	partial_end_off = end & (BRIEFS_BLOCK_SIZE - 1);
	/*
	 * Single-block punch: both the start and end of the punch land inside
	 * the same block (start_blk == end_blk - 1, both ends partial).  In that
	 * case the boundary block is one and the same, so the punched portion is
	 * the sub-range [partial_start_off, partial_end_off) -- NOT the union of
	 * [partial_start_off, BLOCK_SIZE) and [0, partial_end_off), which would
	 * cover the whole block.  Zeroing the whole block here would destroy the
	 * data outside the (small) punched range; the page cache masks this at
	 * runtime, but after a crash/replay (page cache gone) the block reads back
	 * as all-zero -- generic/059.  Handle this case with a single surgical
	 * zero of [partial_start_off, partial_end_off).
	 */
	same_boundary_block = need_partial_start && need_partial_end &&
			      (start_blk == end_blk - 1);

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		loff_t punch_start = max_t(loff_t, offset, 0);
		loff_t punch_end = min_t(loff_t, end, inode->i_size);


		if (punch_start < punch_end) {
			memset(binfo->disk_inode.inline_data + punch_start, 0,
			       punch_end - punch_start);
			changed = true;
		}

		if (changed) {
			now = current_time(inode);
			inode->i_mtime_sec = now.tv_sec;
			inode->i_mtime_nsec = now.tv_nsec;
			inode->i_ctime_sec = now.tv_sec;
			inode->i_ctime_nsec = now.tv_nsec;
			briefs_sync_inode_times(inode, &binfo->disk_inode);
			briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
					&binfo->disk_inode);
		}
		goto out_update;
	}

	/* With delayed allocation (#6), freshly-written blocks may still be
	 * BH_Delay in the page cache and not yet present in the extent list.
	 * Punch consults the extent list to decide what to free/zero, so a
	 * delayed block in the punch range would be missed -- the hole would
	 * never be zeroed and its (still-dirty) page would be written back
	 * later with the original data, leaking it past the punch.  ext4 solves
	 * this by writing the range back first to convert delalloc to real
	 * extents; do the same.  inode_lock is held, which is fine: writeback
	 * takes only the allocator spinlock and extent_seq, not inode_lock.
	 */
	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		goto out_free;

	if (binfo->disk_inode.num_extents_total > INT_MAX / 2) {
		ret = -ENOMEM;
		goto out_free;
	}

	/*
	 * Collect+rebuild punch stand-in (formalized as delete_range + insert
	 * remainders in P4). fallocate holds inode_lock but NOT extent_lock, so
	 * take extent_lock around the collect+rebuild+free to serialize against
	 * concurrent get_block inserters. The writeback flush above ran before
	 * the lock (writeback calls get_block, which takes extent_lock -> would
	 * self-deadlock if we held it here).
	 */
	mutex_lock(&binfo->extent_lock);

	if (binfo->disk_inode.flags & InodeFlagIndexed) {
		/*
		 * Tree-backed punch: delete the fully-punched interior blocks
		 * via briefs_btree_delete_range, then zero the partially-punched
		 * boundary blocks (which remain allocated as left/right
		 * straddlers). No collect+rebuild, no full tree teardown.
		 *
		 * The interior range is [start_blk, end_blk) with the boundary
		 * blocks excluded when the punch is not block-aligned: a partial
		 * start block (start_blk) and/or partial end block (end_blk-1)
		 * stay allocated and are zeroed in their punched portion only.
		 */
		u64 del_start = start_blk + (need_partial_start ? 1 : 0);
		u64 del_end = end_blk - (need_partial_end ? 1 : 0);
		u64 total_before = binfo->disk_inode.num_extents_total;


		if (total_before != 0) {
			bool del_modified = false;

			if (del_start < del_end) {
				ret = briefs_btree_delete_range(inode->i_sb,
								&binfo->disk_inode,
								del_start, del_end,
								&del_modified);
				pr_debug("briefs: btree_delete_range returned %d del_modified=%d\n", ret, del_modified);
				if (ret)
					goto out_unlock;
			}

			/*
			 * Zero the partially-punched boundary blocks. They remain
			 * allocated (delete_range kept them as straddlers), so look
			 * them up and zero the punched portion. -ENOENT means the
			 * boundary was already a hole: nothing to zero.
			 *
			 * CRITICAL: We must NOT do synchronous I/O (briefs_zero_block_range
			 * does submit_bio_wait) while holding extent_lock, or concurrent
			 * punch operations will deadlock waiting for the lock while I/O
			 * blocks. Release the lock, do the I/O, then re-acquire and verify.
			 */
			if (same_boundary_block) {
				struct briefs_extent pext;
				u64 ab;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 start_blk, &pext,
								 true);
				if (ret == 0) {
					ab = pext.phys + (start_blk - pext.offset);
				} else if (ret == -ENOENT) {
					ab = 0;
					ret = 0;  /* Hole is not an error - just nothing to zero */
				} else {
					goto out_unlock;
				}

				if (ab != 0) {
					mutex_unlock(&binfo->extent_lock);
					ret = briefs_zero_block_range(inode->i_sb, ab,
								      partial_start_off,
								      partial_end_off);
					mutex_lock(&binfo->extent_lock);
					if (ret)
						goto out_unlock;
				}
				changed = true;
			} else {
			if (need_partial_start) {
				struct briefs_extent pext;
				u64 ab;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 start_blk, &pext,
								 true);
				if (ret == 0) {
					ab = pext.phys + (start_blk - pext.offset);
				} else if (ret == -ENOENT) {
					ab = 0;
					ret = 0;  /* Hole is not an error - just nothing to zero */
				} else {
					goto out_unlock;
				}

				if (ab != 0) {
					mutex_unlock(&binfo->extent_lock);
					ret = briefs_zero_block_range(inode->i_sb, ab,
								      partial_start_off,
								      BRIEFS_BLOCK_SIZE);
					mutex_lock(&binfo->extent_lock);
					if (ret)
						goto out_unlock;
				}
				changed = true;
			}
			if (need_partial_end) {
				struct briefs_extent pext;
				u64 pblk = end_blk - 1;
				u64 ab;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 pblk, &pext,
								 true);
				if (ret == 0) {
					ab = pext.phys + (pblk - pext.offset);
				} else if (ret == -ENOENT) {
					ab = 0;
					ret = 0;  /* Hole is not an error */
				} else {
					goto out_unlock;
				}

				if (ab != 0) {
					mutex_unlock(&binfo->extent_lock);
					ret = briefs_zero_block_range(inode->i_sb, ab,
								      0, partial_end_off);
					mutex_lock(&binfo->extent_lock);
					if (ret)
						goto out_unlock;
				}
				changed = true;
			}
			}

			if (binfo->disk_inode.num_extents_total != total_before)
				changed = true;
			/* An extent split (interior blocks freed, a straddler
			 * kept) leaves num_extents_total unchanged, but the
			 * mapping DID change: the freed blocks are now holes and
			 * their (still-uptodate) pagecache pages hold pre-punch
			 * data. Without invalidating them, a later read returns
			 * that stale data (generic/522/616). del_modified reports
			 * the split; fold it into `changed` so both the
			 * pagecache invalidation and the mtime/ctime update run. */
			if (del_modified)
				changed = true;
		}
	} else {
	ret = briefs_collect_all_extents(inode->i_sb, &binfo->disk_inode,
					 &old_exts, &old_count);
	if (ret)
		goto out_unlock;
	if (old_count == 0)
		goto out_unlock;

	new_exts = kvmalloc_array(old_count * 2, sizeof(*new_exts), GFP_KERNEL);
	if (!new_exts) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	/*
	 * Build the new extent list and zero any partially-holed boundary
	 * blocks that remain allocated. Extents arrive already sorted by offset
	 * (the tree maintains order), so the resulting new_exts is sorted too.
	 */
	for (i = 0; i < old_count; i++) {
		struct briefs_extent ext = old_exts[i];
		u64 ext_end = ext.offset + ext.len;
		u64 free_start, free_end;

		if (ext_end <= start_blk || ext.offset >= end_blk) {
			new_exts[new_count++] = ext;
			continue;
		}

		free_start = max(ext.offset, start_blk);
		free_end = min(ext_end, end_blk);

		if (need_partial_start && free_start == start_blk &&
		    free_start < free_end)
			free_start++;

		if (need_partial_end && free_end == end_blk &&
		    free_start < free_end)
			free_end--;

		if (same_boundary_block) {
			/*
			 * Punch is wholly inside one block: zero only the
			 * sub-range [partial_start_off, partial_end_off), not
			 * the whole block.  See same_boundary_block comment above.
			 *
			 * CRITICAL: Release extent_lock around synchronous I/O.
			 */
			if (start_blk >= ext.offset && start_blk < ext_end) {
				u64 abs_block = ext.phys + (start_blk - ext.offset);

				mutex_unlock(&binfo->extent_lock);
				ret = briefs_zero_block_range(inode->i_sb, abs_block,
							      partial_start_off,
							      partial_end_off);
				mutex_lock(&binfo->extent_lock);
				if (ret)
					goto out_unlock;
				changed = true;
			}
		} else {
		if (need_partial_start && start_blk >= ext.offset &&
		    start_blk < ext_end) {
			u64 abs_block = ext.phys + (start_blk - ext.offset);

			mutex_unlock(&binfo->extent_lock);
			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      partial_start_off,
						      BRIEFS_BLOCK_SIZE);
			mutex_lock(&binfo->extent_lock);
			if (ret)
				goto out_unlock;
			changed = true;
		}

		if (need_partial_end && (end_blk - 1) >= ext.offset &&
		    (end_blk - 1) < ext_end) {
			u64 abs_block = ext.phys + ((end_blk - 1) - ext.offset);

			mutex_unlock(&binfo->extent_lock);
			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      0,
						      partial_end_off);
			mutex_lock(&binfo->extent_lock);
			if (ret)
				goto out_unlock;
			changed = true;
		}
		}

		if (free_start > ext.offset) {
			struct briefs_extent left = ext;

			left.len = free_start - ext.offset;
			new_exts[new_count++] = left;
		}

		if (free_end < ext_end) {
			struct briefs_extent right = ext;

			right.offset = free_end;
			right.phys = ext.phys + (free_end - ext.offset);
			right.len = ext_end - free_end;
			new_exts[new_count++] = right;
		}

		if (free_start > ext.offset || free_end < ext_end)
			changed = true;
	}

	/*
	 * Rebuild the index from new_exts. Frees the old tree nodes, resets the
	 * inode, and re-inserts the kept extents (left/right remainders + the
	 * untouched extents). Kept extents' data blocks stay allocated and are
	 * re-referenced by the new index.
	 */
	ret = briefs_rebuild_extent_list(inode->i_sb, binfo, new_exts, new_count);
	if (ret)
		goto out_unlock;

	/*
	 * The inode now references only kept blocks.  It is safe to free the
	 * removed data blocks.  briefs_rebuild_extent_list already freed the
	 * old tree node blocks (the old chain blocks, in chain terms).
	 */
	u64 unwritten_freed = 0;
	for (i = 0; i < old_count; i++) {
		struct briefs_extent ext = old_exts[i];
		u64 ext_end = ext.offset + ext.len;
		u64 free_start, free_end;

		if (ext_end <= start_blk || ext.offset >= end_blk)
			continue;

		free_start = max(ext.offset, start_blk);
		free_end = min(ext_end, end_blk);

		if (need_partial_start && free_start == start_blk &&
		    free_start < free_end)
			free_start++;

		if (need_partial_end && free_end == end_blk &&
		    free_start < free_end)
			free_end--;

		if (free_start < free_end) {
			u64 free_phys = ext.phys + (free_start - ext.offset);
			u64 free_len = free_end - free_start;

			if (ext.flags & BRIEFS_EXT_UNWRITTEN)
				unwritten_freed += free_len;
			briefs_journal_extent_free(bsi->journal, inode->i_ino,
						   free_start, free_phys,
						   free_len);
			briefs_free_blocks_range(bsi, free_phys, free_len);
			changed = true;
		}
	}
	/* Release the metadata reservation held for the unwritten blocks just
	 * freed.  No-op when none of the punched extents were unwritten. */
	briefs_release_unwritten_reserve(inode, unwritten_freed);
	} /* end inline-only punch */

	inode->i_blocks = briefs_compute_i_blocks(inode->i_sb,
						 &binfo->disk_inode);

	/*
	 * Update mtime/ctime BEFORE persisting+journaling the inode snapshot,
	 * so the journaled INODE_FULL (which is what replay restores) carries
	 * the new times.  Previously the time update ran in out_update AFTER the
	 * journal emit, so replay restored the pre-punch mtime/ctime -- and only
	 * ctime was bumped, never mtime (generic/059 "mtime did not increase").
	 * Hole punching changes both mtime and ctime.
	 */
	if (changed) {
		now = current_time(inode);
		inode->i_mtime_sec = now.tv_sec;
		inode->i_mtime_nsec = now.tv_nsec;
		inode->i_ctime_sec = now.tv_sec;
		inode->i_ctime_nsec = now.tv_nsec;
		briefs_sync_inode_times(inode, &binfo->disk_inode);
	}

	briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
			&binfo->disk_inode);

out_unlock:
	mutex_unlock(&binfo->extent_lock);

out_update:
	if (changed) {
		mark_inode_dirty(inode);
		truncate_pagecache_range(inode, offset, end - 1);
	}

out_free:
	kvfree(old_exts);
	kvfree(new_exts);
	inode_unlock(inode);
	if (ret == 0)
		ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * File-range exchange engine.
 *
 * BrieFS has no reflink: every data block has exactly one owning inode and an
 * extent record IS that ownership.  Exchanging two file ranges is therefore a
 * pure remapping -- no data is copied and no data block is freed or allocated;
 * only the extent metadata (and, for TO_EOF, the on-disk sizes) move between
 * the two inodes.  The B+ tree merge path is gated on physical contiguity, so
 * segments that merely meet at a range boundary after rebasing cannot wrongly
 * coalesce with a kept neighbour unless the physical blocks actually abut.
 *
 * The engine mirrors xfs_exchange_range_checks / xfs_exchrange_contents but
 * uses BrieFS's collect/clip/rebuild primitives instead of a logged
 * transaction.  Atomicity is the per-record journal model every other BrieFS
 * op uses: two JRN_INODE_FULL records, one journal sync.  A crash between the
 * two records can leave a duplicate extent reference that fsck.briefs
 * reconciles; no data is lost and no block is orphaned.
 *
 * Locking (new in BrieFS): both inode_lock (i_rwsem) and extent_lock are taken
 * in inode-pointer order so two concurrent exchanges cannot deadlock.  The
 * same-inode case takes each lock once.
 */

struct briefs_exch {
	struct file		*file1;		/* donor */
	struct file		*file2;		/* open file (target) */
	loff_t			file1_offset;	/* o1, bytes */
	loff_t			file2_offset;	/* o2, bytes */
	u64			length;		/* bytes; recomputed for TO_EOF */
	u64			flags;
	const struct briefs_commit_range_fresh *fresh;	/* NULL unless commit */
};

/*
 * briefs_emit_clipped - append the part of each extent in @src that overlaps
 * [lo, hi), rebased by @rebase (a signed block delta), to @out.  Extent
 * offsets/lengths are in blocks; @lo/@hi/@rebase are in block units.
 */
static void briefs_emit_clipped(struct briefs_extent *out, int *n,
				const struct briefs_extent *src, int nsrc,
				u64 lo, u64 hi, s64 rebase)
{
	int i;

	for (i = 0; i < nsrc; i++) {
		const struct briefs_extent *e = &src[i];
		u64 s = max(e->offset, lo);
		u64 en = min(e->offset + e->len, hi);

		if (s >= en)
			continue;
		out[*n].offset = (u64)((s64)s + rebase);
		out[*n].phys = e->phys + (s - e->offset);
		out[*n].len = en - s;
		out[*n].flags = e->flags;
		out[*n].pad = 0;
		(*n)++;
	}
}

/*
 * briefs_emit_subtract - append the part of each extent in @src that overlaps
 * [lo, hi) but does NOT overlap any range in @mask (sorted, non-overlapping,
 * in the same block coordinate space as @src), rebased by @rebase, to @out.
 * Used to keep the ranges of a file that are NOT being exchanged (the complement
 * of the exchange mask).
 */
static void briefs_emit_subtract(struct briefs_extent *out, int *n,
				 const struct briefs_extent *src, int nsrc,
				 u64 lo, u64 hi,
				 const struct briefs_extent *mask, int nmask,
				 s64 rebase)
{
	int i, j;

	for (i = 0; i < nsrc; i++) {
		const struct briefs_extent *e = &src[i];
		u64 s = max(e->offset, lo);
		u64 en = min(e->offset + e->len, hi);
		u64 cur;

		if (s >= en)
			continue;
		cur = s;
		for (j = 0; j < nmask && cur < en; j++) {
			u64 ms = mask[j].offset;
			u64 me = mask[j].offset + mask[j].len;
			u64 a, b;

			if (me <= cur)
				continue;
			if (ms >= en)
				break;
			a = max(ms, cur);
			b = min(me, en);
			if (a > cur) {
				out[*n].offset = (u64)((s64)cur + rebase);
				out[*n].phys = e->phys + (cur - e->offset);
				out[*n].len = a - cur;
				out[*n].flags = e->flags;
				out[*n].pad = 0;
				(*n)++;
			}
			cur = b;
		}
		if (cur < en) {
			out[*n].offset = (u64)((s64)cur + rebase);
			out[*n].phys = e->phys + (cur - e->offset);
			out[*n].len = en - cur;
			out[*n].flags = e->flags;
			out[*n].pad = 0;
			(*n)++;
		}
	}
}

static void briefs_lock_two_inodes(struct inode *inode1, struct inode *inode2)
{
	if (inode1 == inode2) {
		inode_lock(inode1);
		return;
	}
	if (inode1 < inode2) {
		inode_lock(inode1);
		inode_lock(inode2);
	} else {
		inode_lock(inode2);
		inode_lock(inode1);
	}
}

static void briefs_unlock_two_inodes(struct inode *inode1, struct inode *inode2)
{
	if (inode1 == inode2) {
		inode_unlock(inode1);
		return;
	}
	inode_unlock(inode1);
	inode_unlock(inode2);
}

static void briefs_lock_two_extents(struct briefs_inode_info *b1,
				     struct briefs_inode_info *b2)
{
	if (b1 == b2) {
		mutex_lock(&b1->extent_lock);
		return;
	}
	if (b1 < b2) {
		mutex_lock(&b1->extent_lock);
		mutex_lock(&b2->extent_lock);
	} else {
		mutex_lock(&b2->extent_lock);
		mutex_lock(&b1->extent_lock);
	}
}

static void briefs_unlock_two_extents(struct briefs_inode_info *b1,
				      struct briefs_inode_info *b2)
{
	if (b1 == b2) {
		mutex_unlock(&b1->extent_lock);
		return;
	}
	mutex_unlock(&b1->extent_lock);
	mutex_unlock(&b2->extent_lock);
}

/*
 * Exchange two non-overlapping sub-ranges of a single inode.  The two ranges
 * [lo, lo+len) and [hi, hi+len) (lo < hi) are rotated: the hi range's contents
 * move to the lo position, the lo range's contents move to the hi position, and
 * everything outside both ranges is unchanged.  The five clipped emits below
 * produce the new list already sorted by offset.  Caller holds extent_lock.
 */
static int briefs_exch_engine_same(struct super_block *sb,
				   struct briefs_inode_info *b,
				   u64 o1, u64 o2, u64 len)
{
	struct briefs_extent *old = NULL, *new = NULL;
	int n = 0, nn = 0, ret;
	u64 lo = min(o1, o2);
	u64 hi = max(o1, o2);
	s64 r_lo_hi = (s64)lo - (s64)hi;	/* hi range -> lo position */
	s64 r_hi_lo = (s64)hi - (s64)lo;	/* lo range -> hi position */

	ret = briefs_collect_all_extents(sb, &b->disk_inode, &old, &n);
	if (ret)
		return ret;

	new = kvmalloc_array(6 * n + 8, sizeof(*new), GFP_KERNEL);
	if (!new) {
		kvfree(old);
		return -ENOMEM;
	}

	briefs_emit_clipped(new, &nn, old, n, 0, lo, 0);
	briefs_emit_clipped(new, &nn, old, n, hi, hi + len, r_lo_hi);
	briefs_emit_clipped(new, &nn, old, n, lo + len, hi, 0);
	briefs_emit_clipped(new, &nn, old, n, lo, lo + len, r_hi_lo);
	briefs_emit_clipped(new, &nn, old, n, hi + len, U64_MAX, 0);

	ret = briefs_rebuild_extent_list(sb, b, new, nn);
	kvfree(old);
	kvfree(new);
	return ret;
}

/*
 * Exchange [o1, o1+len) of file1 with [o2, o2+len) of file2 (two distinct
 * inodes), or to EOF when @to_eof.  With FILE1_WRITTEN, only file1's WRITTEN
 * extents within the range are exchanged; everywhere else each file keeps its
 * own extents (holes/unwritten do not move).  Caller holds both extent_locks.
 */
static int briefs_exch_engine_diff(struct super_block *sb,
				   struct briefs_inode_info *b1,
				   struct briefs_inode_info *b2,
				   u64 o1, u64 o2, u64 len, bool to_eof,
				   u64 flags)
{
	struct briefs_extent *old1 = NULL, *old2 = NULL;
	struct briefs_extent *mask = NULL, *mask2 = NULL;
	struct briefs_extent *new1 = NULL, *new2 = NULL;
	int n1 = 0, n2 = 0, nmask = 0, nn1 = 0, nn2 = 0;
	u64 cend1, cend2;
	s64 d12, d21;
	int ret = 0, i;

	cend1 = to_eof ? U64_MAX : o1 + len;
	cend2 = to_eof ? U64_MAX : o2 + len;
	d12 = (s64)o1 - (s64)o2;		/* rebase file2 -> file1 coords */
	d21 = (s64)o2 - (s64)o1;		/* rebase file1 -> file2 coords */

	ret = briefs_collect_all_extents(sb, &b1->disk_inode, &old1, &n1);
	if (ret)
		goto out;
	ret = briefs_collect_all_extents(sb, &b2->disk_inode, &old2, &n2);
	if (ret)
		goto out;

	/*
	 * Exchange mask M, in file1 block coords: the sub-ranges of [o1, cend1)
	 * that actually participate in the exchange.  Without FILE1_WRITTEN the
	 * whole range exchanges; with it, only file1's written extents (unwritten
	 * extents and holes exchange nothing, so file2 keeps its own contents
	 * there).
	 */
	mask = kvmalloc_array(n1 + 2, sizeof(*mask), GFP_KERNEL);
	mask2 = kvmalloc_array(n1 + 2, sizeof(*mask2), GFP_KERNEL);
	new1 = kvmalloc_array(2 * (n1 + n2) + 8, sizeof(*new1), GFP_KERNEL);
	new2 = kvmalloc_array(2 * (n1 + n2) + 8, sizeof(*new2), GFP_KERNEL);
	if (!mask || !mask2 || !new1 || !new2) {
		ret = -ENOMEM;
		goto out;
	}

	if (flags & BRIEFS_EXCHANGE_RANGE_FILE1_WRITTEN) {
		for (i = 0; i < n1; i++) {
			u64 s, en;

			if (old1[i].flags & BRIEFS_EXT_UNWRITTEN)
				continue;
			s = max(old1[i].offset, o1);
			en = min(old1[i].offset + old1[i].len, cend1);
			if (s >= en)
				continue;
			mask[nmask].offset = s;
			mask[nmask].len = en - s;
			mask[nmask].phys = 0;
			mask[nmask].flags = 0;
			mask[nmask].pad = 0;
			mask2[nmask].offset = (u64)((s64)s + d21);
			mask2[nmask].len = en - s;
			mask2[nmask].phys = 0;
			mask2[nmask].flags = 0;
			mask2[nmask].pad = 0;
			nmask++;
		}
	} else {
		mask[0].offset = o1;
		mask[0].len = cend1 - o1;
		mask[0].phys = 0;
		mask[0].flags = 0;
		mask[0].pad = 0;
		mask2[0].offset = o2;
		mask2[0].len = cend2 - o2;
		mask2[0].phys = 0;
		mask2[0].flags = 0;
		mask2[0].pad = 0;
		nmask = 1;
	}

	/*
	 * new1 = old1[0,o1) ++ old1[o1,cend1)\M (kept) ++
	 *        (old2's M-corresponding pieces, rebased d12) ++ old1[cend1,EOF).
	 * new2 = old2[0,o2) ++ old2[o2,cend2)\M2 (kept) ++
	 *        (old1's M pieces, rebased d21) ++ old2[cend2,EOF).
	 */
	briefs_emit_clipped(new1, &nn1, old1, n1, 0, o1, 0);
	briefs_emit_subtract(new1, &nn1, old1, n1, o1, cend1, mask, nmask, 0);
	for (i = 0; i < nmask; i++) {
		u64 ms = (u64)((s64)mask[i].offset + d21);
		u64 me = (u64)((s64)(mask[i].offset + mask[i].len) + d21);

		briefs_emit_clipped(new1, &nn1, old2, n2, ms, me, d12);
	}
	briefs_emit_clipped(new1, &nn1, old1, n1, cend1, U64_MAX, 0);

	briefs_emit_clipped(new2, &nn2, old2, n2, 0, o2, 0);
	briefs_emit_subtract(new2, &nn2, old2, n2, o2, cend2, mask2, nmask, 0);
	for (i = 0; i < nmask; i++)
		briefs_emit_clipped(new2, &nn2, old1, n1,
				    mask[i].offset, mask[i].offset + mask[i].len,
				    d21);
	briefs_emit_clipped(new2, &nn2, old2, n2, cend2, U64_MAX, 0);

	ret = briefs_rebuild_extent_list(sb, b1, new1, nn1);
	if (ret)
		goto out;
	ret = briefs_rebuild_extent_list(sb, b2, new2, nn2);
out:
	kvfree(old1);
	kvfree(old2);
	kvfree(mask);
	kvfree(mask2);
	kvfree(new1);
	kvfree(new2);
	return ret;
}

/*
 * Range validation, mirroring xfs_exchange_range_checks (xfs_exchrange.c
 * 337-446) so the generic/717 error matrix matches exactly.  Recomputes
 * @fx->length for TO_EOF.  Returns 0 or -errno.
 */
static int briefs_exch_checks(struct briefs_exch *fx)
{
	struct inode *inode1 = file_inode(fx->file1);
	struct inode *inode2 = file_inode(fx->file2);
	loff_t size1 = i_size_read(inode1);
	loff_t size2 = i_size_read(inode2);
	unsigned int alloc_unit = inode1->i_sb->s_blocksize;
	uint64_t allocmask = alloc_unit - 1;
	int64_t test_len;
	uint64_t blen;
	loff_t tmp;
	int error;

	if (IS_IMMUTABLE(inode1) || IS_IMMUTABLE(inode2))
		return -EPERM;
	if (IS_SWAPFILE(inode1) || IS_SWAPFILE(inode2))
		return -ETXTBSY;

	if (fx->file1_offset > size1 || fx->file2_offset > size2)
		return -EINVAL;

	if (fx->flags & BRIEFS_EXCHANGE_RANGE_TO_EOF) {
		fx->length = max_t(int64_t, size1 - fx->file1_offset,
				   size2 - fx->file2_offset);
	} else {
		if (fx->file1_offset + fx->length > size1 ||
		    fx->file2_offset + fx->length > size2)
			return -EINVAL;
	}

	if (!IS_ALIGNED(fx->file1_offset, alloc_unit) ||
	    !IS_ALIGNED(fx->file2_offset, alloc_unit))
		return -EINVAL;

	if (check_add_overflow(fx->file1_offset, fx->length, &tmp) ||
	    check_add_overflow(fx->file2_offset, fx->length, &tmp))
		return -EINVAL;

	test_len = fx->length;
	error = generic_write_check_limits(fx->file2, fx->file2_offset, &test_len);
	if (error)
		return error;
	error = generic_write_check_limits(fx->file1, fx->file1_offset, &test_len);
	if (error)
		return error;
	if (test_len != fx->length)
		return -EINVAL;

	blen = fx->length;
	if (fx->file1_offset + fx->length == size1)
		blen = ALIGN(size1, alloc_unit) - fx->file1_offset;
	else if (fx->file2_offset + fx->length == size2)
		blen = ALIGN(size2, alloc_unit) - fx->file2_offset;
	else if (!IS_ALIGNED(fx->length, alloc_unit))
		return -EINVAL;

	if (inode1 == inode2 &&
	    fx->file2_offset + blen > fx->file1_offset &&
	    fx->file1_offset + blen > fx->file2_offset)
		return -EINVAL;

	if ((fx->length & allocmask) == 0)
		return 0;

	blen = fx->length;
	if (fx->file2_offset + blen < size2)
		blen &= ~allocmask;
	if (fx->file1_offset + blen < size1)
		blen &= ~allocmask;

	return blen == fx->length ? 0 : -EINVAL;
}

/* Verify file2 has not changed since START_COMMIT sampled its freshness. */
static int briefs_check_freshness(struct inode *inode2,
				  const struct briefs_commit_range_fresh *f)
{
	struct timespec64 ctime = inode_get_ctime(inode2);
	struct timespec64 mtime = inode_get_mtime(inode2);

	if (f->file2_ino != inode2->i_ino ||
	    f->file2_gen != inode2->i_generation ||
	    (s64)f->file2_ctime != ctime.tv_sec ||
	    (s32)f->file2_ctime_nsec != ctime.tv_nsec ||
	    (s64)f->file2_mtime != mtime.tv_sec ||
	    (s32)f->file2_mtime_nsec != mtime.tv_nsec)
		return -EBUSY;
	return 0;
}

/*
 * briefs_exch_contents - the locked body of a range exchange.  Mirrors
 * xfs_exchange_range_prep + xfs_exchrange_contents + xfs_exchange_range_finish.
 * Caller has already done the no-lock checks and file_start_write(file2).
 */
static int briefs_exch_contents(struct briefs_exch *fx)
{
	struct inode *inode1 = file_inode(fx->file1);
	struct inode *inode2 = file_inode(fx->file2);
	struct briefs_inode_info *b1 = briefs_i(inode1);
	struct briefs_inode_info *b2 = briefs_i(inode2);
	struct super_block *sb = inode1->i_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	bool same_inode = (inode1 == inode2);
	int ret;

	briefs_lock_two_inodes(inode1, inode2);

	if (fx->fresh) {
		ret = briefs_check_freshness(inode2, fx->fresh);
		if (ret)
			goto out_unlock_inodes;
	}

	ret = briefs_exch_checks(fx);
	if (ret || fx->length == 0)
		goto out_unlock_inodes;

	inode_dio_wait(inode1);
	if (!same_inode)
		inode_dio_wait(inode2);

	ret = filemap_write_and_wait_range(inode1->i_mapping,
			fx->file1_offset, fx->file1_offset + fx->length - 1);
	if (ret)
		goto out_unlock_inodes;
	ret = filemap_write_and_wait_range(inode2->i_mapping,
			fx->file2_offset, fx->file2_offset + fx->length - 1);
	if (ret)
		goto out_unlock_inodes;

	if (((fx->file1->f_flags | fx->file2->f_flags) & O_SYNC) ||
	    IS_SYNC(inode1) || IS_SYNC(inode2))
		fx->flags |= BRIEFS_EXCHANGE_RANGE_DSYNC;

	if (fx->flags & BRIEFS_EXCHANGE_RANGE_DRY_RUN)
		goto out_unlock_inodes;

	/* Strip setuid/caps before the exchange so the persist captures it. */
	ret = file_remove_privs(fx->file1);
	if (ret)
		goto out_unlock_inodes;
	if (!same_inode) {
		ret = file_remove_privs(fx->file2);
		if (ret)
			goto out_unlock_inodes;
	}

	if (b1->disk_inode.flags & InodeFlagInlineData) {
		ret = briefs_promote_inline_data(inode1);
		if (ret)
			goto out_unlock_inodes;
	}
	if (!same_inode && (b2->disk_inode.flags & InodeFlagInlineData)) {
		ret = briefs_promote_inline_data(inode2);
		if (ret)
			goto out_unlock_inodes;
	}

	/*
	 * Drop any cached pages in the exchanged ranges so that post-exchange
	 * reads re-fault through iomap against the new mappings.  Pages are
	 * clean (writeback waited above) so this just removes them; doing it
	 * before the swap closes the stale-cache window.
	 *
	 * For TO_EOF the invalidated tail must reach the end of the file: a
	 * byte-exact end (file_offset + length - 1) lands inside the last
	 * (unaligned) page, and truncate_inode_pages_range() would *partially*
	 * zero that folio instead of removing it, leaving stale cached bytes
	 * for the swapped tail block (generic/713 unalignedeof).  Pass
	 * LLONG_MAX so the final folio is removed wholesale and the re-fault
	 * sees the new mapping.  For a same-file exchange the two ranges live in
	 * one address_space, so invalidate each range separately.
	 */
	{
		loff_t lend1, lend2;

		lend1 = (fx->flags & BRIEFS_EXCHANGE_RANGE_TO_EOF)
			? LLONG_MAX : fx->file1_offset + fx->length - 1;
		lend2 = (fx->flags & BRIEFS_EXCHANGE_RANGE_TO_EOF)
			? LLONG_MAX : fx->file2_offset + fx->length - 1;
		truncate_inode_pages_range(inode1->i_mapping,
					   fx->file1_offset, lend1);
		if (!same_inode)
			truncate_inode_pages_range(inode2->i_mapping,
						   fx->file2_offset, lend2);
		else
			truncate_inode_pages_range(inode1->i_mapping,
						   fx->file2_offset, lend2);
	}

	briefs_lock_two_extents(b1, b2);

	if (same_inode) {
		ret = briefs_exch_engine_same(sb, b1,
				fx->file1_offset >> BRIEFS_BLOCK_SHIFT,
				fx->file2_offset >> BRIEFS_BLOCK_SHIFT,
				fx->length >> BRIEFS_BLOCK_SHIFT);
	} else {
		ret = briefs_exch_engine_diff(sb, b1, b2,
				fx->file1_offset >> BRIEFS_BLOCK_SHIFT,
				fx->file2_offset >> BRIEFS_BLOCK_SHIFT,
				fx->length >> BRIEFS_BLOCK_SHIFT,
				fx->flags & BRIEFS_EXCHANGE_RANGE_TO_EOF,
				fx->flags);
	}
	if (ret)
		goto out_unlock_extents;

	if (fx->flags & BRIEFS_EXCHANGE_RANGE_TO_EOF) {
		u64 t = i_size_read(inode1);

		i_size_write(inode1, i_size_read(inode2));
		i_size_write(inode2, t);
		briefs_extent_write_begin(b1);
		b1->disk_inode.filesize = i_size_read(inode1);
		briefs_extent_write_end(b1);
		if (!same_inode) {
			briefs_extent_write_begin(b2);
			b2->disk_inode.filesize = i_size_read(inode2);
			briefs_extent_write_end(b2);
		}
	}

	inode1->i_blocks = briefs_compute_i_blocks(sb, &b1->disk_inode);
	if (!same_inode)
		inode2->i_blocks = briefs_compute_i_blocks(sb, &b2->disk_inode);

	{
		struct timespec64 now = current_time(inode1);

		inode_set_mtime_to_ts(inode1, now);
		inode_set_ctime_to_ts(inode1, now);
		if (!same_inode) {
			inode_set_mtime_to_ts(inode2, now);
			inode_set_ctime_to_ts(inode2, now);
		}
	}
	briefs_sync_inode_times(inode1, &b1->disk_inode);
	if (!same_inode)
		briefs_sync_inode_times(inode2, &b2->disk_inode);

	briefs_persist_and_journal_inode_warn(sb, inode1, &b1->disk_inode);
	if (!same_inode)
		briefs_persist_and_journal_inode_warn(sb, inode2, &b2->disk_inode);

	mark_inode_dirty(inode1);
	if (!same_inode)
		mark_inode_dirty(inode2);

out_unlock_extents:
	briefs_unlock_two_extents(b1, b2);
out_unlock_inodes:
	briefs_unlock_two_inodes(inode1, inode2);

	if (ret == 0 && (fx->flags & BRIEFS_EXCHANGE_RANGE_DSYNC)) {
		int r2 = briefs_flush_pending_journal_snapshots(bsi->journal, sb);

		if (!r2)
			r2 = briefs_journal_sync(bsi->journal);
		if (!r2)
			r2 = blkdev_issue_flush(sb->s_bdev);
		if (r2 && ret == 0)
			ret = r2;
	}

	return ret;
}

/*
 * briefs_do_exchange - resolve and validate the two files, run the exchange.
 * Mirrors xfs_exchange_range (the no-lock checks, file_start_write, fsnotify).
 * @fresh is non-NULL for COMMIT_RANGE (verify file2 unchanged before exchange).
 */
static long briefs_do_exchange(struct file *file1, struct file *file2,
			       u64 o1, u64 o2, u64 length, u64 flags,
			       const struct briefs_commit_range_fresh *fresh)
{
	struct inode *inode1 = file_inode(file1);
	struct inode *inode2 = file_inode(file2);
	struct briefs_exch fx = {
		.file1 = file1,
		.file2 = file2,
		.file1_offset = o1,
		.file2_offset = o2,
		.length = length,
		.flags = flags,
		.fresh = fresh,
	};
	loff_t check_len;
	int ret;

	if (file1->f_path.mnt != file2->f_path.mnt)
		return -EXDEV;
	if (flags & ~BRIEFS_EXCHANGE_RANGE_ALL_FLAGS)
		return -EINVAL;
	if (S_ISDIR(inode1->i_mode) || S_ISDIR(inode2->i_mode))
		return -EISDIR;
	if (!S_ISREG(inode1->i_mode) || !S_ISREG(inode2->i_mode))
		return -EINVAL;
	if (!(file1->f_mode & FMODE_READ) || !(file1->f_mode & FMODE_WRITE) ||
	    !(file2->f_mode & FMODE_READ) || !(file2->f_mode & FMODE_WRITE))
		return -EBADF;
	if ((file1->f_flags & O_APPEND) || (file2->f_flags & O_APPEND))
		return -EBADF;

	check_len = (flags & BRIEFS_EXCHANGE_RANGE_TO_EOF) ? 0 : length;
	ret = remap_verify_area(file1, o1, check_len, true);
	if (ret)
		return ret;
	ret = remap_verify_area(file2, o2, check_len, true);
	if (ret)
		return ret;

	file_start_write(file2);
	ret = briefs_exch_contents(&fx);
	file_end_write(file2);
	if (ret)
		return ret;

	fsnotify_modify(file1);
	if (file2 != file1)
		fsnotify_modify(file2);
	return 0;
}

/*
 * XFS_IOC_EXCHANGE_RANGE: exchange [o1,o1+len) of file1 with [o2,o2+len) of
 * the open file (file2).  file1 is the donor fd passed in the arg.
 */
static long briefs_ioc_exchange_range(struct file *file,
				      struct briefs_exchange_range __user *argp)
{
	struct briefs_exchange_range args;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;
	if (memchr_inv(&args.pad, 0, sizeof(args.pad)))
		return -EINVAL;
	if (args.flags & ~BRIEFS_EXCHANGE_RANGE_ALL_FLAGS)
		return -EINVAL;

	CLASS(fd, file1)(args.file1_fd);
	if (fd_empty(file1))
		return -EBADF;

	return briefs_do_exchange(fd_file(file1), file, args.file1_offset,
				  args.file2_offset, args.length, args.flags,
				  NULL);
}

/* XFS_IOC_START_COMMIT: sample file2's freshness into the user blob. */
static long briefs_ioc_start_commit(struct file *file,
				    struct briefs_commit_range __user *argp)
{
	struct briefs_commit_range_fresh kf = { };
	struct inode *inode2 = file_inode(file);
	struct timespec64 ctime, mtime;

	BUILD_BUG_ON(sizeof(struct briefs_commit_range_fresh) !=
		     sizeof_field(struct briefs_commit_range, file2_freshness));

	inode_lock(inode2);
	ctime = inode_get_ctime(inode2);
	mtime = inode_get_mtime(inode2);
	kf.file2_ino = inode2->i_ino;
	kf.file2_gen = inode2->i_generation;
	kf.file2_ctime = ctime.tv_sec;
	kf.file2_ctime_nsec = ctime.tv_nsec;
	kf.file2_mtime = mtime.tv_sec;
	kf.file2_mtime_nsec = mtime.tv_nsec;
	kf.magic = BRIEFS_XCR_FRESH_MAGIC;
	inode_unlock(inode2);

	if (copy_to_user((struct briefs_commit_range_fresh __user *)
			 &argp->file2_freshness, &kf, sizeof(kf)))
		return -EFAULT;
	return 0;
}

/* XFS_IOC_COMMIT_RANGE: verify file2 unchanged, then exchange. */
static long briefs_ioc_commit_range(struct file *file,
				   struct briefs_commit_range __user *argp)
{
	struct briefs_commit_range args;
	struct briefs_commit_range_fresh *kf;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;
	if (args.flags & ~BRIEFS_EXCHANGE_RANGE_ALL_FLAGS)
		return -EINVAL;
	kf = (struct briefs_commit_range_fresh *)&args.file2_freshness;
	if (kf->magic != BRIEFS_XCR_FRESH_MAGIC)
		return -EBUSY;

	CLASS(fd, file1)(args.file1_fd);
	if (fd_empty(file1))
		return -EBADF;

	return briefs_do_exchange(fd_file(file1), file, args.file1_offset,
				  args.file2_offset, args.length, args.flags,
				  kf);
}

/*
 * XFS_IOC_SWAPEXT: whole-file extent swap (the historical precursor to
 * EXCHANGE_RANGE).  The open file is the target (file2); sx_fdtmp is the donor
 * (file1).  It is a TO_EOF exchange at offset 0 of both files.  The swapfile
 * check (generic/711) returns ETXTBSY.
 */
static long briefs_ioc_swapext(struct file *file,
			      struct briefs_swapext __user *argp)
{
	struct briefs_swapext args;

	if (copy_from_user(&args, argp, sizeof(args)))
		return -EFAULT;

	CLASS(fd, target)(args.sx_fdtarget);
	if (fd_empty(target))
		return -EINVAL;
	if (!(fd_file(target)->f_mode & FMODE_WRITE) ||
	    !(fd_file(target)->f_mode & FMODE_READ) ||
	    (fd_file(target)->f_flags & O_APPEND))
		return -EBADF;

	CLASS(fd, tmp)(args.sx_fdtmp);
	if (fd_empty(tmp))
		return -EINVAL;
	if (!(fd_file(tmp)->f_mode & FMODE_WRITE) ||
	    !(fd_file(tmp)->f_mode & FMODE_READ) ||
	    (fd_file(tmp)->f_flags & O_APPEND))
		return -EBADF;

	if (IS_SWAPFILE(file_inode(fd_file(target))) ||
	    IS_SWAPFILE(file_inode(fd_file(tmp))))
		return -ETXTBSY;
	if (file_inode(fd_file(target)) == file_inode(fd_file(tmp)))
		return -EINVAL;

	/* whole-file, to EOF, offset 0 */
	return briefs_do_exchange(fd_file(tmp), fd_file(target), 0, 0, 0,
				  BRIEFS_EXCHANGE_RANGE_TO_EOF, NULL);
}

/*
 * briefs_zero_alloc_hole - allocate fresh unwritten blocks for the hole
 * [h_start, h_end) and append the resulting extent(s) to @new (advancing
 * *@n_new), recording each allocation in @allocd (advancing *@n_allocd) so
 * the caller can free them on a later failure.  *@added accumulates the
 * newly-unwritten block count for the meta_shield reserve.  Caller holds
 * extent_lock; briefs_alloc_blocks/briefs_alloc_block take alloc->lock under
 * it (the established extent_lock -> alloc->lock order).  Returns 0 or
 * -ENOSPC (the recorded allocations are the caller's to free on error).
 */
static int briefs_zero_alloc_hole(struct briefs_sb_info *bsi,
				  struct briefs_extent *new, int *n_new,
				  struct briefs_extent *allocd, int *n_allocd,
				  u64 h_start, u64 h_end, u64 *added)
{
	u64 seg = h_end - h_start;
	u64 rel, phys;
	int i;

	/* Prefer one contiguous run so rebuild can keep a single extent. */
	rel = briefs_alloc_blocks(&bsi->alloc, seg);
	if (rel != 0) {
		phys = data_to_abs(bsi->sb, rel);
		new[*n_new].offset = h_start;
		new[*n_new].phys = phys;
		new[*n_new].len = seg;
		new[*n_new].flags = BRIEFS_EXT_UNWRITTEN;
		new[*n_new].pad = 0;
		(*n_new)++;
		allocd[*n_allocd].offset = h_start;
		allocd[*n_allocd].phys = phys;
		allocd[*n_allocd].len = seg;
		(*n_allocd)++;
		*added += seg;
		return 0;
	}

	/* No contiguous run of seg fit: fall back to per-block allocation. */
	for (i = 0; i < seg; i++) {
		rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0)
			return -ENOSPC;
		phys = data_to_abs(bsi->sb, rel);
		new[*n_new].offset = h_start + i;
		new[*n_new].phys = phys;
		new[*n_new].len = 1;
		new[*n_new].flags = BRIEFS_EXT_UNWRITTEN;
		new[*n_new].pad = 0;
		(*n_new)++;
		allocd[*n_allocd].offset = h_start + i;
		allocd[*n_allocd].phys = phys;
		allocd[*n_allocd].len = 1;
		(*n_allocd)++;
		(*added)++;
	}
	return 0;
}

/*
 * briefs_do_zero_range - FALLOC_FL_ZERO_RANGE.  Zero the contents of
 * [offset, offset+len), matching ext4/xfs semantics (generic/009):
 *
 *   - byte-granular pagecache zeroing over the whole range via
 *     iomap_zero_range with the read ops.  Written blocks are zeroed in
 *     place and stay "data"; holes/unwritten already read as zero.  This
 *     alone covers partial-block ranges, which must NOT convert to unwritten
 *     (generic/009 case 17: a partial-block zero keeps the block as "data").
 *
 *   - the block-aligned middle [ceil(offset), floor(end)) is converted to
 *     UNWRITTEN extents: written blocks have their flag flipped in place
 *     (the on-disk data is masked by IOMAP_UNWRITTEN, as on ext4 -- no free
 *     or realloc), and holes are allocated as fresh unwritten blocks.  fiemap
 *     then reports the zeroed region as "unwritten", not "hole".
 *
 * The conversion reuses the collect/rebuild frame (like punch and collapse):
 * collect the sorted extent set, build a new sorted set with in-range extents
 * flipped to unwritten and hole segments allocated as unwritten, and rebuild
 * (whose insert merges adjacent unwritten extents).  The pagecache for the
 * converted middle is then dropped so reads return the unwritten zeros rather
 * than the stale on-disk data they still occupy.  The unwritten reserve
 * (meta_shield) is raised for the newly-unwritten blocks so a later
 * partial-write conversion can allocate its split metadata.
 *
 * With !KEEP_SIZE the file may be extended to end; the extension is allocated
 * unwritten and reads as zero.  The caller (briefs_fallocate) holds inode_lock
 * and file_remove_privs has already run.
 */
static long briefs_do_zero_range(struct file *file, int mode, loff_t offset,
				 loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct timespec64 now;
	loff_t end = offset + len;
	loff_t old_size;
	u64 s_full, e_full;
	bool grew_size = false;
	int ret;

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		if (end <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			loff_t z_start = max_t(loff_t, offset, 0);
			loff_t z_end = min_t(loff_t, end, inode->i_size);

			if (z_start < z_end)
				memset(binfo->disk_inode.inline_data + z_start,
				       0, z_end - z_start);
			if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
				briefs_extent_write_begin(binfo);
				inode->i_size = end;
				binfo->disk_inode.filesize = end;
				briefs_extent_write_end(binfo);
				grew_size = true;
			}
			now = current_time(inode);
			inode->i_ctime_sec = now.tv_sec;
			inode->i_ctime_nsec = now.tv_nsec;
			if (grew_size) {
				inode->i_mtime_sec = now.tv_sec;
				inode->i_mtime_nsec = now.tv_nsec;
			}
			briefs_sync_inode_times(inode, &binfo->disk_inode);
			briefs_persist_and_journal_inode_warn(sb, inode,
					&binfo->disk_inode);
			mark_inode_dirty(inode);
			inode_unlock(inode);
			return briefs_inode_sync(inode);
		}
		ret = briefs_promote_inline_data(inode);
		if (ret)
			goto out;
		truncate_inode_pages(inode->i_mapping, 0);
	}

	/* Flush delalloc so iomap_zero_range sees real mappings, not BH_Delay
	 * blocks absent from the extent list (same reason as punch). */
	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		goto out;

	/* Byte-granular zero of the pagecache over the whole range.  For
	 * written blocks this zeroes and dirties the folio; for holes/unwritten
	 * it is a no-op (they already read as zero).  This covers partial-block
	 * ranges, which stay "data" (generic/009 case 17). */
	ret = iomap_zero_range(inode, offset, len, NULL, &briefs_iomap_ops);
	if (ret)
		goto out;

	/* Convert the block-aligned middle [s_full, e_full) to unwritten
	 * extents.  Partial head/tail blocks (when offset/end are not
	 * block-aligned) are excluded: they were zeroed in the pagecache above
	 * and keep their type. */
	s_full = (offset + BRIEFS_BLOCK_SIZE - 1) >> BRIEFS_BLOCK_SHIFT;
	e_full = end >> BRIEFS_BLOCK_SHIFT;
	if (s_full < e_full) {
		struct briefs_extent *old = NULL, *new = NULL, *allocd = NULL;
		int n_ext = 0, n_new = 0, n_allocd = 0, i;
		u64 cursor, added = 0;

		mutex_lock(&binfo->extent_lock);
		ret = briefs_collect_all_extents(sb, &binfo->disk_inode,
						 &old, &n_ext);
		if (ret) {
			mutex_unlock(&binfo->extent_lock);
			goto out;
		}
		/* Upper bound: kept/flipped extents (<= 2 each from straddle
		 * splits) plus the hole segments we allocate (<= 1 extent per
		 * block in the per-block fallback). */
		new = kvmalloc_array((n_ext * 2) + (e_full - s_full) + 2,
				     sizeof(*new), GFP_KERNEL);
		allocd = kvmalloc_array((e_full - s_full) + 1,
					sizeof(*allocd), GFP_KERNEL);
		if (!new || !allocd) {
			ret = -ENOMEM;
			goto conv_fail;
		}

		cursor = s_full;
		for (i = 0; i < n_ext; i++) {
			struct briefs_extent e = old[i];
			u64 o = e.offset, ee = o + e.len;

			/* Hole before this extent, inside [s_full, e_full):
			 * allocate unwritten.  cursor is always >= s_full, so
			 * [cursor, min(o,e_full)) lies inside the range. */
			if (o > cursor) {
				u64 he = min_t(u64, o, e_full);

				if (cursor < he) {
					ret = briefs_zero_alloc_hole(bsi, new,
								 &n_new,
								 allocd,
								 &n_allocd,
								 cursor, he,
								 &added);
					if (ret)
						goto conv_fail;
				}
			}

			if (ee <= s_full) {
				/* Entirely before the range: keep as-is. */
				new[n_new++] = e;
			} else if (o >= e_full) {
				/* Entirely after the range: keep as-is. */
				new[n_new++] = e;
			} else {
				u64 ms, me;

				/* Prefix before the range: keep as-is. */
				if (o < s_full) {
					struct briefs_extent p = e;

					p.len = s_full - o;
					new[n_new++] = p;
				}
				/* In-range portion: flip to unwritten (same
				 * phys; the data is masked by IOMAP_UNWRITTEN,
				 * as on ext4). */
				ms = max_t(u64, o, s_full);
				me = min_t(u64, ee, e_full);
				new[n_new].offset = ms;
				new[n_new].phys = e.phys + (ms - o);
				new[n_new].len = me - ms;
				new[n_new].flags = BRIEFS_EXT_UNWRITTEN;
				new[n_new].pad = 0;
				n_new++;
				if (!(e.flags & BRIEFS_EXT_UNWRITTEN))
					added += me - ms;	/* data->unwritten */
				/* (unwritten->unwritten stays counted) */

				/* Suffix after the range: keep as-is. */
				if (ee > e_full) {
					struct briefs_extent s = e;

					s.offset = e_full;
					s.phys = e.phys + (e_full - o);
					s.len = ee - e_full;
					new[n_new++] = s;
				}
			}
			cursor = max_t(u64, cursor, min_t(u64, ee, e_full));
		}
		/* Trailing hole after the last extent, inside the range. */
		if (cursor < e_full) {
			ret = briefs_zero_alloc_hole(bsi, new, &n_new, allocd,
						     &n_allocd, cursor, e_full,
						     &added);
			if (ret)
				goto conv_fail;
		}

		ret = briefs_rebuild_extent_list(sb, binfo, new, n_new);
		if (ret)
			goto conv_fail;

		briefs_raise_unwritten_reserve(inode, added);
		kvfree(new);
		kvfree(old);
		kvfree(allocd);
		mutex_unlock(&binfo->extent_lock);

		/* Drop the pagecache for the converted middle: the in-range
		 * blocks are now unwritten (read as zero), so the stale on-disk
		 * data they still occupy must not be served from the cache. */
		truncate_inode_pages_range(inode->i_mapping,
					   s_full << BRIEFS_BLOCK_SHIFT,
					   (e_full << BRIEFS_BLOCK_SHIFT) - 1);
		goto converted;

conv_fail:
		/* Free the blocks allocated this call (no rebuild ran, so the
		 * extent index is unchanged) and release everything. */
		for (i = 0; i < n_allocd; i++)
			briefs_free_blocks_range(bsi, allocd[i].phys,
						 allocd[i].len);
		kvfree(new);
		kvfree(old);
		kvfree(allocd);
		mutex_unlock(&binfo->extent_lock);
		goto out;
	}
converted:
	old_size = inode->i_size;
	if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
		/* Zero the old-EOF block's tail before i_size advances past it,
		 * so a mid-block EOF (possibly mmap-polluted) does not leak as
		 * valid data (generic/363).  Skip when the EOF block was just
		 * converted to unwritten: it already reads as zero, and
		 * dirtying its folio would let writeback flip it back to "data"
		 * (breaking the unwritten layout). */
		if ((old_size & (BRIEFS_BLOCK_SIZE - 1)) &&
		    (old_size >> BRIEFS_BLOCK_SHIFT) < s_full)
			briefs_zero_eof_tail(inode->i_mapping, old_size);
		briefs_extent_write_begin(binfo);
		inode->i_size = end;
		binfo->disk_inode.filesize = end;
		briefs_extent_write_end(binfo);
		grew_size = true;
	}

	inode->i_blocks = briefs_compute_i_blocks(sb, &binfo->disk_inode);
	now = current_time(inode);
	inode->i_ctime_sec = now.tv_sec;
	inode->i_ctime_nsec = now.tv_nsec;
	if (grew_size) {
		inode->i_mtime_sec = now.tv_sec;
		inode->i_mtime_nsec = now.tv_nsec;
	}
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_persist_and_journal_inode_warn(sb, inode, &binfo->disk_inode);
	mark_inode_dirty(inode);

out:
	inode_unlock(inode);
	if (ret == 0)
		ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * briefs_shift_extents - shared collect/shift/rebuild core for collapse_range
 * and insert_range.  Both rewrite the extent index by collecting the sorted
 * extent set, transforming each extent's logical offset at/after the split
 * point @S, and rebuilding via briefs_rebuild_extent_list (which frees the old
 * tree nodes and re-inserts the kept extents).  Caller holds extent_lock.
 *
 * For collapse (@dir == -1, @L == len/blk): the middle [S, S+L) is removed --
 * its data blocks are freed (and journaled) and dropped from the index; every
 * extent at/after S+L shifts down by L; extents straddling S or S+L are split.
 *
 * For insert (@dir == +1, @L == len/blk): a hole [S, S+L) is opened; every
 * extent at/after S shifts up by L; an extent straddling S is split into a
 * prefix (kept) and a shifted suffix.  No blocks are freed or allocated (the
 * gap is a plain hole, which reads as zero).
 *
 * Returns 0 or -errno.  On collapse, *@unwritten_freed accumulates the count
 * of freed unwritten blocks so the caller can release the meta_shield reserve.
 */
static int briefs_shift_extents(struct inode *inode, u64 S, u64 L, int dir,
				u64 *unwritten_freed)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct briefs_extent *old = NULL, *new = NULL;
	int old_n = 0, new_n = 0, i;
	int ret;

	ret = briefs_collect_all_extents(sb, &binfo->disk_inode, &old, &old_n);
	if (ret)
		return ret;
	if (old_n == 0)
		return 0;	/* nothing to shift; caller still adjusts i_size */

	new = kvmalloc_array(old_n * 2, sizeof(*new), GFP_KERNEL);
	if (!new) {
		kvfree(old);
		return -ENOMEM;
	}

	for (i = 0; i < old_n; i++) {
		struct briefs_extent e = old[i];
		u64 o = e.offset, eend = o + e.len;

		if (dir < 0) {
			/* collapse: remove [S, S+L), shift >= S+L down by L */
			if (eend <= S) {
				new[new_n++] = e;
			} else if (o >= S + L) {
				e.offset = o - L;
				new[new_n++] = e;
			} else if (o >= S && eend <= S + L) {
				/* wholly inside the removed range: free, drop */
				if (e.flags & BRIEFS_EXT_UNWRITTEN)
					*unwritten_freed += e.len;
				briefs_journal_extent_free(bsi->journal,
							   inode->i_ino, o,
							   e.phys, e.len);
				briefs_free_blocks_range(bsi, e.phys, e.len);
			} else {
				/* straddles S and/or S+L */
				u64 mid_start = max(o, S);
				u64 mid_end = min(eend, S + L);

				if (o < S) {
					struct briefs_extent left = e;

					left.len = S - o;
					new[new_n++] = left;
				}
				if (mid_start < mid_end) {
					u64 fp = e.phys + (mid_start - o);
					u64 fl = mid_end - mid_start;

					if (e.flags & BRIEFS_EXT_UNWRITTEN)
						*unwritten_freed += fl;
					briefs_journal_extent_free(bsi->journal,
								   inode->i_ino,
								   mid_start, fp, fl);
					briefs_free_blocks_range(bsi, fp, fl);
				}
				if (eend > S + L) {
					struct briefs_extent right = e;

					right.offset = S;	/* (S+L) - L */
					right.phys = e.phys + (S + L - o);
					right.len = eend - (S + L);
					new[new_n++] = right;
				}
			}
		} else {
			/* insert: open hole [S, S+L), shift >= S up by L */
			if (eend <= S) {
				new[new_n++] = e;
			} else if (o >= S) {
				e.offset = o + L;
				new[new_n++] = e;
			} else {
				/* straddles S: prefix kept, suffix shifted up */
				struct briefs_extent left = e;
				struct briefs_extent right = e;

				left.len = S - o;
				new[new_n++] = left;

				right.offset = S + L;
				right.phys = e.phys + (S - o);
				right.len = eend - S;
				new[new_n++] = right;
			}
		}
	}

	ret = briefs_rebuild_extent_list(sb, binfo, new, new_n);
	kvfree(old);
	kvfree(new);
	return ret;
}

/*
 * briefs_do_collapse_range - FALLOC_FL_COLLAPSE_RANGE.  Remove
 * [offset, offset+len) and shift the data after it down by len; i_size -= len.
 * The VFS guarantees offset/len are block-aligned and offset+len <= i_size.
 */
static long briefs_do_collapse_range(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct timespec64 now;
	u64 S = offset >> BRIEFS_BLOCK_SHIFT;
	u64 L = len >> BRIEFS_BLOCK_SHIFT;
	u64 unwritten_freed = 0;
	int ret;

	/*
	 * Collapse range requires block-aligned offset/len and a range that
	 * lies strictly within EOF (offset+len < i_size); reaching or exceeding
	 * EOF is -EINVAL (a tail removal is a truncate, not a collapse).  The
	 * VFS fallocate path does not enforce either condition, so -- as
	 * ext4_collapse_range does -- we must.  Without these checks a request
	 * with len > i_size (or offset+len > i_size) underflows the
	 * `inode->i_size -= len` below to a huge negative value; a later
	 * buffered write then trips pagecache_isize_extended's WARN_ON, because
	 * iomap's `pos + written > old_size` comparison is unsigned (the huge
	 * i_size suppresses i_size_write) while its `old_size < pos` gate is
	 * signed (negative < positive fires the call) -- generic/070.
	 */
	if (!IS_ALIGNED(offset | len, BRIEFS_BLOCK_SIZE)) {
		ret = -EINVAL;
		goto out;
	}
	if (offset + len >= i_size_read(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	ret = filemap_write_and_wait_range(inode->i_mapping, offset,
					   inode->i_size - 1);
	if (ret)
		goto out;

	mutex_lock(&binfo->extent_lock);
	ret = briefs_shift_extents(inode, S, L, -1, &unwritten_freed);
	if (ret) {
		mutex_unlock(&binfo->extent_lock);
		goto out;
	}
	briefs_release_unwritten_reserve(inode, unwritten_freed);
	mutex_unlock(&binfo->extent_lock);

	/* Pages at/after @offset now map to shifted/removed extents: drop them
	 * so reads repopulate from the new mapping. */
	truncate_inode_pages_range(inode->i_mapping, offset, (loff_t)-1);

	inode->i_size -= len;
	binfo->disk_inode.filesize = inode->i_size;
	inode->i_blocks = briefs_compute_i_blocks(sb, &binfo->disk_inode);
	now = current_time(inode);
	inode->i_mtime_sec = now.tv_sec;
	inode->i_mtime_nsec = now.tv_nsec;
	inode->i_ctime_sec = now.tv_sec;
	inode->i_ctime_nsec = now.tv_nsec;
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_persist_and_journal_inode_warn(sb, inode, &binfo->disk_inode);
	mark_inode_dirty(inode);

out:
	inode_unlock(inode);
	if (ret == 0)
		ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * briefs_do_insert_range - FALLOC_FL_INSERT_RANGE.  Open a hole of len at
 * offset, shifting the data at/after offset up by len; i_size += len.  The VFS
 * guarantees offset/len are block-aligned and offset < i_size.  The inserted
 * range is a plain hole (reads as zero); no blocks are allocated.
 */
static long briefs_do_insert_range(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct timespec64 now;
	u64 S = offset >> BRIEFS_BLOCK_SHIFT;
	u64 L = len >> BRIEFS_BLOCK_SHIFT;
	int ret;

	/*
	 * Insert range requires block-aligned offset/len and an offset that
	 * lies within the file (offset < i_size); inserting at or beyond EOF is
	 * -EINVAL.  The VFS does not enforce either, so -- as
	 * ext4_insert_range does -- we must.  Without the alignment check a
	 * non-block-aligned request (fsstress sends one half the time) would
	 * shift extents by a sub-block count and corrupt the index; without
	 * the offset check a request past EOF would grow i_size by len over a
	 * region the shift never touched.
	 */
	if (!IS_ALIGNED(offset | len, BRIEFS_BLOCK_SIZE)) {
		ret = -EINVAL;
		goto out;
	}
	if (offset >= i_size_read(inode)) {
		ret = -EINVAL;
		goto out;
	}

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	ret = filemap_write_and_wait_range(inode->i_mapping, offset,
					   inode->i_size - 1);
	if (ret)
		goto out;

	mutex_lock(&binfo->extent_lock);
	ret = briefs_shift_extents(inode, S, L, 1, NULL);
	if (ret) {
		mutex_unlock(&binfo->extent_lock);
		goto out;
	}
	mutex_unlock(&binfo->extent_lock);

	truncate_inode_pages_range(inode->i_mapping, offset, (loff_t)-1);

	inode->i_size += len;
	binfo->disk_inode.filesize = inode->i_size;
	inode->i_blocks = briefs_compute_i_blocks(sb, &binfo->disk_inode);
	now = current_time(inode);
	inode->i_mtime_sec = now.tv_sec;
	inode->i_mtime_nsec = now.tv_nsec;
	inode->i_ctime_sec = now.tv_sec;
	inode->i_ctime_nsec = now.tv_nsec;
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_persist_and_journal_inode_warn(sb, inode, &binfo->disk_inode);
	mark_inode_dirty(inode);

out:
	inode_unlock(inode);
	if (ret == 0)
		ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * briefs_fallocate - VFS fallocate implementation.
 *
 * Supports plain pre-allocation (mode == 0), FALLOC_FL_KEEP_SIZE,
 * FALLOC_FL_PUNCH_HOLE (with KEEP_SIZE), FALLOC_FL_ZERO_RANGE,
 * FALLOC_FL_COLLAPSE_RANGE, and FALLOC_FL_INSERT_RANGE.
 */
long briefs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct timespec64 now;
	loff_t end;
	u64 start_blk, end_blk, blk;
	u64 rel, phys, run_len, rel_run, phys_run;
	u64 i, j;
	u64 added_unwritten = 0;	/* unwritten data blocks added this call */
	struct briefs_extent ext;
	bool changed = false;
	bool grew_size = false;
	int ret = 0;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_ZERO_RANGE | FALLOC_FL_COLLAPSE_RANGE |
		     FALLOC_FL_INSERT_RANGE))
		return -EOPNOTSUPP;

	if (offset < 0 || len <= 0)
		return -EINVAL;

	end = offset + len;
	if (end > inode->i_sb->s_maxbytes)
		return -EFBIG;

	if ((mode & FALLOC_FL_PUNCH_HOLE) && !(mode & FALLOC_FL_KEEP_SIZE))
		return -EINVAL;

	inode_lock(inode);

	/*
	 * The VFS fallocate path (vfs_fallocate) does not strip suid/sgid or
	 * clear security.capability, so -- as ext4_fallocate does -- do it
	 * here before any allocation or punch.  An unprivileged
	 * (non-CAP_FSETID) caller's fallocate/punch of a setid file must
	 * clear the setid bits (generic/683/684), and any fallocate of a
	 * file carrying capabilities must clear them regardless of caller
	 * (generic/688).  Under inode_lock, as file_remove_privs' notify_change
	 * requires.
	 */
	ret = file_remove_privs(file);
	if (ret)
		goto out_unlock;

	/*
	 * Enforce RLIMIT_FSIZE on an allocation that grows i_size.  The VFS
	 * fallocate path does not run setattr_prepare() for us, so -- as in
	 * briefs_setattr() -- the ulimit check that other filesystems get via
	 * inode_newsize_ok() is ours to do.  inode_newsize_ok() sends SIGXFSZ
	 * and returns -EFBIG when end grows beyond RLIMIT_FSIZE
	 * (generic/228: fallocate past the FSIZE ulimit must fail with "File
	 * too large").  KEEP_SIZE and PUNCH_HOLE never grow i_size, so they
	 * are exempt.  The inode is now locked, as inode_newsize_ok() requires.
	 * INSERT_RANGE grows i_size by len (a new hole is punched at @offset and
	 * the tail shifts up), so it must clear the ulimit gate for i_size+len too;
	 * ZERO_RANGE without KEEP_SIZE can grow i_size to end (handled here as the
	 * generic !KEEP_SIZE case).  COLLAPSE_RANGE only shrinks i_size.
	 *
	 * Compute the new size with an overflow-safe comparison: i_size may sit
	 * at s_maxbytes (MAX_LFS_FILESIZE), so "i_size + len" would wrap to
	 * negative in signed loff_t and slip past inode_newsize_ok()'s
	 * s_maxbytes check (generic/485: insert past the FS max must fail with
	 * "File too large", the XFS signed-overflow regression).  Test len
	 * against the headroom (s_maxbytes - i_size) instead.
	 */
	if (mode & FALLOC_FL_INSERT_RANGE) {
		if (len > inode->i_sb->s_maxbytes - inode->i_size) {
			inode_unlock(inode);
			return -EFBIG;
		}
		ret = inode_newsize_ok(inode, inode->i_size + len);
		if (ret) {
			inode_unlock(inode);
			return ret;
		}
	} else if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
		ret = inode_newsize_ok(inode, end);
		if (ret) {
			inode_unlock(inode);
			return ret;
		}
	}

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		briefs_stat_inc(bsi, punch_holes);
		ret = briefs_do_punch_hole(file, offset, len);
		return ret;
	}

	if (mode & FALLOC_FL_COLLAPSE_RANGE) {
		ret = briefs_do_collapse_range(file, offset, len);
		return ret;
	}

	if (mode & FALLOC_FL_INSERT_RANGE) {
		ret = briefs_do_insert_range(file, offset, len);
		return ret;
	}

	if (mode & FALLOC_FL_ZERO_RANGE) {
		ret = briefs_do_zero_range(file, mode, offset, len);
		return ret;
	}

	briefs_stat_inc(bsi, fallocate_calls);

	/*
	 * Inline-data files don't consume real blocks.  If the requested range
	 * fits entirely inside the inline region we only need to possibly grow
	 * i_size.  Otherwise we must promote to an extent-backed file first.
	 */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		if (end <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
				briefs_extent_write_begin(binfo);
				inode->i_size = end;
				binfo->disk_inode.filesize = end;
				briefs_extent_write_end(binfo);
				changed = true;
				grew_size = true;
			}
			goto out_update;
		}

		ret = briefs_promote_inline_data(inode);
		if (ret)
			goto out_unlock;
		truncate_inode_pages(inode->i_mapping, 0);
		changed = true;
	}

	/* Flush delayed allocation in the range before consulting the extent
	 * list: briefs_block_mapped() below skips blocks already in the list,
	 * but with #6 a written-but-unsynced block is BH_Delay and not yet in
	 * the list, so without this flush fallocate would allocate a *new*
	 * physical block for it while the delayed page is still bound for a
	 * different (later) allocation -- a double-alloc / data-placement
	 * mismatch.  Write the range back first so the skip logic sees reality.
	 */
	ret = filemap_write_and_wait_range(inode->i_mapping, offset, end - 1);
	if (ret)
		goto out_unlock;

	start_blk = offset >> BRIEFS_BLOCK_SHIFT;
	end_blk = (end + BRIEFS_BLOCK_SIZE - 1) >> BRIEFS_BLOCK_SHIFT;

	/*
	 * Pre-allocate blocks for the requested range.  Skip blocks already
	 * mapped; for each maximal run of unmapped blocks, try to allocate the
	 * whole run contiguously with one briefs_alloc_blocks() call and one
	 * extent append (its merge logic handles len=k).  If no contiguous run
	 * of that length fits (or run_len == 1), fall back to per-block
	 * allocation so fragmented fallocate still succeeds and we never
	 * regress on ENOSPC.
	 *
	 * LOCK ORDER FIX (deadlock prevention): We must take extent_lock BEFORE
	 * alloc->lock to match the global lock order (extent_lock -> alloc->lock
	 * -> j->write_lock). The old code called briefs_alloc_blocks() (takes
	 * alloc->lock) then briefs_append_extent_nojournal() (takes extent_lock),
	 * inverting the order used by briefs_iomap_begin_common and
	 * briefs_btree_insert_locked. Now we take extent_lock first, do an
	 * unlocked alloc->lock section for allocation, then insert under the
	 * held extent_lock.
	 */
	blk = start_blk;
	while (blk < end_blk) {
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto falloc_loop_done;
		}
		/* Unlocked check for already-mapped blocks (fast path). */
		if (briefs_block_mapped(inode, blk)) {
			blk++;
			continue;
		}
		run_len = 1;
		while (blk + run_len < end_blk &&
		       !briefs_block_mapped(inode, blk + run_len))
			run_len++;

		if (run_len > 1) {
			/* Allocate the run (takes alloc->lock). */
			rel_run = briefs_alloc_blocks(&bsi->alloc, run_len);
			if (rel_run != 0) {
				phys_run = data_to_abs(bsi->sb, rel_run);
				/*
				 * The run is recorded as BRIEFS_EXT_UNWRITTEN, so
				 * the iomap read path maps it to IOMAP_UNWRITTEN and
				 * returns zeros without reading these data blocks; a
				 * later write converts just the written blocks,
				 * splitting the extent so the un-written wings stay
				 * unwritten (briefs_convert_unwritten_range).  Do
				 * NOT zero the blocks here: zeroing would do a
				 * synchronous sync_dirty_buffer per block, so a large
				 * fallocate issued millions of sync writes and ran
				 * for hours (generic/103: 100 GB fill), and the loop
				 * never checked fatal_signal_pending() so it could
				 * not be interrupted (SIGKILL would not land).  The
				 * per-block fallback below likewise stores unwritten
				 * extents and skips the zeroing.
				 */
				/* Take extent_lock BEFORE insert to maintain lock order. */
				mutex_lock(&binfo->extent_lock);
				ext.offset = blk;
				ext.phys = phys_run;
				ext.len = run_len;
				ext.flags = BRIEFS_EXT_UNWRITTEN;
				ret = briefs_btree_insert_locked(inode->i_sb,
								 &binfo->disk_inode, &ext);
				mutex_unlock(&binfo->extent_lock);
				if (ret == -EEXIST) {
					/*
					 * A concurrent writer mapped part of this
					 * run after our unlocked briefs_block_mapped
					 * check.  Free the run and retry block-by-
					 * block via the per-block fallback below,
					 * which skips the now-mapped block(s) on
					 * their own -EEXIST.
					 */
					for (j = 0; j < run_len; j++)
						briefs_free_block(&bsi->alloc,
								  rel_run + j);
					/* fall through to per-block fallback */
				} else {
					if (ret) {
						for (j = 0; j < run_len; j++)
							briefs_free_block(&bsi->alloc,
									  rel_run + j);
						goto falloc_loop_done;
					}
					changed = true;
					added_unwritten += run_len;
					blk += run_len;
					continue;
				}
			}
		}

		/* per-block fallback: run_len == 1, or no contiguous run fit */
		for (i = 0; i < run_len; i++) {
			if (fatal_signal_pending(current)) {
				ret = -EINTR;
				goto falloc_loop_done;
			}
			/* Allocate single block (takes alloc->lock). */
			rel = briefs_alloc_block(&bsi->alloc);
			if (rel == 0) {
				ret = -ENOSPC;
				goto falloc_loop_done;
			}
			phys = data_to_abs(bsi->sb, rel);

			/* No zeroing: the block is recorded unwritten (see above). */

			/* Take extent_lock BEFORE insert to maintain lock order. */
			mutex_lock(&binfo->extent_lock);
			ext.offset = blk + i;
			ext.phys = phys;
			ext.len = 1;
			ext.flags = BRIEFS_EXT_UNWRITTEN;
			ret = briefs_btree_insert_locked(inode->i_sb,
							 &binfo->disk_inode, &ext);
			mutex_unlock(&binfo->extent_lock);
			if (ret == -EEXIST) {
				/* Block mapped by a concurrent writer after our
				 * unlocked check; skip it (already allocated by
				 * the racer). */
				briefs_free_block(&bsi->alloc, rel);
				continue;
			}
			if (ret) {
				briefs_free_block(&bsi->alloc, rel);
				goto falloc_loop_done;
			}
			changed = true;
			added_unwritten++;
		}
		blk += run_len;
	}
falloc_loop_done:

	/*
	 * Reserve worst-case B+tree metadata for the unwritten blocks this
	 * fallocate added (ext4-style).  The reserve is a count held in
	 * bsi->alloc.meta_shield (not bitmap bits), so a later fill to 100%
	 * stops at free_count == meta_shield, leaving those blocks free for the
	 * splits that fragmenting this prealloc will need
	 * (briefs_alloc_block_meta bypasses the shield).  Raised after the loop
	 * so it covers exactly the blocks actually inserted as unwritten (a
	 * re-fallocate over already-unwritten blocks adds 0 -> no double-count).
	 * Raised even on partial/EINTR: the inserted unwritten blocks persist
	 * (journaled) and could be fragmented later, so they need the reserve.
	 */
	if (added_unwritten)
		briefs_raise_unwritten_reserve(inode, added_unwritten);

	if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
		/* Zero the tail of the old-EOF block before growing i_size past
		 * it.  A mid-block EOF block is delalloc (BH_Delay, possibly
		 * mmap-polluted by fsx -e pollute_eofpage); left unzeroed, its
		 * stale tail persists and leaks as valid data once i_size
		 * advances past the block (generic/363: FALLOC PAST_EOF after a
		 * mid-block MAPWRITE).  filemap_write_and_wait_range above only
		 * flushed [offset,end); the old-EOF block lies before offset, so
		 * it is still a cached delayed folio here -- briefs_zero_eof_tail
		 * zeroes its tail directly. */
		if (inode->i_size & (BRIEFS_BLOCK_SIZE - 1))
			briefs_zero_eof_tail(inode->i_mapping, inode->i_size);
		inode->i_size = end;
		binfo->disk_inode.filesize = end;
		changed = true;
		grew_size = true;
	}

	inode->i_blocks = briefs_compute_i_blocks(inode->i_sb, &binfo->disk_inode);

	briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
			&binfo->disk_inode);

out_update:
	if (changed) {
		now = current_time(inode);
		inode->i_ctime_sec = now.tv_sec;
		inode->i_ctime_nsec = now.tv_nsec;
		if (grew_size) {
			inode->i_mtime_sec = now.tv_sec;
			inode->i_mtime_nsec = now.tv_nsec;
		}
		briefs_sync_inode_times(inode, &binfo->disk_inode);
		mark_inode_dirty(inode);
	}

out_unlock:
	inode_unlock(inode);
	if (ret == 0)
		ret = briefs_inode_sync(inode);
	return ret;
}

/*
 * briefs_symlink - create a symbolic link.
 * Stores the symlink target path as file data using the normal
 * data block / extent mechanism.
 */
int briefs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			   struct dentry *dentry, const char *symname)
{
	struct briefs_sb_info *bsi = dir->i_sb->s_fs_info;
	struct inode *inode;
	int ret;
	size_t len = strlen(symname);

	pr_debug("briefs: symlink %pd -> %s in dir %lu\n", dentry, symname, dir->i_ino);

	if (len == 0 || len > BRIEFS_NAME_LEN * 10)
		return -ENAMETOOLONG;

	inode = briefs_new_inode(idmap, dir, dentry, S_IFLNK | 0777, 0);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	/* Set the symlink size in the VFS and disk inode */
	inode->i_size = len;
	briefs_i(inode)->disk_inode.filesize = len;

	/*
	 * Store the symlink target.  Short targets (<= 256 bytes) are kept
	 * directly in the inode inline_data region; larger targets use a
	 * data block and extent as before.
	 */
	if (len > 0 && len <= BRIEFS_INODE_INLINE_DATA_SIZE) {
		struct briefs_inode_info *binfo = briefs_i(inode);

		memset(binfo->disk_inode.inline_data, 0,
		       sizeof(binfo->disk_inode.inline_data));
		memcpy(binfo->disk_inode.inline_data, symname, len);
		binfo->disk_inode.flags |= InodeFlagInlineData;
		inode->i_blocks = 0;

		/* Persist updated inode with inline target */
		briefs_persist_disk_inode(dir->i_sb, inode->i_ino, &binfo->disk_inode, false);
	} else if (len > BRIEFS_INODE_INLINE_DATA_SIZE) {
		struct buffer_head *bh;
		u64 rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0) {
			briefs_create_abort(dir->i_sb, dir, inode, &dentry->d_name, false);
			return -ENOSPC;
		}
		u64 phys = data_to_abs(bsi->sb, rel);

		bh = sb_bread(dir->i_sb, phys);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			briefs_create_abort(dir->i_sb, dir, inode, &dentry->d_name, false);
			return -EIO;
		}
		memset(bh->b_data, 0, dir->i_sb->s_blocksize);
		memcpy(bh->b_data, symname, len);
		briefs_mark_buffer_dirty(bh, dir->i_sb);

		/*
		 * Journal the symlink target bytes so replay can restore them even
		 * if the data block was not flushed before a crash.
		 */
		ret = briefs_journal_symlink_data(bsi->journal, inode->i_ino, phys,
						symname, len);
		if (ret) {
			brelse(bh);
			briefs_free_block(&bsi->alloc, rel);
			briefs_create_abort(dir->i_sb, dir, inode, &dentry->d_name, false);
			return ret;
		}

		brelse(bh);

		struct briefs_extent ext;
		ext.offset = 0;
		ext.phys = phys;
		ext.len = 1;
		ext.flags = 0;
		ret = briefs_append_extent(dir->i_sb, &briefs_i(inode)->disk_inode, &ext);
		if (ret != 0) {
			briefs_free_block(&bsi->alloc, rel);
			briefs_create_abort(dir->i_sb, dir, inode, &dentry->d_name, false);
			return ret;
		}

		inode->i_blocks = (BRIEFS_BLOCK_SIZE / 512);
		briefs_i(inode)->disk_inode.num_extents_total = 1;
		briefs_i(inode)->disk_inode.num_extents_inline = 1;
		memcpy(&briefs_i(inode)->disk_inode.inline_extents[0], &ext, sizeof(ext));

		/* Persist updated inode with extent */
		briefs_persist_disk_inode(dir->i_sb, inode->i_ino, &briefs_i(inode)->disk_inode, false);
	}

	ret = briefs_finish_create(dir, dentry, inode, 1);
	if (ret)
		return ret;

	d_instantiate(dentry, inode);

	pr_debug("briefs: symlink inode %lu -> %s added to dir\n", inode->i_ino, symname);
	ret = briefs_inode_sync(dir);
	return ret;
}
/*
 * briefs_mknod - create a special file (block, char, fifo, socket).
 */
int briefs_mknod(struct mnt_idmap *idmap, struct inode *dir,
			 struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;
	int ret;

	pr_debug("briefs: mknod %pd (mode=%o, rdev=%u:%u) in dir %lu\n",
		 dentry, mode, MAJOR(rdev), MINOR(rdev), dir->i_ino);

	inode = briefs_new_inode(idmap, dir, dentry, mode, rdev);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ret = briefs_finish_create(dir, dentry, inode, 1);
	if (ret)
		return ret;

	d_instantiate(dentry, inode);

	pr_debug("briefs: mknod inode %lu (mode=%o) added to dir\n", inode->i_ino, mode);
	ret = briefs_inode_sync(dir);
	return ret;
}
/*
 * briefs_get_link - read the symlink target path.
 * Called by the VFS when following a symlink.
 */
const char *briefs_get_link(struct dentry *dentry, struct inode *inode,
				    struct delayed_call *done)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	char *link;

	pr_debug("briefs: get_link inode=%lu\n", inode->i_ino);

	if (!dentry)
		return ERR_PTR(-ECHILD);

	if (inode->i_size == 0)
		return ERR_PTR(-ENOENT);

	/* Allocate a kernel buffer for the target path */
	link = kmalloc(inode->i_size + 1, GFP_KERNEL);
	if (!link)
		return ERR_PTR(-ENOMEM);

	/* Read the target from inline data or the first extent. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		memcpy(link, binfo->disk_inode.inline_data, inode->i_size);
		link[inode->i_size] = '\0';
	} else if (binfo->disk_inode.num_extents_total > 0) {
		struct briefs_extent ext;
		int ret;

		ret = briefs_read_extent(inode->i_sb, &binfo->disk_inode, 0, &ext);
		if (ret != 0) {
			kfree(link);
			return ERR_PTR(ret);
		}

		struct buffer_head *bh = sb_bread(inode->i_sb, ext.phys);
		if (!bh) {
			kfree(link);
			return ERR_PTR(-EIO);
		}

		memcpy(link, bh->b_data, inode->i_size);
		link[inode->i_size] = '\0';
		brelse(bh);
	} else {
		kfree(link);
		return ERR_PTR(-EIO);
	}

	set_delayed_call(done, kfree_link, link);
	return link;
}
/*
 * iomap address_space_operations for BrieFS regular files.
 *
 * Read, writeback, and folio reclaim all go through iomap; the buffered-write
 * path (briefs_write_iter -> briefs_iomap_buffered_write) calls
 * iomap_file_buffered_write directly, so this aops has no write_begin/write_end
 * and block_write_begin never attaches a buffer_head head to a data folio.  With
 * no buffer_heads on data folios, iomap-submitted read/write bios never race a
 * buffer_head completion handler on a shared page -- the mix that crashed the
 * kernel when only read was on iomap.  Metadata inodes use buffer_head via their
 * own paths; only regular-file data uses this.
 *
 * .bmap uses iomap_bmap (FIBMAP + the swapfile activation fallback);
 * .swap_activate wraps iomap_swapfile_activate.  .direct_IO is noop_direct_IO:
 * it is never called -- DIO is routed by briefs_read_iter/briefs_write_iter to
 * iomap_dio_rw directly, bypassing the aops write_begin/end -- but dentry_open()
 * gates O_DIRECT opens on a_ops->direct_IO being non-NULL (it sets
 * FMODE_CAN_ODIRECT from it, else returns -EINVAL), so the no-op must stay.
 */
static int briefs_iomap_read_folio(struct file *file, struct folio *folio)
{
	return iomap_read_folio(folio, &briefs_iomap_ops);
}

static void briefs_iomap_readahead(struct readahead_control *rac)
{
	iomap_readahead(rac, &briefs_iomap_ops);
}

static int briefs_iomap_writepages(struct address_space *mapping,
				   struct writeback_control *wbc)
{
	struct iomap_writepage_ctx wpc = { };

	return iomap_writepages(mapping, wbc, &wpc, &briefs_writeback_ops);
}

/*
 * briefs_iomap_bmap - translate a logical file block to its physical device
 * block via the iomap extent map.  Used by FIBMAP and by the swapfile
 * activation fallback.  iomap_bmap walks briefs_iomap_ops (the read/report
 * translation) and returns the absolute physical block for a mapped block or 0
 * for a hole.
 */
static sector_t briefs_iomap_bmap(struct address_space *mapping, sector_t block)
{
	return iomap_bmap(mapping, block, &briefs_iomap_ops);
}

/*
 * briefs_iomap_swap_activate - activate a regular file as a swapfile via the
 * iomap extent map.  iomap_swapfile_activate walks the file with
 * briefs_iomap_ops (IOMAP_REPORT), rejecting holes, unwritten extents, inline
 * data, and delalloc -- a swapfile must be fully of mapped, written, contiguous
 * extents -- and builds the swap extent list the swap layer consumes.  The test
 * creates the swapfile by writing it out, so every block is MAPPED.  The extent
 * map is independent of the data read/write path.
 */
static int briefs_iomap_swap_activate(struct swap_info_struct *sis,
				      struct file *file, sector_t *span)
{
	return iomap_swapfile_activate(sis, file, span, &briefs_iomap_ops);
}

const struct address_space_operations briefs_iomap_aops = {
	.read_folio	= briefs_iomap_read_folio,
	.readahead	= briefs_iomap_readahead,
	.writepages	= briefs_iomap_writepages,
	.dirty_folio	= iomap_dirty_folio,
	.invalidate_folio = iomap_invalidate_folio,
	.release_folio	= iomap_release_folio,
	.is_partially_uptodate = iomap_is_partially_uptodate,
	.migrate_folio	= filemap_migrate_folio,
	.bmap		= briefs_iomap_bmap,
	.direct_IO	= noop_direct_IO,
	.swap_activate	= briefs_iomap_swap_activate,
};

/*
 * mmap write support via .page_mkwrite.
 *
 * A plain generic_file_mmap path (no .page_mkwrite) would let a write fault
 * into a hole read the folio (zero-filled by iomap_read_folio) and dirty the
 * PTE without ever allocating a block; the block would then be allocated
 * during ->writeback by briefs_writeback_map_blocks (write=true) -- i.e. the
 * writeback worker takes extent_lock, runs briefs_btree_insert_locked and
 * mark_inode_dirty, and reaches the journal, all from inside the writeback
 * path.  Under the fsx+mmap workload of generic/127 that writeback-time
 * allocation deadlocks (silent full freeze, no soft-lockup trace: both CPUs
 * stuck with IRQs disabled).
 *
 * iomap_page_mkwrite instead allocates the block at fault time -- in the
 * faulting task, through briefs_write_iomap_ops.begin -- and marks the folio
 * dirty, so by the time writeback runs the folio is already MAPPED and
 * briefs_writeback_map_blocks just returns the cached mapping (no allocation,
 * no extent_lock, no mark_inode_dirty, no journal) and writeback submits a
 * plain bio.  This is the zonefs pattern: sb_start_pagefault / file_update_time
 * / filemap_invalidate_lock_shared around iomap_page_mkwrite, with
 * filemap_fault + filemap_map_pages for read faults.
 */
static vm_fault_t briefs_vm_page_mkwrite(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;

	sb_start_pagefault(inode->i_sb);
	file_update_time(vmf->vma->vm_file);
	filemap_invalidate_lock_shared(inode->i_mapping);
	ret = iomap_page_mkwrite(vmf, &briefs_write_iomap_ops);
	filemap_invalidate_unlock_shared(inode->i_mapping);
	sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct briefs_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite	= briefs_vm_page_mkwrite,
};

int briefs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	vma->vm_ops = &briefs_file_vm_ops;
	return 0;
}
