/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/falloc.h>
#include <linux/fiemap.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/seqlock.h>
#include <linux/pagemap.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"
#include "briefs_debug.h"

/* address_space_operations wrappers (kernel 6.12 folio-based APIs). */

/* read_folio: wrapper calling mpage_read_folio with our get_block */
static int briefs_read_folio(struct file *file, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	struct briefs_inode_info *binfo = briefs_i(inode);

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		loff_t pos = (loff_t)folio->index << PAGE_SHIFT;
		size_t folio_size = PAGE_SIZE;
		void *addr = folio_address(folio);
		size_t copy_len;

		if (pos >= inode->i_size) {
			memset(addr, 0, folio_size);
		} else {
			copy_len = min_t(size_t, inode->i_size - pos, folio_size);
			memcpy(addr, binfo->disk_inode.inline_data + pos, copy_len);
			if (copy_len < folio_size)
				memset((u8 *)addr + copy_len, 0, folio_size - copy_len);
		}

		flush_dcache_folio(folio);
		folio_mark_uptodate(folio);
		folio_unlock(folio);
		return 0;
	}

	{
		int rf_ret = mpage_read_folio(folio, briefs_get_block);
		return rf_ret;
	}
}
/*
 * #6 Step 3: run-allocating writepages.
 *
 * write_begin defers allocation (BH_Delay); this writepages coalesces the
 * resulting dirty delayed folios into contiguous runs and allocates each run
 * with ONE briefs_alloc_blocks(run) call and ONE extent append -- which emits
 * ONE JRN_EXTENT_ALLOC(len=run) journal record instead of one per block.  On a
 * fresh/unfragmented fs the per-run extents also merge into a single extent
 * for the whole sequential write (the alloc blocks come back contiguous, so
 * briefs_btree_insert_locked's merge-with-neighbor branch grows the last extent by `run`).
 *
 * writeback_iter() hands us the dirty folios of the wbc range one at a time,
 * ascending, locked and with the pagecache dirty tag already cleared (ready to
 * write).  We accumulate a maximal run of consecutive single-block delayed
 * folios (blocksize == PAGE_SIZE -> 1 block/folio), holding them locked, then
 * flush the run: allocate a contiguous physical run, append one extent for it,
 * and write each folio back bound to phys_run+i.  Folios that are not run
 * candidates (already-mapped overwrites, multi-block folios, or non-delay
 * dirty buffers) break the run and are written via the normal buffer path.
 *
 * Crash ordering is unchanged from Step 2: allocation + extent append happen
 * here in .writepages, strictly before briefs_write_inode (->journal_inode_full)
 * runs; fsync does file_write_and_wait_range (writeback) before
 * sync_inode_metadata.  Every block of a run is written in the same writeback
 * pass that appends its extent, so the extent is never journaled ahead of its
 * data.  mark_inode_dirty is called once per writeback (after all runs), not
 * per block.
 *
 * Bounding the run caps the number of folios held locked at once; on kmalloc
 * failure we fall back to mpage_writepages (the Step 2 per-block path).
 */
#define BRIEFS_DELALLOC_RUN_MAX	256

/* block_write_full_folio() isn't exported to modules (only __block_write_full_
 * folio is), so replicate its i_size handling around the exported primitive.
 * Used for the non-run writeback path (overwrites / multi-block folios). */
static int briefs_block_write_full_folio(struct folio *folio,
					 struct writeback_control *wbc,
					 get_block_t get_block)
{
	struct inode *inode = folio->mapping->host;
	loff_t i_size = i_size_read(inode);

	if (folio_pos(folio) + folio_size(folio) <= i_size)
		return __block_write_full_folio(inode, folio, get_block, wbc);
	if (folio_pos(folio) >= i_size) {
		folio_unlock(folio);
		return 0;
	}
	folio_zero_segment(folio, offset_in_folio(folio, i_size),
			   folio_size(folio));
	return __block_write_full_folio(inode, folio, get_block, wbc);
}

/* Write a folio whose single buffer has already been mapped to @phys (no
 * get_block call).  The folio data (user bytes + zeroed tails from
 * briefs_write_begin) is correct, so we must not zero -- only invalidate any
 * stale blockdev alias for the freshly allocated physical block, mirroring what
 * __block_write_full_folio does in its get_block path (set_buffer_new +
 * clean_bdev_bh_alias).  __block_write_full_folio then sees a mapped, !delay
 * buffer and submits it directly.  Run folios are delayed write folios, hence
 * within i_size with tails already zeroed by write_begin, so the i_size-straddle
 * wrapper isn't needed here.  It unlocks the folio. */
static int briefs_write_folio_mapped(struct inode *inode,
				     struct writeback_control *wbc,
				     struct folio *folio, sector_t phys)
{
	struct buffer_head *bh = folio_buffers(folio);

	map_bh(bh, inode->i_sb, phys);
	clear_buffer_delay(bh);
	set_buffer_new(bh);
	clean_bdev_bh_alias(bh);
	clear_buffer_new(bh);
	return __block_write_full_folio(inode, folio, briefs_get_block, wbc);
}

/* Per-block fallback: allocate one block, append a len=1 extent, map, and write
 * the folio.  This is the Step 2 writeback path (one JRN_EXTENT_ALLOC(len=1)
 * per block), used when no contiguous run of `n` could be allocated or when a
 * run extent append failed.  On ENOSPC the folio is redirtied so writeback
 * retries later and the error is stashed via mapping_set_error. */
static int briefs_write_folio_alloc_one(struct inode *inode,
					struct writeback_control *wbc,
					struct folio *folio, bool *any_alloc)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 rel, phys, iblock;
	struct briefs_extent ext;
	int ret;

	iblock = folio_pos(folio) >> BRIEFS_BLOCK_SHIFT;
	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0) {
		/* No free block: fail the folio via __block_write_full_folio's
		 * recover path instead of redirtying it.  Redirtying leaves the
		 * dirty count unchanged and wedges balance_dirty_pages on a full
		 * filesystem (generic/015).  briefs_get_block re-attempts the
		 * alloc and, still finding none, returns -ENOSPC, which makes
		 * __block_write_full_folio clear dirty on the unconverted delayed
		 * buffer, set AS_ENOSPC on the mapping, and end writeback -- so
		 * the dirty page drops and writeback makes progress.  The folio's
		 * unwritten data is discarded; the error is surfaced via fsync. */
		return briefs_block_write_full_folio(folio, wbc, briefs_get_block);
	}
	phys = data_to_abs(bsi->sb, rel);
	ext.offset = iblock;
	ext.phys = phys;
	ext.len = 1;
	ext.flags = 0;
	ret = briefs_append_extent_nojournal(inode->i_sb, &binfo->disk_inode, &ext);
	if (ret == -EEXIST) {
		/*
		 * Lost a race: the block is already mapped.  Free our block and
		 * write the folio through the existing mapping via briefs_get_block
		 * (which re-looks it up).  Falling through to the redirty path would
		 * loop forever, since the next attempt would hit -EEXIST again.
		 */
		briefs_free_block(&bsi->alloc, rel);
		return briefs_block_write_full_folio(folio, wbc, briefs_get_block);
	}
	if (ret) {
		briefs_free_block(&bsi->alloc, rel);
		if (ret == -ENOSPC) {
			/* Extent append failed for lack of a btree chain block:
			 * same full-fs condition as above -- fail the folio so
			 * writeback makes progress instead of redirtying forever. */
			return briefs_block_write_full_folio(folio, wbc,
							     briefs_get_block);
		}
		folio_redirty_for_writepage(wbc, folio);
		folio_unlock(folio);
		mapping_set_error(inode->i_mapping, ret);
		return ret;
	}
	*any_alloc = true;
	return briefs_write_folio_mapped(inode, wbc, folio, phys);
}

/* Flush one accumulated run of `n` consecutive dirty delayed folios starting at
 * logical block blk0.  Try to allocate the whole run contiguously and append a
 * single len=n extent; on success write each folio bound to phys_run+i.  If no
 * contiguous run of n fits, or the extent append fails, fall back to the
 * per-block path for every folio (never regresses on ENOSPC/fragmentation). */
static int briefs_flush_run(struct inode *inode, struct address_space *mapping,
			    struct writeback_control *wbc,
			    struct folio **run, int n, bool *any_alloc)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 blk0 = folio_pos(run[0]) >> BRIEFS_BLOCK_SHIFT;
	u64 rel, phys_run;
	struct briefs_extent ext;
	int ret = 0, i, r;

	rel = briefs_alloc_blocks(&bsi->alloc, (u64)n);
	if (rel != 0) {
		phys_run = data_to_abs(bsi->sb, rel);
		ext.offset = blk0;
		ext.phys = phys_run;
		ext.len = (u64)n;
		ext.flags = 0;
		r = briefs_append_extent_nojournal(inode->i_sb,
						   &binfo->disk_inode, &ext);
		if (r == 0) {
			*any_alloc = true;
			for (i = 0; i < n; i++) {
				r = briefs_write_folio_mapped(inode, wbc,
							      run[i],
							      phys_run + (u64)i);
				if (r) {
					mapping_set_error(mapping, r);
					if (!ret)
						ret = r;
				}
			}
			return ret;
		}
		/* Append failed (e.g. chain block ENOSPC): free the run
		 * per-block and fall through to the per-block path. */
		for (i = 0; i < n; i++)
			briefs_free_block(&bsi->alloc, rel + (u64)i);
	}

	/* Per-block fallback. */
	for (i = 0; i < n; i++) {
		r = briefs_write_folio_alloc_one(inode, wbc, run[i], any_alloc);
		if (r == -ENOSPC) {
			/* Fs is full: briefs_write_folio_alloc_one already
			 * failed run[i] through __block_write_full_folio's
			 * recover.  Fail the remaining folios the same way
			 * (discard + AS_ENOSPC + end writeback) rather than
			 * redirtying, so balance_dirty_pages can make progress. */
			mapping_set_error(mapping, -ENOSPC);
			while (++i < n)
				briefs_block_write_full_folio(run[i], wbc,
							     briefs_get_block);
			return -ENOSPC;
		} else if (r) {
			mapping_set_error(mapping, r);
			if (!ret)
				ret = r;
		}
	}
	return ret;
}

static int briefs_writepages(struct address_space *mapping,
			     struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct folio *folio = NULL;
	struct folio **run;
	int run_n = 0, error = 0, r;
	bool any_alloc = false;
	struct blk_plug plug;

	run = kmalloc_array(BRIEFS_DELALLOC_RUN_MAX, sizeof(*run), GFP_KERNEL);
	if (!run) {
		/* No memory for the run buffer: fall back to the Step 2
		 * per-block writeback path (mpage_writepages). */
		return mpage_writepages(mapping, wbc, briefs_get_block);
	}

	blk_start_plug(&plug);
	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		struct buffer_head *bh = folio_buffers(folio);

		if (folio_order(folio) == 0 && bh && buffer_delay(bh)) {
			/* Run candidate: a delayed single-block folio. */
			if (run_n > 0 &&
			    folio->index == run[run_n - 1]->index + 1 &&
			    run_n < BRIEFS_DELALLOC_RUN_MAX) {
				run[run_n++] = folio;
				error = 0;
				continue;
			}
			/* Gap or run full: flush, then start a new run. */
			if (run_n > 0) {
				error = briefs_flush_run(inode, mapping, wbc,
							 run, run_n, &any_alloc);
				run_n = 0;
			}
			run[run_n++] = folio;
			error = 0;
			continue;
		}

		/* Not a run candidate: flush any open run, then write via the
		 * normal buffer path.  For a mapped !delay buffer
		 * block_write_full_folio skips get_block and just submits; for
		 * a delayed buffer it calls briefs_get_block (one-block alloc). */
		if (run_n > 0) {
			error = briefs_flush_run(inode, mapping, wbc,
						 run, run_n, &any_alloc);
			run_n = 0;
		}
		error = briefs_block_write_full_folio(folio, wbc, briefs_get_block);
		if (error)
			mapping_set_error(mapping, error);
	}
	if (run_n > 0) {
		r = briefs_flush_run(inode, mapping, wbc, run, run_n, &any_alloc);
		if (!error)
			error = r;
	}
	blk_finish_plug(&plug);

	if (any_alloc)
		mark_inode_dirty(inode);

	kfree(run);
	return error;
}
/* bmap: map file block to physical block */
static sector_t briefs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, briefs_get_block);
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
 * ext4 handles this in cont_write_begin/cont_expand_zero, which BrieFS does not
 * use (plain block_write_begin).  We cannot use block_truncate_page here: with
 * delayed allocation the EOF block is typically BH_Delay (not yet allocated), so
 * __block_truncate_page's create=0 get_block returns unmapped and SKIPS the
 * zeroing -- and fsx pollutes via mmap then immediately extends, so the polluted
 * folio is a cached delayed folio at this point (the exact case
 * block_truncate_page cannot touch).  Zero the pagecache folio's tail directly
 * and leave it dirty: writeback (briefs_block_write_full_folio ->
 * briefs_get_block create=1) allocates the block and persists the zeroed folio.
 * Assumes bs == PAGE_SIZE (BrieFS 4K blocks on 4K pages), matching
 * briefs_block_write_full_folio; a cached delayed folio is always uptodate.
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

/* cribbed from xiafs, at least for now.
 *
 * Uses briefs_get_block_write, which DEFERS block allocation: an unmapped
 * block is marked BH_Delay (no allocation, no extent append, no journal record)
 * and is allocated later at writeback by briefs_get_block via mpage /
 * __block_write_full_folio.  This moves the per-block alloc/extent-append/
 * journal off the write() path and onto writeback (#6 delayed allocation).
 *
 * __block_write_begin_int suppresses both the read and the buffer_new partial-
 * tail zeroing for a BH_Delay buffer.  A partial write to a fresh (delayed)
 * block would otherwise leave stale folio bytes in the unwritten head/tail,
 * which writeback would then persist.  Zero those segments here, mirroring the
 * kernel's buffer_new zeroing (fs/buffer.c __block_write_begin_int).  Guarded
 * by !uptodate: if the folio is already uptodate its contents are valid.
 */
static int briefs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata) {
	struct super_block *sb = mapping->host->i_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct folio *folio;
	struct buffer_head *bh, *head;
	size_t from, to, block_start, block_end, blocksize;
	int ret;
	get_block_t *gb;

	/*
	 * Delayed-allocation switch (mirrors ext4's nonda_switch).  With free
	 * space plentiful we defer block allocation to writeback
	 * (briefs_get_block_write -> BH_Delay) for write() throughput.  Once
	 * free space drops below the watermark (a quarter of the data pool,
	 * sized above the per-bdi dirty threshold) we allocate immediately at
	 * write_begin (briefs_get_block) so that ENOSPC is returned
	 * synchronously from write() instead of being discovered in writeback,
	 * where a delayed folio that cannot be allocated would otherwise be
	 * redirtied forever and wedge balance_dirty_pages (generic/015).
	 * Immediate alloc is overwrite-aware: briefs_get_block maps an already
	 * allocated block without touching the free pool, so overwriting an
	 * existing file succeeds even on a full filesystem.  free_count is read
	 * without alloc->lock as a heuristic; a stale read only shifts the
	 * switch point slightly.  briefs_get_block does not journal the extent
	 * (it calls briefs_btree_insert_locked, not _append_extent), so moving
	 * allocation from writeback to write_begin preserves the invariant that
	 * an extent is never journaled ahead of its data.
	 */
	gb = (bsi->alloc.free_count < (bsi->alloc.block_count >> 2))
	     ? briefs_get_block : briefs_get_block_write;

	/* Zero-fill the tail of the block containing the current EOF when this
	 * write jumps past EOF (pos > i_size) and EOF is mid-block.  The tail
	 * [i_size, block_end) is beyond the file's valid data but inside the
	 * EOF block; without zeroing it here, bytes there -- written by an mmap
	 * store beyond EOF (fsx -e EOF pollution) or left over from a prior,
	 * smaller i_size -- persist and leak as valid data once i_size later
	 * grows past the block (generic/363: fsx READ BAD DATA at the old EOF).
	 * Plain block_write_begin does NOT do this zeroing (only cont_write_begin
	 * / cont_expand_zero does, which ext4 uses).  Use briefs_zero_eof_tail,
	 * NOT block_truncate_page: under delalloc the EOF block is typically
	 * BH_Delay (unmapped), so block_truncate_page's create=0 get_block
	 * returns unmapped and skips the zeroing -- exactly the fsx case.
	 */
	if (pos > i_size_read(mapping->host))
		briefs_zero_eof_tail(mapping, i_size_read(mapping->host));

	ret = block_write_begin(mapping, pos, len, foliop, gb);

	/*
	 * Don't free data on error — the VFS / caller will handle cleanup
	 * on the inode.  The old code called briefs_free_inode_data here,
	 * which freed ALL extents even on a partial write, leaving
	 * dangling buffer_heads in the page cache that caused
	 * use-after-free crashes in kswapd and writeback.
	 *
	 * If block_write_begin partially allocated blocks and then
	 * failed, those blocks remain allocated on disk (a minor leak)
	 * until the inode itself is evicted, at which point
	 * briefs_evict_inode will free them properly.
	 */
	if (ret)
		return ret;

	folio = *foliop;
	if (folio_test_uptodate(folio))
		return 0;		/* existing data is valid, no tails to zero */

	from = offset_in_folio(folio, pos);
	to = from + len;

	head = folio_buffers(folio);
	if (!head)
		return 0;
	blocksize = head->b_size;

	bh = head;
	block_start = 0;
	do {
		block_end = block_start + blocksize;
		if (buffer_delay(bh) &&
		    (block_end > to || block_start < from))
			folio_zero_segments(folio, to, block_end,
			                    block_start, from);
		block_start = block_end;
		bh = bh->b_this_page;
	} while (bh != head);

	return 0;
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

	/* Direct I/O is not implemented: BrieFS serves all I/O through the page
	 * cache (the address_space .direct_IO is noop_direct_IO, so an O_DIRECT
	 * open would otherwise succeed while reads/writes silently stay
	 * buffered). Reject O_DIRECT at open with a clear -EINVAL instead. This
	 * makes xfstests' _require_odirect cleanly skip the whole DIO test class
	 * (rather than running those tests against silently-buffered I/O and
	 * producing spurious failures) until real DIO support exists. */
	if (file->f_flags & O_DIRECT)
		return -EINVAL;

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

	return generic_file_read_iter(iocb, to);
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
	return generic_file_write_iter(iocb, from);
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
	 * (briefs_writepages -> briefs_flush_run -> briefs_append_extent_nojournal)
	 * acquires extent_lock to map blocks.  Holding extent_lock across that
	 * wait deadlocks writeback against truncate (AB-BA): the writeback of the
	 * very pages truncate is waiting on blocks on the lock truncate holds.
	 * This is the generic/074 mmap hang (kworker flush wedged in
	 * briefs_append_extent_nojournal, fstest wedged in truncate_pagecache).
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
		 * the gap non-zero).  Every block-based FS (ext2/ext3/minix/fat)
		 * does exactly this block_truncate_page() before truncate_setsize().
		 *
		 * Under delalloc the block at new_size is typically BH_Delay (not
		 * yet allocated), so block_truncate_page's create=0 get_block returns
		 * unmapped and SKIPS the zeroing -- the exact case here (op 471
		 * delalloc WRITE then op 472 TRUNCATE DOWN left the tail pattern,
		 * read back stale after op 473 FALLOC EXTENDING).  briefs_zero_eof_tail
		 * zeroes the cached delayed folio's tail directly first; the
		 * block_truncate_page below then handles the allocated-block case
		 * (read+zero+dirty) and is a no-op for the already-zeroed delayed
		 * folio.  Called before extent_lock: briefs_get_block takes
		 * extent_lock internally and is released before this returns, and
		 * this does not wait on writeback (unlike truncate_setsize's
		 * truncate_pagecache), so there is no AB-BA with the writeback path
		 * described below. */
		briefs_zero_eof_tail(inode->i_mapping, new_size);
		ret = block_truncate_page(inode->i_mapping, new_size,
					  briefs_get_block);
		if (ret)
			return ret;
		truncate_setsize(inode, new_size);
	}

	/* Symmetric zeroing for a GROW (truncate-up): zero+dirty the tail of the
	 * block containing old_size, [old_size, block_end).  That tail is beyond
	 * the old EOF but inside the EOF block, so it may hold bytes written by an
	 * mmap store past EOF (fsx -e EOF pollution) or stale content from a prior
	 * smaller i_size; once truncate_setsize grows i_size past the block,
	 * writeback's [i_size,block_end) zeroing no longer covers it and the stale
	 * tail leaks as valid data (generic/363).  Use briefs_zero_eof_tail, NOT
	 * block_truncate_page: under delalloc the old-EOF block may be BH_Delay
	 * (unmapped), so block_truncate_page's create=0 get_block would skip it.
	 * Inline-data grows manage their bytes directly under extent_lock below,
	 * so they are excluded.  Pre-extent_lock for the same AB-BA reason as the
	 * shrink path (briefs_get_block takes extent_lock internally); this helper
	 * takes no locks and waits on no writeback.  Only the partial old-EOF block
	 * needs this; whole-block gaps in [old_size,new_size) read as zeros. */
	if (new_size > old_size &&
	    !(binfo->disk_inode.flags & InodeFlagInlineData) &&
	    (old_size & (BRIEFS_BLOCK_SIZE - 1)))
		briefs_zero_eof_tail(inode->i_mapping, old_size);

	/*
	 * From this point on we may manipulate the extent list or the chain
	 * blocks that back it.  Hold the per-inode extent lock to serialize
	 * with concurrent appends (briefs_append_extent / briefs_get_block)
	 * and with writeback that maps blocks through the extent list.
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
			ret = briefs_btree_delete_range(inode->i_sb,
							&binfo->disk_inode,
							trunc_block, U64_MAX);
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
 * BrieFS has no hole extents: punch-hole removes extents and frees their
 * blocks, so holes are gaps in logical coverage. We emit only the real
 * allocated extents in ascending logical order; holes are implied by the
 * gaps between them (per the fiemap spec). Preallocated blocks are normal
 * extents (flags == 0). Inline-data inodes report a single DATA_INLINE extent.
 */
int briefs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		  u64 start, u64 len)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_inode *di = &binfo->disk_inode;
	struct super_block *sb = inode->i_sb;
	u64 isize = i_size_read(inode);
	u64 end;
	struct briefs_extent *arr;
	int snap_total, ret, i;

	/* Validate/clip the range; flush dirty pages if FIEMAP_FLAG_SYNC was
	 * requested. supported_flags = 0 means we accept only FIEMAP_FLAG_SYNC
	 * (added by fiemap_prep); FIEMAP_FLAG_XATTR requests get -EBADR since
	 * BrieFS has no xattrs. */
	ret = fiemap_prep(inode, fieinfo, start, &len, 0);
	if (ret)
		return ret;
	end = start + len;   /* len is now clipped by fiemap_prep */

	/* Inline-data inode: a single inline extent covers [0, isize). */
	if (di->flags & InodeFlagInlineData) {
		if (start < isize) {
			ret = fiemap_fill_next_extent(fieinfo, 0, 0, isize,
				FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_LAST);
			/* helper also sets FIEMAP_EXTENT_NOT_ALIGNED for us */
			if (ret < 0)
				return ret;
		}
		return 0;   /* ret == 1 (done) -> return 0; never return 1 */
	}

	/* Delayed allocation (#6): written-but-not-yet-written-back blocks are
	 * BH_Delay in the page cache and absent from the extent list, so without
	 * this flush fiemap would report them as holes.  BrieFS has no
	 * extent-status tree (like ext4_es) to describe delayed extents, so --
	 * unlike ext4, which synthesizes DELALLOC extents -- we force writeback
	 * of the queried range first and then report the now-allocated physical
	 * extents.  This makes fiemap reflect the real post-writeback layout.
	 */
	ret = filemap_write_and_wait_range(inode->i_mapping, start, end - 1);
	if (ret)
		return ret;

	/* Collect every extent in ascending offset order. The B+ tree (and the
	 * inline array) keep extents sorted by offset, so no sort() is needed
	 * — the headline complexity win of the tree conversion. */
	ret = briefs_collect_all_extents(sb, di, &arr, &snap_total);
	if (ret)
		return ret;
	if (snap_total == 0)
		return 0;   /* fully sparse / empty extent-backed file */

	for (i = 0; i < snap_total; i++) {
		u64 logical   = arr[i].offset << BRIEFS_BLOCK_SHIFT;
		u64 ext_bytes = arr[i].len    << BRIEFS_BLOCK_SHIFT;
		u64 ext_end   = logical + ext_bytes;
		u32 flags = 0;

		if (ext_end <= start)
			continue;          /* entirely before the query range */
		if (logical >= end)
			break;             /* sorted -> no more overlap */

		if (i + 1 == snap_total)
			flags |= FIEMAP_EXTENT_LAST;

		if (arr[i].flags & BRIEFS_EXT_UNWRITTEN)
			flags |= FIEMAP_EXTENT_UNWRITTEN;

		ret = fiemap_fill_next_extent(fieinfo, logical,
			arr[i].phys << BRIEFS_BLOCK_SHIFT, ext_bytes, flags);
		if (ret < 0)
			goto out;          /* -EFAULT: propagate */
		if (ret == 1)
			goto done;         /* buffer full or LAST emitted: success */
		/* ret == 0: continue */
	}
done:
	ret = 0;
out:
	kvfree(arr);
	return ret;
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
 * briefs_zero_block_range - zero bytes [start, end) inside the physical
 * block at @abs_block.  Used by punch-hole to zero the portion of a
 * partially-holed block that remains allocated.
 */
static int briefs_zero_block_range(struct super_block *sb, u64 abs_block,
                                   u32 start, u32 end)
{
	struct buffer_head *bh;

	if (start >= end)
		return 0;
	if (end > sb->s_blocksize)
		return -EINVAL;

	bh = sb_bread(sb, abs_block);
	if (!bh)
		return -EIO;

	memset(bh->b_data + start, 0, end - start);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
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
			if (del_start < del_end) {
				ret = briefs_btree_delete_range(inode->i_sb,
								&binfo->disk_inode,
								del_start, del_end);
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
/* address_space_operations for BrieFS regular files.
 * Only read_folio and bmap are used (for mmap/exec).
 * address_space_operations for BrieFS regular files.
 * All writes go through page cache via write_begin/write_end/writepages.
 *
 * Taking some inspiration from xiafs for some of these operations.
 */
const struct address_space_operations briefs_aops = {
	.dirty_folio	= block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio	= briefs_read_folio,
	.writepages	= briefs_writepages,
	.bmap		= briefs_bmap,
	.migrate_folio	= buffer_migrate_folio,
	.is_partially_uptodate = block_is_partially_uptodate,
	.direct_IO  	= noop_direct_IO,
	.write_begin 	= briefs_write_begin,
	.write_end 	= generic_write_end,
};
