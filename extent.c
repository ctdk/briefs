/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs extent and block management */

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
#include "briefs_xattr.h"

/*
 * Data region start: the allocator trie uses data-relative block numbers
 * with block 0 being the first data block on disk.  Convert to absolute
 * block numbers for sb_bread/sb_getblk using this helper.
 */
/*
 * Free a contiguous run of data blocks.  Delegates to briefs_free_blocks, which
 * sets the bits and propagates the summary levels under a single alloc->lock
 * acquisition (the per-block loop took the lock once per block).  The run is
 * clamped to the device end inside briefs_free_blocks, so a corrupt extent
 * length can't free past block_count; there is no loop here to bound, hence no
 * 1M-cap-to-1 guard (that guard only existed to stop the old per-block loop).
 */
void briefs_free_blocks_range(struct briefs_sb_info *bsi, u64 phys_start, u64 len)
{
	u64 rel_start;

	/*
	 * Don't free blocks if filesystem is shut down or being torn down.
	 * Prevents NULL pointer dereference during shutdown/umount (generic/052).
	 */
	if (bsi->mount_flags & BRIEFS_MF_SHUTDOWN)
		return;
	if (!bsi->sb)
		return;

	rel_start = abs_to_data(bsi->sb, phys_start);

	briefs_free_blocks(&bsi->alloc, rel_start, len);
}

/*
 * briefs_read_extent - read an INLINE extent by logical index.
 *
 * After the B+ tree conversion this only serves the symlink paths (journal
 * replay of symlink data, and briefs_get_link), which always read index 0 of
 * a single inline extent (symlinks are never tree-backed). Tree-backed and
 * chain-backed extent access goes through briefs_btree_lookup /
 * briefs_btree_for_each_extent instead. Any idx >= num_extents_inline returns
 * -ENOENT.
 */
int briefs_read_extent(struct super_block *sb, struct briefs_inode *di,
                               int idx, struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo;
	unsigned seq;

	if (idx < 0 || idx >= di->num_extents_inline)
		return -ENOENT;

	binfo = container_of(di, struct briefs_inode_info, disk_inode);

	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		*ext = di->inline_extents[idx];
	} while (read_seqcount_retry(&binfo->extent_seq, seq));
	return 0;
}
/*
 * briefs_inode_lookup_iblock - find the extent covering logical block @iblock.
 *
 * Dispatches on InodeFlagIndexed: tree-backed inodes descend the B+ tree from a
 * freshly-snapped root (O(log E)); inline-only inodes snapshot the inline array
 * under extent_seq and scan it (<=8 entries). @trust_verified is forwarded to
 * the tree read: a caller holding extent_lock may skip the CRC on cached,
 * already-verified buffers (no concurrent modifier can have torn them); an
 * unlocked scanner passes false to verify each read (a torn read surfaces as
 * -EIO, which the get_block locked re-check recovers from).
 *
 * Returns 0 and fills *ext, -ENOENT if no extent covers @iblock, -EIO on a
 * read/checksum failure.
 */
int briefs_inode_lookup_iblock(struct super_block *sb,
			       struct briefs_inode_info *binfo,
			       u64 iblock, struct briefs_extent *ext,
			       bool trust_verified)
{
	struct briefs_inode *di = &binfo->disk_inode;

	if (di->flags & InodeFlagIndexed) {
		u64 base;
		unsigned seq;

		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			base = di->extent_inline_base;
		} while (read_seqcount_retry(&binfo->extent_seq, seq));
		if (base == 0)
			return -ENOENT;
		return briefs_btree_lookup(sb, base, iblock, ext, trust_verified);
	}

	{
		struct briefs_extent snap[8];
		unsigned seq;
		int n, k;

		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			n = di->num_extents_inline;
			if (n > 8)
				n = 8;
			for (k = 0; k < n; k++)
				snap[k] = di->inline_extents[k];
		} while (read_seqcount_retry(&binfo->extent_seq, seq));

		for (k = 0; k < n; k++) {
			if (iblock >= snap[k].offset &&
			    iblock < snap[k].offset + snap[k].len) {
				*ext = snap[k];
				return 0;
			}
		}
		return -ENOENT;
	}
}

/*
 * briefs_next_extent - lower-bound: the first extent with offset > @iblock (the
 * extent that bounds a hole at @iblock on the right). Used by the iomap path to
 * bound a hole mapping so the iomap iterator advances past the whole hole in one
 * step instead of one block at a time (which on a query to s_maxbytes would soft-
 * lockup). Dispatches on InodeFlagIndexed: tree-backed inodes use
 * briefs_btree_next_extent (O(log E)); inline-only inodes scan the <=8-entry
 * inline array. Returns 0 + *ext, -ENOENT if no extent has offset > @iblock (the
 * hole runs to EOF / the query end), or -EIO. @trust_verified is forwarded like
 * briefs_inode_lookup_iblock.
 */
int briefs_next_extent(struct super_block *sb, struct briefs_inode_info *binfo,
		       u64 iblock, struct briefs_extent *ext, bool trust_verified)
{
	struct briefs_inode *di = &binfo->disk_inode;

	if (di->flags & InodeFlagIndexed) {
		u64 base;
		unsigned seq;

		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			base = di->extent_inline_base;
		} while (read_seqcount_retry(&binfo->extent_seq, seq));
		return briefs_btree_next_extent(sb, base, iblock, ext,
						trust_verified);
	}

	{
		struct briefs_extent snap[8];
		unsigned seq;
		int n, k;
		u64 best_off = U64_MAX;
		int best = -1;

		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			n = di->num_extents_inline;
			if (n > 8)
				n = 8;
			for (k = 0; k < n; k++)
				snap[k] = di->inline_extents[k];
		} while (read_seqcount_retry(&binfo->extent_seq, seq));

		for (k = 0; k < n; k++) {
			u64 off = snap[k].offset;

			if (off > iblock && off < best_off) {
				best_off = off;
				best = k;
			}
		}
		if (best < 0)
			return -ENOENT;
		*ext = snap[best];
		return 0;
	}
}

/*
 * briefs_convert_unwritten_range - convert the unwritten blocks in
 * [start_blk, end_blk) of the extent covering @start_blk to written, splitting
 * the extent so any un-written prefix/suffix stays unwritten (and reads back as
 * zeros via IOMAP_UNWRITTEN).  Called by the iomap write path when a write
 * targets a previously-fallocated (unwritten) extent, so the written blocks
 * report as written data and the un-written wings keep returning zeros.
 *
 * BrieFS does NOT zero the data blocks of an unwritten extent at fallocate
 * time (that would be millions of synchronous writes -- it wedged generic/103).
 * Correctness therefore depends on the un-written blocks remaining reachable
 * only as IOMAP_UNWRITTEN: a partial write must NOT flip the whole extent to
 * written, or the un-zeroed blocks would read back as stale on-disk data
 * (generic/363/521/522/616).  This function splits the extent instead of
 * clearing the flag in place: the written sub-range becomes a written extent,
 * and each unwritten wing is re-inserted as its own unwritten extent.
 *
 * If the write covers the whole extent, the flag is cleared in place (no split,
 * no stale data, since every block is written).  Caller MUST hold
 * binfo->extent_lock.  Returns 0 if an extent covered @start_blk (converted or
 * already written), -ENOENT if none.
 *
 * For inline-only inodes the extents live in binfo->disk_inode.inline_extents[],
 * mutated under extent_seq and persisted by briefs_write_inode's INODE_FULL
 * snapshot (mark_inode_dirty schedules it).  For tree-backed inodes the leaf
 * record is shrunk on disk and the wings re-inserted
 * (briefs_btree_convert_unwritten_range).
 */
int briefs_convert_unwritten_range(struct inode *inode, u64 start_blk,
				   u64 end_blk)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_inode *di = &binfo->disk_inode;

	if (di->flags & InodeFlagIndexed)
		return briefs_btree_convert_unwritten_range(inode->i_sb, di,
							    start_blk, end_blk);

	{
		int k, n = di->num_extents_inline;

		if (n > 8)
			n = 8;
		for (k = 0; k < n; k++) {
			struct briefs_extent *e = &di->inline_extents[k];
			u64 off, phys, eend, cstart, cend;
			u32 eflags;

			if (!(start_blk >= e->offset &&
			      start_blk < e->offset + e->len)) {
				if (e->offset > start_blk)
					break;
				continue;
			}

			if (!(e->flags & BRIEFS_EXT_UNWRITTEN))
				return 0;		/* already written */

			off = e->offset;
			phys = e->phys;
			eend = e->offset + e->len;
			eflags = e->flags;

			cstart = start_blk;
			if (cstart < off)
				cstart = off;
			cend = end_blk;
			if (cend > eend)
				cend = eend;

			/*
			 * @converted is the count of blocks flipped unwritten->written
			 * (the written middle [cstart,cend); the wings stay unwritten).
			 * Release that many from the inode's metadata reservation: the
			 * unwritten region just shrank, so its worst-case metadata
			 * reserve shrinks too and the surplus shield returns to the data
			 * pool.  Skipped on the already-written path (converted == 0).
			 */
			if (cstart == off && cend == eend) {
				/* Whole extent written: clear the flag in place. */
				briefs_extent_write_begin(binfo);
				e->flags &= ~BRIEFS_EXT_UNWRITTEN;
				briefs_extent_write_end(binfo);
				mark_inode_dirty(inode);
				briefs_release_unwritten_reserve(inode, eend - off);
				return 0;
			}

			/* Shrink this slot to the written middle.  The wings are
			 * re-inserted as unwritten extents below; the insert
			 * path merges them with same-state neighbors and spills
			 * to the btree if the 8-slot inline array overflows.
			 */
			briefs_extent_write_begin(binfo);
			e->offset = cstart;
			e->phys = phys + (cstart - off);
			e->len = cend - cstart;
			e->flags = eflags & ~BRIEFS_EXT_UNWRITTEN;
			briefs_extent_write_end(binfo);
			mark_inode_dirty(inode);

			if (cstart > off) {
				struct briefs_extent prefix = {
					.offset = off,
					.phys = phys,
					.len = cstart - off,
					.flags = eflags,
				};
				int ret = briefs_btree_insert_locked(inode->i_sb,
								     di, &prefix);
				if (ret)
					return ret;
			}
			if (cend < eend) {
				struct briefs_extent suffix = {
					.offset = cend,
					.phys = phys + (cend - off),
					.len = eend - cend,
					.flags = eflags,
				};
				int ret = briefs_btree_insert_locked(inode->i_sb,
								     di, &suffix);
				if (ret)
					return ret;
			}
			briefs_release_unwritten_reserve(inode, cend - cstart);
			return 0;
		}
		return -ENOENT;
	}
}

/*
 * briefs_append_extent - insert an extent into the index and journal it.  This
 * is the entry point for callers that are not already holding the extent lock.
 * Returns 0 or -errno; on success logs a full inode snapshot so replay
 * restores the extent metadata exactly.  The lock/insert/unlock is shared with
 * briefs_append_extent_nojournal (which callers handling a batch of inserts use
 * to avoid one journal record per extent).
 */
int briefs_append_extent(struct super_block *sb, struct briefs_inode *di,
                         struct briefs_extent *ext)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_disk_inode disk_di;
	int ret;

	ret = briefs_append_extent_nojournal(sb, di, ext);
	if (ret)
		return ret;

	/* Log a full snapshot so replay restores extent metadata exactly. */
	briefs_cpu_inode_to_disk(di, &disk_di);
	briefs_journal_inode_full(bsi->journal, &binfo->vfs_inode, &disk_di);
	return 0;
}

/*
 * briefs_append_extent_nojournal - insert an extent without immediately
 * journaling.  The caller is responsible for logging a full inode snapshot
 * after a batch of inserts (e.g. briefs_fallocate).  Otherwise identical to
 * briefs_append_extent.
 */
int briefs_append_extent_nojournal(struct super_block *sb, struct briefs_inode *di,
                                    struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo;
	int ret;

	binfo = container_of(di, struct briefs_inode_info, disk_inode);
	mutex_lock(&binfo->extent_lock);
	ret = briefs_btree_insert_locked(sb, di, ext);
	mutex_unlock(&binfo->extent_lock);
	return ret;
}

/* briefs_sum_len_cb and briefs_compute_i_blocks now live in briefs.h as
 * static inlines (after the briefs_btree_for_each_extent declaration) so the
 * per-write/truncate i_blocks recompute can be inlined at the call sites. */



/* briefs_free_inode_data - free all data blocks owned by an inode */
void briefs_free_inode_data(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_disk_inode disk_di;

	/*
	 * Free the xattr block (if any) before extents/trie -- clear the
	 * pointer first so replay never sees an inode pointing at a freed
	 * block, same invariant as the dir-trie free below.  No-op (and
	 * lock-free) when the inode has no xattrs.
	 */
	briefs_xattr_free(inode);

	/* For directories, free the trie instead of file extents */
	if (S_ISDIR(inode->i_mode)) {
		u64 old_root;

		mutex_lock(&binfo->trie_lock);

		old_root = binfo->disk_inode.dir_trie_root;

		/*
		 * Zero dir_trie_root and persist+log the cleared inode before freeing
		 * any trie pages.  This guarantees replay never sees an inode pointing
		 * at blocks that are about to be marked free.
		 */
		binfo->disk_inode.dir_trie_root = 0;
		briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
				&binfo->disk_inode);

		/*
		 * Free the trie pages using the saved root.  briefs_trie_free_all
		 * expects the root in disk_inode.dir_trie_root, so restore it
		 * temporarily and clear it again afterward.
		 */
		if (!TRIE_REF_IS_NULL(old_root)) {
			binfo->disk_inode.dir_trie_root = old_root;
			briefs_trie_free_all(inode->i_sb, &binfo->disk_inode);
			binfo->disk_inode.dir_trie_root = 0;
		}

		mutex_unlock(&binfo->trie_lock);
		return;
	}

	/* Inline-data files have no allocated data blocks to free. */
	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		briefs_extent_write_begin(binfo);
		binfo->disk_inode.flags &= ~InodeFlagInlineData;
		memset(binfo->disk_inode.inline_data, 0,
		       sizeof(binfo->disk_inode.inline_data));
		binfo->disk_inode.filesize = 0;
		briefs_extent_write_end(binfo);

		briefs_persist_and_journal_inode_warn(inode->i_sb, inode,
				&binfo->disk_inode);
		return;
	}

	/*
	 * Free every data block and every tree node block the inode owns.
	 * briefs_btree_free_all dispatches on InodeFlagIndexed (inline-only walks
	 * the inline array; tree-backed frees data extents at the leaves and all
	 * node blocks recursively) and journals each free. Both call sites (evict
	 * with nlink==0, create-abort) run after the page cache is drained, so no
	 * concurrent get_block can race the free; take extent_lock anyway to honor
	 * the tree mutators' lock contract.
	 */
	mutex_lock(&binfo->extent_lock);
	briefs_btree_free_all(inode->i_sb, &binfo->disk_inode);

	/*
	 * Drop any outstanding unwritten-metadata reservation: the inode is
	 * losing all its extents (evict / create-abort), so its unwritten blocks
	 * are gone and the shielded count must return to the global pool.  Takes
	 * alloc->lock inside extent_lock (the established order).  No-op for a
	 * normal file with no reservation.
	 */
	briefs_drop_unwritten_reserve(inode);

	briefs_extent_write_begin(binfo);
	binfo->disk_inode.flags &= ~InodeFlagIndexed;
	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
	binfo->disk_inode.extent_inline_base = 0;
	memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
	/* All extents freed -> invalidate the tail cache (0 = unknown). */
	binfo->cached_max_end = 0;
	briefs_extent_write_end(binfo);
	mutex_unlock(&binfo->extent_lock);

	/* Log the cleared inode so replay does not resurrect old extent pointers. */
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_journal_inode_full(bsi->journal, inode, &disk_di);
}
