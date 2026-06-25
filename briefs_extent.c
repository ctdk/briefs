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
	u64 rel_start = abs_to_data(bsi->sb, phys_start);

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
 * briefs_clear_extent_unwritten - convert the unwritten extent covering
 * @iblock to written (clear BRIEFS_EXT_UNWRITTEN, in place -- no split, no
 * block free).  Called by the iomap write path (briefs_iomap_begin) when a
 * write targets a previously-fallocated (unwritten) extent, so a subsequent
 * fiemap reports it as written data instead of unwritten.  Caller MUST hold
 * binfo->extent_lock.  Returns 0 if an
 * extent covered @iblock (converted or already written), -ENOENT if none.
 *
 * For inline-only inodes the flag lives in binfo->disk_inode.inline_extents[],
 * mutated under extent_seq (mirroring briefs_btree_insert_locked) and
 * persisted by briefs_write_inode's INODE_FULL snapshot.  For tree-backed
 * inodes the leaf record is updated on disk (briefs_btree_clear_unwritten).
 */
int briefs_clear_extent_unwritten(struct inode *inode, u64 iblock)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_inode *di = &binfo->disk_inode;

	if (di->flags & InodeFlagIndexed)
		return briefs_btree_clear_unwritten(inode->i_sb, di, iblock);

	{
		int k, n = di->num_extents_inline;

		if (n > 8)
			n = 8;
		for (k = 0; k < n; k++) {
			struct briefs_extent *e = &di->inline_extents[k];

			if (iblock >= e->offset && iblock < e->offset + e->len) {
				if (e->flags & BRIEFS_EXT_UNWRITTEN) {
					write_seqcount_begin(&binfo->extent_seq);
					e->flags &= ~BRIEFS_EXT_UNWRITTEN;
					write_seqcount_end(&binfo->extent_seq);
					mark_inode_dirty(inode);
				}
				return 0;
			}
		}
		return -ENOENT;
	}
}

/*
 * briefs_append_extent - insert an extent into the index.  Takes the per-inode
 * extent lock and calls the locked tree/inline mutator.  This is the entry
 * point for callers that are not already holding the lock.  Returns 0 or
 * -errno; on success logs a full inode snapshot so replay restores the extent
 * metadata exactly.
 */
int briefs_append_extent(struct super_block *sb, struct briefs_inode *di,
                         struct briefs_extent *ext)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_inode_info *binfo;
	struct briefs_disk_inode disk_di;
	int ret;

	binfo = container_of(di, struct briefs_inode_info, disk_inode);
	mutex_lock(&binfo->extent_lock);
	ret = briefs_btree_insert_locked(sb, di, ext);
	mutex_unlock(&binfo->extent_lock);
	if (ret)
		return ret;

	/* Log a full snapshot so replay restores extent metadata exactly. */
	briefs_cpu_inode_to_disk(di, &disk_di);
	briefs_journal_inode_full(bsi->journal, di->inode_number, &disk_di);
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

/* briefs_sum_len_cb - for_each callback accumulating extent length (in blocks)
 * into the u64 @ctx points at. */
static int briefs_sum_len_cb(const struct briefs_extent *ext, void *ctx)
{
	u64 *blocks = ctx;
	*blocks += ext->len;
	return 0;
}

/*
 * briefs_compute_i_blocks - compute number of 512-byte sectors used by the
 * data blocks described by an inode's extents. Sums each extent's length in a
 * single in-order walk of the B+ tree (or the inline array for inline-only
 * inodes): O(E), no per-extent chain re-walk.
 */
inline u64 briefs_compute_i_blocks(struct super_block *sb, struct briefs_inode *di)
{
	struct { u64 blocks; } acc = { .blocks = 0 };

	briefs_btree_for_each_extent(sb, di, briefs_sum_len_cb, &acc);
	return acc.blocks * (BRIEFS_BLOCK_SIZE / 512);
}



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
		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_persist_disk_inode(inode->i_sb, inode->i_ino, &binfo->disk_inode, false);
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

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
		write_seqcount_begin(&binfo->extent_seq);
		binfo->disk_inode.flags &= ~InodeFlagInlineData;
		memset(binfo->disk_inode.inline_data, 0,
		       sizeof(binfo->disk_inode.inline_data));
		binfo->disk_inode.filesize = 0;
		write_seqcount_end(&binfo->extent_seq);

		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
					&binfo->disk_inode, false);
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
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

	write_seqcount_begin(&binfo->extent_seq);
	binfo->disk_inode.flags &= ~InodeFlagIndexed;
	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
	binfo->disk_inode.extent_inline_base = 0;
	memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
	/* All extents freed -> invalidate the tail cache (0 = unknown). */
	binfo->cached_max_end = 0;
	write_seqcount_end(&binfo->extent_seq);
	mutex_unlock(&binfo->extent_lock);

	/* Log the cleared inode so replay does not resurrect old extent pointers. */
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);
}
