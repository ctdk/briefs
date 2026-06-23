/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * briefs_iomap.c - iomap-based file data path for BrieFS.
 *
 * BrieFS's file data mapping is built on the buffer_head get_block stack
 * (briefs_get_block / briefs_get_block_write).  This file re-expresses the
 * same extent index as struct iomap, the buffer_head-free data path shared by
 * buffered read/write, fiemap, fallocate tail-zeroing, direct I/O, swapfile and
 * bmap.
 *
 * Scope: ONLY the per-inode file data mapping is described here.  Metadata
 * (inode table, directory trie pages, btree extent-index nodes, allocator
 * bitmaps, journal) stays on sb_bread/mark_buffer_dirty/sync_dirty_buffer and
 * is unaffected.  The extent lookup machinery -- briefs_inode_lookup_iblock,
 * briefs_btree_insert_locked, briefs_clear_extent_unwritten, briefs_alloc_block
 * -- is reused unchanged; it is called from iomap_begin instead of get_block.
 *
 * Two ops share one begin translation:
 *   briefs_iomap_ops        report / read  -- never allocates, never converts
 *                                            an unwritten extent.
 *   briefs_write_iomap_ops  write          -- allocates on a miss and converts
 *                                            an unwritten extent in place.
 *
 * Both mirror briefs_get_block (briefs_extent.c) exactly: the cached_max_end
 * tail-cache fast path, the unlocked lookup with a locked re-check on a write
 * miss, in-place unwritten conversion, run-coalesced allocation under
 * extent_lock (one len<=256 extent per write_begin, with a single-block
 * fallback), and the -EEXIST race adoption.
 */
#include <linux/iomap.h>
#include <linux/fs.h>
#include <linux/seqlock.h>

#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_iomap.h"

/*
 * Fill *iomap for an extent hit.  The extent may cover more than the single
 * queried block; report the whole extent so the iomap core can coalesce and
 * (for fiemap) merge consecutive same-type mappings.
 */
static void briefs_iomap_fill_mapped(struct inode *inode,
				     const struct briefs_extent *ext,
				     u16 iomap_flags, struct iomap *iomap)
{
	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = ext->offset << BRIEFS_BLOCK_SHIFT;
	iomap->length = ext->len << BRIEFS_BLOCK_SHIFT;
	iomap->addr = ext->phys << BRIEFS_BLOCK_SHIFT;
	iomap->flags = iomap_flags;
	iomap->type = (ext->flags & BRIEFS_EXT_UNWRITTEN) ? IOMAP_UNWRITTEN
							    : IOMAP_MAPPED;
}

/*
 * Report a hole (unmapped region).  BrieFS has no hole extents -- punch-hole
 * removes extents and frees their blocks, so a hole is a gap in logical
 * coverage.  iomap_fiemap skips IOMAP_HOLE (the gap is implied) and iomap reads
 * zero-fill it.
 *
 * The hole length must not run past the next mapped extent or the iomap
 * iterator would skip that extent, and it must not be so small that the
 * iterator crawls one block at a time -- a fiemap with no range queries
 * [0, s_maxbytes), so a one-block hole past the last extent would loop billions
 * of times and soft-lockup.  So a hole is bounded by the next extent above
 * @iblock (via briefs_next_extent, O(log E)), or by the end of the queried
 * range when there is no next extent (the tail / past-EOF case).
 *
 * @tail: true when the caller already knows there is no extent at or above
 * @iblock (the cached_max_end fast path fired, iblock >= cached_max_end), so
 * the hole runs straight to the query end without a tree read.
 */
static int briefs_iomap_fill_hole(struct inode *inode, loff_t pos, loff_t length,
				  u64 iblock, bool tail,
				  struct briefs_inode_info *binfo,
				  struct iomap *iomap)
{
	u64 offset = iblock << BRIEFS_BLOCK_SHIFT;
	u64 query_end = (u64)pos + (u64)length;
	u64 hole_end;
	int ret;

	if (tail) {
		hole_end = query_end;
	} else {
		struct briefs_extent next;

		ret = briefs_next_extent(inode->i_sb, binfo, iblock, &next,
					 false);
		if (ret == 0)
			hole_end = next.offset << BRIEFS_BLOCK_SHIFT;
		else if (ret == -ENOENT)
			hole_end = query_end;		/* no next extent -> to end */
		else
			return ret;			/* -EIO: let the caller retry */
	}

	/* Clamp to the queried range (next extent may lie beyond it). */
	if (hole_end > query_end)
		hole_end = query_end;

	iomap->bdev = inode->i_sb->s_bdev;
	iomap->offset = offset;
	iomap->length = hole_end - offset;
	iomap->addr = IOMAP_NULL_ADDR;
	iomap->flags = 0;
	iomap->type = IOMAP_HOLE;
	return 0;
}

/*
 * Core begin translation, shared by both ops.  @write selects the write
 * behaviour (allocate / convert-unwritten); the report/read ops pass false.
 */
static int briefs_iomap_begin_common(struct inode *inode, loff_t pos,
				     loff_t length, unsigned flags,
				     struct iomap *iomap, bool write)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_inode *di = &binfo->disk_inode;
	u64 iblock = pos >> BRIEFS_BLOCK_SHIFT;
	struct briefs_extent ext, new_ext;
	u64 rel, phys, full_blocks, n_blocks, i;
	unsigned seq;
	u64 snap_max_end;
	int ret;

	/* Inline-data inodes carry their data in the inode block; the iter-level
	 * read/write bypass handles them directly.  The pagecache/fiemap path
	 * sees a single inline mapping covering [0, isize). */
	if (di->flags & InodeFlagInlineData) {
		u64 isize = i_size_read(inode);

		iomap->bdev = inode->i_sb->s_bdev;
		iomap->offset = 0;
		iomap->length = isize;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->flags = 0;
		iomap->type = IOMAP_INLINE;
		iomap->inline_data = di->inline_data;
		return 0;
	}

	/* Tail-cache fast path (verbatim from briefs_get_block): snapshot under
	 * extent_seq; any block at or beyond cached_max_end is definitively
	 * unmapped.  0 means unknown -> fall through to the lookup. */
	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		snap_max_end = binfo->cached_max_end;
	} while (read_seqcount_retry(&binfo->extent_seq, seq));

	if (snap_max_end != 0 && iblock >= snap_max_end) {
		if (!write)
			return briefs_iomap_fill_hole(inode, pos, length, iblock,
						      true, binfo, iomap);
		goto locked_create;
	}

	/* Unlocked extent lookup (verifies CRCs). */
	ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, iblock, &ext, false);
	if (ret == 0) {
		/* A write into an unwritten extent converts it to written
		 * (deferred to the locked path, which re-looks-up under
		 * extent_lock and clears BRIEFS_EXT_UNWRITTEN, exactly as
		 * briefs_get_block does).  Reads/fiemap see it as unwritten. */
		if (write && (ext.flags & BRIEFS_EXT_UNWRITTEN))
			goto locked_create;
		briefs_iomap_fill_mapped(inode, &ext, 0, iomap);
		return 0;
	}
	/* -ENOENT: not mapped.  -EIO: torn/corrupt read -> fall through to the
	 * locked re-check, which re-reads under the lock and recovers if the
	 * tear is gone, or returns the error. */

	if (!write)
		return briefs_iomap_fill_hole(inode, pos, length, iblock,
					      false, binfo, iomap);

locked_create:
	/* Allocate / convert under extent_lock, re-checking first: another
	 * thread may have inserted the block while we did the unlocked lookup
	 * above (without the re-check two threads can both insert an extent for
	 * the same iblock, leaking the first block). */
	mutex_lock(&binfo->extent_lock);

	/* Under extent_lock the tail cache is authoritative (every insert that
	 * can raise cached_max_end runs under this mutex).  If iblock is still
	 * beyond it, skip straight to allocation.  0 = unknown -> lookup. */
	if (binfo->cached_max_end != 0 && iblock >= binfo->cached_max_end)
		goto do_alloc;

	/* Locked re-check: trust_verified (no concurrent modifier can have torn
	 * a buffer we cached under this same lock). */
	ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, iblock, &ext, true);
	if (ret == 0) {
		if (ext.flags & BRIEFS_EXT_UNWRITTEN)
			briefs_clear_extent_unwritten(inode, iblock);
		briefs_iomap_fill_mapped(inode, &ext, 0, iomap);
		mutex_unlock(&binfo->extent_lock);
		return 0;
	}
	if (ret != -ENOENT) {
		mutex_unlock(&binfo->extent_lock);
		return ret;		/* -EIO */
	}

do_alloc:
	/* Size the allocation as a contiguous run, not a single block, so a
	 * large sequential write maps N blocks as one extent instead of N.
	 * The run covers only the FULL blocks remaining in this write
	 * (floor(length/block)); any trailing partial block is mapped by a
	 * separate begin call, so every block in the run is fully written and
	 * iomap's IOMAP_F_NEW head/tail zeroing never leaves a stale tail
	 * inside it.  Cap the run at BRIEFS_DELALLOC_RUN_MAX and bound it by
	 * the next existing extent so it never overlaps a later mapping:
	 * iblock itself is unmapped (the re-check above), but iblock+k may
	 * already be mapped.  Writeback calls here with len == 1 block, so
	 * full_blocks == 1 and this collapses to the single-block path.
	 */
	full_blocks = (u64)length >> BRIEFS_BLOCK_SHIFT;
	n_blocks = full_blocks ? min_t(u64, full_blocks,
				       BRIEFS_DELALLOC_RUN_MAX) : 1;
	if (n_blocks > 1) {
		ret = briefs_next_extent(inode->i_sb, binfo, iblock, &ext,
					 true);
		if (ret == 0 && ext.offset > iblock)
			n_blocks = min_t(u64, n_blocks, ext.offset - iblock);
		/* -ENOENT: no later extent, no bound.  -EIO: ignore; the insert
		 * below will fail and drop to the single-block path.
		 */
	}

	rel = briefs_alloc_blocks(&bsi->alloc, n_blocks);
	if (rel != 0) {
		phys = data_to_abs(bsi->sb, rel);
		new_ext.offset = iblock;
		new_ext.phys = phys;
		new_ext.len = n_blocks;
		new_ext.flags = 0;

		ret = briefs_btree_insert_locked(inode->i_sb,
						 &binfo->disk_inode, &new_ext);
		if (ret == 0) {
			mark_inode_dirty(inode);
			briefs_iomap_fill_mapped(inode, &new_ext,
						 IOMAP_F_NEW, iomap);
			mutex_unlock(&binfo->extent_lock);
			return 0;
		}
		/* Run insert failed (tree-grow ENOSPC, or a race -EEXIST the
		 * bound above should have prevented): free the run per-block
		 * (briefs_free_blocks_range 1M-caps; a 256-block run sits at
		 * the cap) and drop to the single-block path.
		 */
		for (i = 0; i < n_blocks; i++)
			briefs_free_block(&bsi->alloc, rel + i);
	}

	/* Single-block fallback: briefs_alloc_blocks could not obtain a
	 * contiguous physical run of n_blocks (fragmentation), or the run
	 * insert failed.  Allocate one block and insert a len=1 extent.
	 */
	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0) {
		mutex_unlock(&binfo->extent_lock);
		return -ENOSPC;
	}
	phys = data_to_abs(bsi->sb, rel);

	new_ext.offset = iblock;
	new_ext.phys = phys;
	new_ext.len = 1;
	new_ext.flags = 0;

	ret = briefs_btree_insert_locked(inode->i_sb, &binfo->disk_inode,
					 &new_ext);
	if (ret == -EEXIST) {
		/* Lost the race: free our block and adopt the existing mapping.
		 * The locked re-check above makes this unreachable in practice;
		 * handle it anyway. */
		briefs_free_block(&bsi->alloc, rel);
		ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, iblock,
						 &ext, true);
		if (ret == 0) {
			briefs_iomap_fill_mapped(inode, &ext, 0, iomap);
			mutex_unlock(&binfo->extent_lock);
			return 0;
		}
		mutex_unlock(&binfo->extent_lock);
		return ret;
	}
	if (ret != 0) {
		briefs_free_block(&bsi->alloc, rel);
		mutex_unlock(&binfo->extent_lock);
		return ret;
	}

	/* The extent list changed; make the VFS write back the inode so the
	 * on-disk inode references this block (else it is allocated but
	 * unreachable after a sync). */
	mark_inode_dirty(inode);

	briefs_iomap_fill_mapped(inode, &new_ext, IOMAP_F_NEW, iomap);
	mutex_unlock(&binfo->extent_lock);
	return 0;
}

static int briefs_iomap_begin_ro(struct inode *inode, loff_t pos, loff_t length,
				 unsigned flags, struct iomap *iomap,
				 struct iomap *srcmap)
{
	return briefs_iomap_begin_common(inode, pos, length, flags, iomap, false);
}

static int briefs_iomap_begin_write(struct inode *inode, loff_t pos,
				    loff_t length, unsigned flags,
				    struct iomap *iomap,
				    struct iomap *srcmap)
{
	return briefs_iomap_begin_common(inode, pos, length, flags, iomap, true);
}

/*
 * briefs_write_iomap_end -- commit a write that allocated a run (IOMAP_F_NEW).
 * The run is sized to the FULL blocks remaining in the write, so in the normal
 * case every block in the run is fully written (the trailing partial block, if
 * any, is mapped by a separate begin call) and there is nothing to unreserve.
 * mark_inode_dirty was done in begin.  The only short write is an EFAULT /
 * balance_dirty_pages error mid-run, which leaves the unwritten run tail
 * allocated but past i_size; it is invisible to ordinary reads and reclaimed by
 * a later truncate.  Freeing that tail here (a sub-range extent delete) is
 * correct but not yet wired -- add it if a stale-tail regression appears.
 */
static int briefs_write_iomap_end(struct inode *inode, loff_t pos, loff_t length,
				  ssize_t written, unsigned flags,
				  struct iomap *iomap)
{
	if (written <= 0)
		return written;
	return 0;
}

const struct iomap_ops briefs_iomap_ops = {
	.iomap_begin	= briefs_iomap_begin_ro,
};

const struct iomap_ops briefs_write_iomap_ops = {
	.iomap_begin	= briefs_iomap_begin_write,
	.iomap_end	= briefs_write_iomap_end,
};

/*
 * Writeback mapping.  This mirrors briefs_get_block(create=1) from the old
 * buffer_head writeback path: allocate a block on a miss and convert an
 * unwritten extent to written in place (write=true).
 *
 * Most dirty folios were allocated at write_begin time
 * (briefs_write_iomap_ops.begin) or at mmap-fault time (briefs_vm_page_mkwrite
 * -> iomap_page_mkwrite, briefs_file.c) and arrive here already mapped, so
 * writeback just re-maps them and submits a bio -- the write=true lookup hits
 * and takes no locks beyond the seqcount read.  write=true (allocate on a miss,
 * convert unwritten in place) is kept as the safety net for any dirty hole that
 * reaches writeback without having gone through write_begin or page_mkwrite:
 * with write=false the iomap core would skip an IOMAP_HOLE (case IOMAP_HOLE:
 * break) and end writeback without writing, but the folio would still be
 * mapped-dirty and re-queued forever, busy-looping the writeback worker.  On a
 * miss write=true allocates the block here, exactly as
 * briefs_get_block(create=1) did, and iomap_add_to_ioend writes the (already
 * zero-filled, uptodate) folio to it.
 *
 * The cached wpc->iomap is reused while it still covers the requested offset,
 * so one begin call serves every folio inside a single extent (matching the
 * coalescing the old run-allocating writeback got for free).
 */
static int briefs_writeback_map_blocks(struct iomap_writepage_ctx *wpc,
				       struct inode *inode, loff_t offset,
				       unsigned int len)
{
	if (offset >= wpc->iomap.offset &&
	    offset < wpc->iomap.offset + wpc->iomap.length)
		return 0;

	return briefs_iomap_begin_common(inode, offset, len, IOMAP_WRITE,
					  &wpc->iomap, true);
}

const struct iomap_writeback_ops briefs_writeback_ops = {
	.map_blocks	= briefs_writeback_map_blocks,
};

/*
 * briefs_dio_write_end_io - complete a direct-I/O write.
 *
 * iomap_dio_rw advances iocb->ki_pos in iomap_dio_complete AFTER end_io
 * returns, so ki_pos here is still the ORIGINAL write offset and
 * ki_pos + size is the new EOF when the write extended the file.  Update
 * i_size and mark the inode dirty so briefs_write_inode persists the new
 * disk_inode.filesize (briefs_inode.c briefs_write_inode copies inode->i_size
 * to binfo->disk_inode.filesize).  mark_inode_dirty is needed even when begin
 * hit an already-allocated extent (no allocation -> no mark_inode_dirty there):
 * without it a DIO write that only extends EOF into a pre-existing block would
 * not have its i_size persisted.
 *
 * BrieFS allocates blocks WRITTEN at begin time and iomap_dio zeroes the
 * head/tail of IOMAP_F_NEW blocks, so there are no unwritten extents to convert
 * here and IOMAP_DIO_UNWRITTEN is never raised.  On error, propagate it so the
 * caller (and generic_write_sync for O_SYNC) sees it.
 */
static int briefs_dio_write_end_io(struct kiocb *iocb, ssize_t size,
				   int error, unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (error)
		return error;
	if (size > 0 && iocb->ki_pos + size > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos + size);
		mark_inode_dirty(inode);
	}
	return 0;
}

const struct iomap_dio_ops briefs_dio_write_ops = {
	.end_io		= briefs_dio_write_end_io,
};
