/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/seqlock.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/migrate.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"
#include "briefs_debug.h"
#include "briefs_iomap.h"

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

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	ret = sync_inode_metadata(inode, 1);
	if (ret)
		return ret;

	/* Flush journal to disk on explicit fsync */
	if (bsi->journal && bsi->journal->dirty) {
		ret = briefs_journal_sync(bsi->journal);
	}

	return ret;
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
 * FS_IOC_FSSETXATTR and every other cmd fall through to -ENOTTY, preserving
 * the prior behavior (no handler existed, so the VFS returned -ENOTTY for all
 * ioctls): xflag/chattr-style tests that expect the feature unsupported keep
 * failing/notrun cleanly rather than silently no-op'ing.
 */
long briefs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case FS_IOC_FSGETXATTR: {
		struct fsxattr fsx;

		memset(&fsx, 0, sizeof(fsx));
		if (copy_to_user((void __user *)arg, &fsx, sizeof(fsx)))
			return -EFAULT;
		return 0;
	}
	}

	return -ENOTTY;
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
	struct buffer_head *bh;

	if (!(binfo->disk_inode.flags & InodeFlagInlineData))
		return 0;

	/* Empty inline file: just clear the flag, let the write path allocate. */
	if (old_size == 0) {
		write_seqcount_begin(&binfo->extent_seq);
		binfo->disk_inode.flags &= ~InodeFlagInlineData;
		write_seqcount_end(&binfo->extent_seq);
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
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	write_seqcount_begin(&binfo->extent_seq);
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
	write_seqcount_end(&binfo->extent_seq);

	inode->i_blocks = (BRIEFS_BLOCK_SIZE / 512);

	briefs_journal_extent_alloc(bsi->journal, inode->i_ino, 0, phys, 1, 0);
	{
		struct briefs_disk_inode disk_di;
		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
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

	return generic_file_read_iter(iocb, to);
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
	ret = iomap_file_buffered_write(iocb, from, &briefs_write_iomap_ops,
					 NULL);
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

	if ((binfo->disk_inode.flags & InodeFlagInlineData) || inode->i_size == 0) {
		if (total_size <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			u8 tmp[BRIEFS_INODE_INLINE_DATA_SIZE];

			if (copy_from_iter(tmp, count, from) != count) {
				inode_unlock(inode);
				return -EFAULT;
			}
			memcpy(binfo->disk_inode.inline_data + pos, tmp, count);

			write_seqcount_begin(&binfo->extent_seq);
			binfo->disk_inode.flags |= InodeFlagInlineData;
			if (total_size > inode->i_size) {
				inode->i_size = total_size;
				binfo->disk_inode.filesize = total_size;
			}
			write_seqcount_end(&binfo->extent_seq);

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
			return count;
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
	write_seqcount_begin(&binfo->extent_seq);
	di->flags &= ~InodeFlagIndexed;
	di->extent_inline_base = 0;
	di->num_extents_inline = 0;
	di->num_extents_total = 0;
	memset(di->inline_extents, 0, sizeof(di->inline_extents));
	binfo->cached_max_end = 0;
	write_seqcount_end(&binfo->extent_seq);

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

			ret = iomap_truncate_page(inode, new_size, &did_zero,
						  &briefs_write_iomap_ops);
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
			write_seqcount_begin(&binfo->extent_seq);
			memset(binfo->disk_inode.inline_data + old_size, 0,
			       new_size - old_size);
			binfo->disk_inode.filesize = new_size;
			inode->i_size = new_size;
			inode->i_blocks = 0;
			write_seqcount_end(&binfo->extent_seq);

			briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
						  &binfo->disk_inode, false);
			{
				struct briefs_disk_inode disk_di;
				briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
				briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
			}
		} else {
			ret = briefs_promote_inline_data(inode);
			if (ret)
				goto out_unlock;

			/* Promotion allocated one block; set the new size. */
			binfo->disk_inode.filesize = new_size;
			inode->i_size = new_size;
			inode->i_blocks = (BRIEFS_BLOCK_SIZE / 512);
			briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
						  &binfo->disk_inode, false);
			{
				struct briefs_disk_inode disk_di;
				briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
				briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
			}
		}
		mutex_unlock(&binfo->extent_lock);
		return 0;
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
		briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
					  &binfo->disk_inode, false);
		{
			struct briefs_disk_inode disk_di;
			briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
			briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
		}
		mutex_unlock(&binfo->extent_lock);
		mark_inode_dirty(inode);
		return 0;
	}

	pr_debug("briefs: setattr truncate ino=%lu %llu -> %llu\n",
		inode->i_ino, old_size, new_size);

	/* Inline-data truncate is handled separately. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		write_seqcount_begin(&binfo->extent_seq);
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
		write_seqcount_end(&binfo->extent_seq);

		briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
					  &binfo->disk_inode, false);
		{
			struct briefs_disk_inode disk_di;
			briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
			briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
		}
		mutex_unlock(&binfo->extent_lock);
		return 0;
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
					briefs_journal_extent_free(bsi->journal,
								   inode->i_ino,
								   e->offset, e->phys,
								   e->len);
					briefs_free_blocks_range(bsi, e->phys, e->len);
				}
			}

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
	briefs_persist_disk_inode(inode->i_sb, inode->i_ino, &binfo->disk_inode, false);

	/*
	 * Log a full snapshot after truncate so replay restores the exact
	 * extent list and size, not just the freed bitmap bits.
	 */
	{
		struct briefs_disk_inode disk_di;
		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
	}

	mutex_unlock(&binfo->extent_lock);
	return 0;

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
	binfo->disk_inode.filemode = inode->i_mode;
	binfo->disk_inode.uid = from_kuid(&init_user_ns, inode->i_uid);
	binfo->disk_inode.gid = from_kgid(&init_user_ns, inode->i_gid);
	binfo->disk_inode.nlinks = inode->i_nlink;
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	mark_inode_dirty(inode);
	return 0;
}
/* briefs_getattr - get file attributes */
int briefs_getattr(struct mnt_idmap *idmap, const struct path *path,

                   struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 i_blocks;

	generic_fillattr(idmap, request_mask, inode, stat);

	/* Inline-data files consume no data blocks. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		i_blocks = 0;
	} else {
		/* Recompute i_blocks from all extents in a single in-order walk. */
		i_blocks = briefs_compute_i_blocks(inode->i_sb, &binfo->disk_inode);
	}
	inode->i_blocks = i_blocks;
	stat->blocks = i_blocks;
	pr_debug("briefs: getattr ino=%lu i_blocks=%llu\n", inode->i_ino, i_blocks);
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
 * briefs_zero_block - ensure the on-disk block at @abs_block is zero-filled.
 */
static int briefs_zero_block(struct super_block *sb, u64 abs_block)
{
	struct buffer_head *bh;

	bh = briefs_get_zero_block(sb, abs_block);
	if (!bh)
		return -EIO;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
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
	struct briefs_disk_inode disk_di;
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
			briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
						  &binfo->disk_inode, false);
			briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
			briefs_journal_inode_full(bsi->journal, inode->i_ino,
						  &disk_di);
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
				if (ret)
					goto out_unlock;
			}

			/*
			 * Zero the partially-punched boundary blocks. They remain
			 * allocated (delete_range kept them as straddlers), so look
			 * them up and zero the punched portion. -ENOENT means the
			 * boundary was already a hole: nothing to zero.
			 */
			if (same_boundary_block) {
				struct briefs_extent pext;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 start_blk, &pext,
								 true);
				if (ret == 0) {
					u64 ab = pext.phys + (start_blk - pext.offset);

					ret = briefs_zero_block_range(inode->i_sb, ab,
								      partial_start_off,
								      partial_end_off);
					if (ret)
						goto out_unlock;
					changed = true;
				} else if (ret == -ENOENT) {
					ret = 0;
				} else {
					goto out_unlock;
				}
			} else {
			if (need_partial_start) {
				struct briefs_extent pext;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 start_blk, &pext,
								 true);
				if (ret == 0) {
					u64 ab = pext.phys + (start_blk - pext.offset);

					ret = briefs_zero_block_range(inode->i_sb, ab,
								      partial_start_off,
								      BRIEFS_BLOCK_SIZE);
					if (ret)
						goto out_unlock;
					changed = true;
				} else if (ret == -ENOENT) {
					ret = 0;
				} else {
					goto out_unlock;
				}
			}
			if (need_partial_end) {
				struct briefs_extent pext;
				u64 pblk = end_blk - 1;

				ret = briefs_inode_lookup_iblock(inode->i_sb, binfo,
								 pblk, &pext,
								 true);
				if (ret == 0) {
					u64 ab = pext.phys + (pblk - pext.offset);

					ret = briefs_zero_block_range(inode->i_sb, ab,
								      0, partial_end_off);
					if (ret)
						goto out_unlock;
					changed = true;
				} else if (ret == -ENOENT) {
					ret = 0;
				} else {
					goto out_unlock;
				}
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
			 */
			if (start_blk >= ext.offset && start_blk < ext_end) {
				u64 abs_block = ext.phys + (start_blk - ext.offset);

				ret = briefs_zero_block_range(inode->i_sb, abs_block,
							      partial_start_off,
							      partial_end_off);
				if (ret)
					goto out_unlock;
			}
		} else {
		if (need_partial_start && start_blk >= ext.offset &&
		    start_blk < ext_end) {
			u64 abs_block = ext.phys + (start_blk - ext.offset);

			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      partial_start_off,
						      BRIEFS_BLOCK_SIZE);
			if (ret)
				goto out_unlock;
		}

		if (need_partial_end && (end_blk - 1) >= ext.offset &&
		    (end_blk - 1) < ext_end) {
			u64 abs_block = ext.phys + ((end_blk - 1) - ext.offset);

			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      0,
						      partial_end_off);
			if (ret)
				goto out_unlock;
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

			briefs_journal_extent_free(bsi->journal, inode->i_ino,
						   free_start, free_phys,
						   free_len);
			briefs_free_blocks_range(bsi, free_phys, free_len);
			changed = true;
		}
	}
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

	briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
				  &binfo->disk_inode, false);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

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
	return ret;
}

/*
 * briefs_fallocate - VFS fallocate implementation.
 *
 * Supports plain pre-allocation (mode == 0), FALLOC_FL_KEEP_SIZE, and
 * FALLOC_FL_PUNCH_HOLE (which must be combined with FALLOC_FL_KEEP_SIZE).
 */
long briefs_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_disk_inode disk_di;
	struct timespec64 now;
	loff_t end;
	u64 start_blk, end_blk, blk;
	u64 rel, phys, run_len, rel_run, phys_run;
	u64 i, j;
	struct briefs_extent ext;
	bool changed = false;
	bool grew_size = false;
	int ret = 0;

	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE))
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
	 * Enforce RLIMIT_FSIZE on an allocation that grows i_size.  The VFS
	 * fallocate path does not run setattr_prepare() for us, so -- as in
	 * briefs_setattr() -- the ulimit check that other filesystems get via
	 * inode_newsize_ok() is ours to do.  inode_newsize_ok() sends SIGXFSZ
	 * and returns -EFBIG when end grows beyond RLIMIT_FSIZE
	 * (generic/228: fallocate past the FSIZE ulimit must fail with "File
	 * too large").  KEEP_SIZE and PUNCH_HOLE never grow i_size, so they
	 * are exempt.  The inode is now locked, as inode_newsize_ok() requires.
	 */
	if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
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

	briefs_stat_inc(bsi, fallocate_calls);

	/*
	 * Inline-data files don't consume real blocks.  If the requested range
	 * fits entirely inside the inline region we only need to possibly grow
	 * i_size.  Otherwise we must promote to an extent-backed file first.
	 */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		if (end <= BRIEFS_INODE_INLINE_DATA_SIZE) {
			if (!(mode & FALLOC_FL_KEEP_SIZE) && end > inode->i_size) {
				write_seqcount_begin(&binfo->extent_seq);
				inode->i_size = end;
				binfo->disk_inode.filesize = end;
				write_seqcount_end(&binfo->extent_seq);
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
	 * of that length fits (or run_len == 1), fall back to the original
	 * per-block allocation so fragmented fallocate still succeeds and we
	 * never regress on ENOSPC.  Free per-block (not briefs_free_blocks_range,
	 * which 1M-caps) in error paths so large runs can't hit the cap.
	 */
	blk = start_blk;
	while (blk < end_blk) {
		if (briefs_block_mapped(inode, blk)) {
			blk++;
			continue;
		}
		run_len = 1;
		while (blk + run_len < end_blk &&
		       !briefs_block_mapped(inode, blk + run_len))
			run_len++;

		if (run_len > 1) {
			rel_run = briefs_alloc_blocks(&bsi->alloc, run_len);
			if (rel_run != 0) {
				phys_run = data_to_abs(bsi->sb, rel_run);
				for (i = 0; i < run_len; i++) {
					ret = briefs_zero_block(inode->i_sb,
							       phys_run + i);
					if (ret) {
						for (j = 0; j < run_len; j++)
							briefs_free_block(&bsi->alloc,
									  rel_run + j);
						goto falloc_loop_done;
					}
				}
				ext.offset = blk;
				ext.phys = phys_run;
				ext.len = run_len;
				ext.flags = BRIEFS_EXT_UNWRITTEN;
				ret = briefs_append_extent_nojournal(inode->i_sb,
								     &binfo->disk_inode,
								     &ext);
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
					blk += run_len;
					continue;
				}
			}
		}

		/* per-block fallback: run_len == 1, or no contiguous run fit */
		for (i = 0; i < run_len; i++) {
			rel = briefs_alloc_block(&bsi->alloc);
			if (rel == 0) {
				ret = -ENOSPC;
				goto falloc_loop_done;
			}
			phys = data_to_abs(bsi->sb, rel);

			ret = briefs_zero_block(inode->i_sb, phys);
			if (ret) {
				briefs_free_block(&bsi->alloc, rel);
				goto falloc_loop_done;
			}

			ext.offset = blk + i;
			ext.phys = phys;
			ext.len = 1;
			ext.flags = BRIEFS_EXT_UNWRITTEN;

			ret = briefs_append_extent_nojournal(inode->i_sb,
							     &binfo->disk_inode,
							     &ext);
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
		}
		blk += run_len;
	}
falloc_loop_done:

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

	briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
				  &binfo->disk_inode, false);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

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
		mark_buffer_dirty(bh);

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
	return 0;
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
	return 0;
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
