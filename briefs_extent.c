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



/*
 * briefs_get_block - get_block callback for block-based page cache helpers.
 *
 * Maps an inode block number (iblock) to a physical block on disk.
 * If create is set and the block is a hole, allocates a new block.
 *
 * Extent list synchronization:
 *
 * Currently uses a seqcount (binfo->extent_seq) to snapshot the extent
 * metadata before iterating.  This ensures num_extents_total,
 * num_extents_inline, and extent_inline_base are read atomically with
 * respect to briefs_append_extent's writers.
 *
 * The chain blocks are append-only for the lifetime of the inode (freed
 * only during briefs_free_inode_data / eviction), so walking from a
 * snapped chain base is safe even without holding the seqcount open
 * during I/O.  A concurrent append may add extents after our snapshot;
 * we will simply miss them and fall through to the create path below.
 * This wastes at most one block and one extent slot per race window.
 *
 * Future work: switch to a mutex or spinlock for strict atomicity of the
 * read-then-append sequence (eliminating the wasted-block race above).
 * See the discussion in commit message or design notes for trade-offs.
 *
 *   mutex: simpler reasoning, blocks instead of retries, but serializes
 *          all extent access.  OK since briefs_get_block is process
 *          context.
 *   spinlock: even lighter, but expensive for the chain block I/O path.
 */
int briefs_get_block(struct inode *inode, sector_t iblock,
                     struct buffer_head *bh_result, int create)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;
	u64 phys;
	int ret;
	unsigned seq;
	u64 snap_max_end;

	/*
	 * Inline-data inodes are served directly from the inode, not through
	 * the block mapping path.  They must be promoted to extent-backed in
	 * briefs_write_iter before generic_file_write_iter is called.
	 */
	if (binfo->disk_inode.flags & InodeFlagInlineData)
		return create ? -EIO : 0;

	/* Snapshot only the tail cache; the lookup helper snapshots the root
	 * pointer/inline array itself under extent_seq. */
	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		snap_max_end = binfo->cached_max_end;
	} while (read_seqcount_retry(&binfo->extent_seq, seq));

	/*
	 * Extent-tail fast path: cached_max_end is the running max of
	 * (offset+len) over all extents, so any block at or beyond it is
	 * definitively unmapped.  0 means "unknown/empty" -> never use the fast
	 * path (fall through to the lookup, which is the ground truth).  This turns
	 * the append/EOF and read-beyond-EOF cases from O(E) to O(1) per block.
	 */
	if (snap_max_end != 0 && (u64)iblock >= snap_max_end) {
		if (!create)
			return 0;		/* definitively unmapped -> sparse/zero */
		goto locked_create;	/* skip the unlocked O(log E) lookup */
	}

	/* Look up iblock in the snapped extent view (unlocked: verify CRCs). */
	ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, (u64)iblock, &ext, false);
	if (ret == 0) {
		phys = ext.phys + ((u64)iblock - ext.offset);
		map_bh(bh_result, inode->i_sb, phys);
		return 0;
	}
	/* -ENOENT: not mapped.  -EIO: torn/corrupt read -> fall through to the
	 * locked re-check, which re-reads under the lock and recovers if the
	 * tear is gone, or returns the error. */

	/* Not mapped */
	if (!create)
		return 0;

	/*
	 * Allocate and insert a new single-block extent.  Take the extent lock
	 * and re-check the mapping: another thread may have inserted the block
	 * while we were doing the unlocked lookup above.  Without the re-check,
	 * two threads can both insert an extent for the same iblock, leaving the
	 * first allocated block unreachable (leaked).
	 */
locked_create:
	mutex_lock(&binfo->extent_lock);

	/*
	 * Under extent_lock the tail cache is authoritative: every insert that
	 * can raise cached_max_end runs under this mutex (and updates the field
	 * before releasing it), so any concurrent inserter's contribution is
	 * already visible to us here.  If iblock is still beyond the cached max,
	 * the block is definitely not yet mapped and the lookup below cannot
	 * find it -> skip straight to allocation.  0 = unknown -> lookup.
	 */
	if (binfo->cached_max_end != 0 && (u64)iblock >= binfo->cached_max_end)
		goto do_alloc;

	/* Locked re-check: trust BH_Verified (no concurrent modifier can have
	 * torn a buffer we cached under this same lock). */
	ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, (u64)iblock, &ext, true);
	if (ret == 0) {
		phys = ext.phys + ((u64)iblock - ext.offset);
		map_bh(bh_result, inode->i_sb, phys);
		mutex_unlock(&binfo->extent_lock);
		return 0;
	}
	if (ret != -ENOENT) {
		mutex_unlock(&binfo->extent_lock);
		return ret;		/* -EIO */
	}

do_alloc:
	{
		struct briefs_extent new_ext;
		u64 rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0) {
			mutex_unlock(&binfo->extent_lock);
			return -ENOSPC;
		}

		phys = data_to_abs(bsi->sb, rel);

		new_ext.offset = (u64)iblock;
		new_ext.phys = phys;
		new_ext.len = 1;
		new_ext.flags = 0;

		ret = briefs_btree_insert_locked(inode->i_sb, &binfo->disk_inode,
						  &new_ext);
		if (ret != 0) {
			briefs_free_block(&bsi->alloc, rel);
			mutex_unlock(&binfo->extent_lock);
			return ret;
		}

		/*
		 * The extent list changed; make sure the VFS writes back the
		 * inode so the on-disk inode references this block.  Otherwise
		 * the block is allocated but unreferenced after a sync.
		 */
		mark_inode_dirty(inode);

		map_bh(bh_result, inode->i_sb, phys);
		set_buffer_new(bh_result);
		mutex_unlock(&binfo->extent_lock);
		return 0;
	}
}

/*
 * briefs_get_block_write - the write-path (block_write_begin) get_block.
 *
 * Unlike briefs_get_block (which allocates a block on a miss under create=1),
 * this defers allocation: on a miss it sets BH_Delay on the buffer and returns
 * 0 without mapping or allocating anything.  The actual allocation + extent
 * append + journal record happen later, at writeback, when mpage /
 * __block_write_full_folio call briefs_get_block (the allocating variant) to
 * convert the delayed buffer.  This moves block allocation off the write() hot
 * path and onto writeback (#6: one allocation per block at writeback instead of
 * at write_begin; contiguous runs are coalesced later by the merge logic).
 *
 * On a hit the block is already mapped (an overwrite of an existing block, or a
 * write into a previously-fallocated region): map_bh it with no buffer_new so
 * the kernel reads the existing block for a partial overwrite, exactly as the
 * create=0 read path does.
 *
 * No extent_lock is taken: setting BH_Delay is a buffer-local bit, and a
 * concurrent allocator's mapping is reconciled at writeback (briefs_get_block's
 * locked re-check finds the now-mapped block and map_bh's it without
 * re-allocating).  The unlocked lookup is safe because the tree is immutable
 * below a freshly-published root: a concurrent insert publishes a new root last
 * under extent_seq, and the old root it replaced remains valid (its children
 * are on disk / being synced).
 */
int briefs_get_block_write(struct inode *inode, sector_t iblock,
                           struct buffer_head *bh_result, int create)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;
	u64 phys;
	int ret;
	unsigned seq;
	u64 snap_max_end;

	/* Inline-data inodes must be promoted before the block path is used. */
	if (binfo->disk_inode.flags & InodeFlagInlineData)
		return -EIO;

	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		snap_max_end = binfo->cached_max_end;
	} while (read_seqcount_retry(&binfo->extent_seq, seq));

	/* Extent-tail fast path: anything at/after the cached max is unmapped. */
	if (snap_max_end != 0 && (u64)iblock >= snap_max_end) {
		set_buffer_delay(bh_result);
		return 0;
	}

	/* Unlocked lookup (verify CRCs): a hit means the block is already mapped. */
	ret = briefs_inode_lookup_iblock(inode->i_sb, binfo, (u64)iblock, &ext, false);
	if (ret == 0) {
		phys = ext.phys + ((u64)iblock - ext.offset);
		map_bh(bh_result, inode->i_sb, phys);
		return 0;
	}
	/* -ENOENT or -EIO (torn read): treat as not-yet-mapped and defer.  A
	 * spurious -EIO here is harmless: the writeback-time get_block re-lookup
	 * under extent_lock recovers the real mapping (or allocates). */

	/* Not mapped: defer allocation to writeback. */
	set_buffer_delay(bh_result);
	return 0;
}
/* briefs_free_inode_data - free all data blocks owned by an inode */
void briefs_free_inode_data(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_disk_inode disk_di;

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
