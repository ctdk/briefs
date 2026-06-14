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


/* Extents per chain block (see struct briefs_extent_chain) */
#define CHAIN_EXTENTS 127

/*
 * Data region start: the allocator trie uses data-relative block numbers
 * with block 0 being the first data block on disk.  Convert to absolute
 * block numbers for sb_bread/sb_getblk using this helper.
 */
/*
 * briefs_compute_i_blocks - compute number of 512-byte sectors used
 * by the inline extents in an inode. This only covers inline extents;
 * chain blocks are handled by briefs_getattr which has sb context.
 */
inline u64 briefs_compute_i_blocks(struct briefs_inode *di)
{
	u64 blocks = 0;
	int i;
	for (i = 0; i < di->num_extents_inline; i++)
		blocks += di->inline_extents[i].len;
	return blocks * (BRIEFS_BLOCK_SIZE / 512);
}
/*
 * briefs_read_extent - read an extent by logical index from inline extents,
 * walking chain blocks as needed. Returns the extent data in *ext.
 * Returns 0 on success, -ENOENT if index >= num_extents_total.
 */
int briefs_read_extent(struct super_block *sb, struct briefs_inode *di,
                               int idx, struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	int chain_idx;
	u64 chain_block;

	if (idx < 0 || idx >= di->num_extents_total)
		return -ENOENT;

	binfo = container_of(di, struct briefs_inode_info, disk_inode);

	/* Check if the extent is inline — read under seqcount protection */
	if (idx < di->num_extents_inline) {
		unsigned seq;
		do {
			seq = read_seqcount_begin(&binfo->extent_seq);
			*ext = di->inline_extents[idx];
		} while (read_seqcount_retry(&binfo->extent_seq, seq));
		return 0;
	}

	/* Walk chain blocks */
	chain_block = di->extent_inline_base;
	if (chain_block == 0)
		return -ENOENT;

	chain_idx = idx - di->num_extents_inline;

	while (chain_block) {
		bh = sb_bread(sb, chain_block);
		if (!bh) {
			pr_err("briefs: failed to read chain block %llu\n", chain_block);
			return -EIO;
		}
		chain = (struct briefs_extent_chain *)bh->b_data;

		if (briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
			pr_err("briefs: chain block %llu checksum mismatch\n", chain_block);
			brelse(bh);
			return -EIO;
		}

		if (chain_idx < chain->num_extents_in_block) {
			*ext = chain->extents[chain_idx];
			brelse(bh);
			return 0;
		}

		chain_idx -= chain->num_extents_in_block;
		chain_block = chain->next_overflow_block;
		brelse(bh);
	}

	return -ENOENT;
}
/*
 * __briefs_append_extent - internal append helper.  Caller must hold
 * binfo->extent_lock.  Appends an extent to the list, creating chain blocks
 * if the 8 inline slots are full.
 * Returns 0 on success, -ENOSPC if no blocks available for chain.
 */
static int __briefs_append_extent(struct super_block *sb, struct briefs_inode *di,
                                 struct briefs_extent *ext)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	u64 rel, chain_block;
	int slot;

	/* Get the inode info from the embedded disk_inode */
	binfo = container_of(di, struct briefs_inode_info, disk_inode);

	/*
	 * All modifications to extent fields are done inside a write_seqcount
	 * critical section so that briefs_write_inode (called by VFS writeback)
	 * never sees a partially-updated extent list.
	 */

	if (di->num_extents_inline < 8) {
		/* Still fits inline */
		write_seqcount_begin(&binfo->extent_seq);
		int n = di->num_extents_inline;
		di->inline_extents[n] = *ext;
		di->num_extents_inline++;
		di->num_extents_total++;
		write_seqcount_end(&binfo->extent_seq);

		/* Journal the extent allocation */
		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    ext->offset, ext->phys,
					    ext->len, n);
		return 0;
	}

	/* Inline is full - append to chain blocks */

	/* Allocate first chain block if needed */
	chain_block = di->extent_inline_base;
	if (chain_block == 0) {
		rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0) {
			return -ENOSPC;
		}
		chain_block = data_to_abs(bsi->sb, rel);

		write_seqcount_begin(&binfo->extent_seq);
		di->extent_inline_base = chain_block;
		write_seqcount_end(&binfo->extent_seq);

		/* Journal the chain block allocation */
		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, chain_block, 1, -1);

		bh = sb_getblk(sb, chain_block);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			return -EIO;
		}
		/* sb_getblk may return an unmapped buffer on loop devices */
		if (!buffer_mapped(bh)) {
			bh->b_blocknr = chain_block;
			set_buffer_mapped(bh);
		}
		memset(bh->b_data, 0, sb->s_blocksize);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	/* Walk to the last chain block */
	while (1) {
		bh = sb_bread(sb, chain_block);
		if (!bh) {
			return -EIO;
		}
		chain = (struct briefs_extent_chain *)bh->b_data;

		if (briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
			pr_err("briefs: chain block %llu checksum mismatch on append\n", chain_block);
			brelse(bh);
			return -EIO;
		}

		if (chain->num_extents_in_block < CHAIN_EXTENTS) {
			/* Room in this block */
			slot = chain->num_extents_in_block;
			chain->extents[slot] = *ext;
			chain->num_extents_in_block++;
			chain->checksum = briefs_chain_checksum(bh->b_data);
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);

			write_seqcount_begin(&binfo->extent_seq);
			di->num_extents_total++;
			write_seqcount_end(&binfo->extent_seq);

			/* Journal the extent addition to chain */
			briefs_journal_extent_alloc(bsi->journal, di->inode_number,
						    ext->offset, ext->phys,
						    ext->len, -1);
			return 0;
		}

		/* Chain block full - follow or allocate next */
		if (chain->next_overflow_block) {
			chain_block = chain->next_overflow_block;
			brelse(bh);
			continue;
		}

		/* Allocate a new chain block */
		rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0) {
			brelse(bh);
			return -ENOSPC;
		}
		u64 new_block = data_to_abs(bsi->sb, rel);
		chain->next_overflow_block = new_block;
		chain->checksum = briefs_chain_checksum(bh->b_data);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		/* Journal the new chain block allocation */
		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, new_block, 1, -1);

		/* Set up new chain block */
		bh = sb_getblk(sb, new_block);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			return -EIO;
		}
		/* sb_getblk may return an unmapped buffer on loop devices */
		if (!buffer_mapped(bh)) {
			bh->b_blocknr = new_block;
			set_buffer_mapped(bh);
		}
		memset(bh->b_data, 0, sb->s_blocksize);
		chain = (struct briefs_extent_chain *)bh->b_data;
		chain->extents[0] = *ext;
		chain->num_extents_in_block = 1;
		chain->checksum = briefs_chain_checksum(bh->b_data);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		write_seqcount_begin(&binfo->extent_seq);
		di->num_extents_total++;
		write_seqcount_end(&binfo->extent_seq);

		/* Journal the extent addition to chain */
		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    ext->offset, ext->phys,
					    ext->len, -1);
		return 0;
	}
}

/*
 * briefs_append_extent - append an extent to the extent list.  Takes the
 * per-inode extent lock and calls the internal helper.  This is the
 * entry point for callers that are not already holding the lock.
 */
int briefs_append_extent(struct super_block *sb, struct briefs_inode *di,
                         struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo;
	int ret;

	binfo = container_of(di, struct briefs_inode_info, disk_inode);
	mutex_lock(&binfo->extent_lock);
	ret = __briefs_append_extent(sb, di, ext);
	mutex_unlock(&binfo->extent_lock);
	return ret;
}

/*
 * briefs_read_extent_chain - read an extent from the chain portion only,
 * using caller-supplied snapshot values for num_extents_inline and
 * extent_inline_base.  This avoids the seqcount inside briefs_read_extent
 * and allows the caller (briefs_get_block) to iterate the extent list
 * using a single metadata snapshot taken before the loop.
 *
 * Returns 0 on success with *ext filled, -ENOENT if not found.
 */
static int briefs_read_extent_chain(struct super_block *sb, int idx,
                                    u64 snap_inline, u64 snap_chain_base,
                                    struct briefs_extent *ext)
{
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	int chain_idx;
	u64 chain_block;

	if (idx < snap_inline)
		return -ENOENT;

	chain_block = snap_chain_base;
	if (chain_block == 0)
		return -ENOENT;

	chain_idx = idx - snap_inline;

	while (chain_block) {
		bh = sb_bread(sb, chain_block);
		if (!bh) {
			pr_err("briefs: failed to read chain block %llu\n", chain_block);
			return -EIO;
		}
		chain = (struct briefs_extent_chain *)bh->b_data;

		if (briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
			pr_err("briefs: chain block %llu checksum mismatch\n", chain_block);
			brelse(bh);
			return -EIO;
		}

		if (chain_idx < chain->num_extents_in_block) {
			*ext = chain->extents[chain_idx];
			brelse(bh);
			return 0;
		}

		chain_idx -= chain->num_extents_in_block;
		chain_block = chain->next_overflow_block;
		brelse(bh);
	}

	return -ENOENT;
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
	int i, ret;
	unsigned seq;

	/*
	 * Snapshot extent metadata under seqcount so we iterate a
	 * consistent view even if briefs_append_extent runs concurrently.
	 */
	u64 snap_total, snap_inline, snap_chain_base;

	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		snap_total = binfo->disk_inode.num_extents_total;
		snap_inline = binfo->disk_inode.num_extents_inline;
		snap_chain_base = binfo->disk_inode.extent_inline_base;
	} while (read_seqcount_retry(&binfo->extent_seq, seq));

	/* Look up iblock in the snapped extent view */
	for (i = 0; i < snap_total; i++) {
		if (i < snap_inline) {
			/*
			 * Inline extent — read under seqcount protection.
			 * briefs_read_extent handles this correctly.
			 */
			ret = briefs_read_extent(inode->i_sb, &binfo->disk_inode,
			                         i, &ext);
		} else {
			/*
			 * Chain extent — walk from our snapped chain base.
			 * The chain blocks are append-only (never freed until
			 * eviction), so addressing into them by snapped index
			 * is safe even if a concurrent writer adds new blocks.
			 */
			ret = briefs_read_extent_chain(inode->i_sb, i,
			                               snap_inline,
			                               snap_chain_base, &ext);
		}
		if (ret != 0)
			break;
		if ((u64)iblock >= ext.offset &&
		    (u64)iblock < ext.offset + ext.len) {
			phys = ext.phys + ((u64)iblock - ext.offset);
			map_bh(bh_result, inode->i_sb, phys);
			return 0;
		}
	}

	/* Not mapped */
	if (!create)
		return 0;

	/*
	 * Allocate and append a new single-block extent.  Take the extent
	 * lock and re-check the mapping: another thread may have appended the
	 * block while we were doing the unlocked lookup above.  Without the
	 * re-check, two threads can both append an extent for the same iblock,
	 * leaving the first allocated block unreachable (leaked).
	 */
	mutex_lock(&binfo->extent_lock);

	for (i = 0; i < binfo->disk_inode.num_extents_total; i++) {
		ret = briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext);
		if (ret != 0)
			continue;
		if ((u64)iblock >= ext.offset &&
		    (u64)iblock < ext.offset + ext.len) {
			phys = ext.phys + ((u64)iblock - ext.offset);
			map_bh(bh_result, inode->i_sb, phys);
			mutex_unlock(&binfo->extent_lock);
			return 0;
		}
	}

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

		ret = __briefs_append_extent(inode->i_sb, &binfo->disk_inode, &new_ext);
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
/* briefs_free_inode_data - free all data blocks owned by an inode */
void briefs_free_inode_data(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	struct briefs_extent ext;
	u64 chain_block, next_chain;
	int i;
	u64 b;

	/* For directories, free the trie instead of file extents */
	if (S_ISDIR(inode->i_mode)) {
		briefs_trie_free_all(inode->i_sb, &binfo->disk_inode);
		return;
	}

	/* Walk all extents (inline + chain) */
	for (i = 0; i < binfo->disk_inode.num_extents_total; i++) {
		if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext) != 0)
			break;

		/* Journal the extent free */
		briefs_journal_extent_free(bsi->journal, inode->i_ino,
					   ext.offset, ext.phys, ext.len);

		/*
		 * Sanity-check the extent length.  A corrupt ext.len (e.g. from
		 * an inode written by an older buggy version) could cause an
		 * infinite loop here, triggering a soft lockup watchdog.
		 * Cap to a reasonable maximum to avoid this.
		 */
		u64 blocks_to_free = ext.len;
		if (blocks_to_free > 1024 * 1024) {
			pr_warn("briefs: inode %lu extent has suspicious len=%llu, capping to 1\n",
				inode->i_ino, ext.len);
			blocks_to_free = 1;
		}

		for (b = 0; b < blocks_to_free; b++) {
			u64 abs_block = ext.phys + b;
			u64 rel_block = abs_to_data(bsi->sb, abs_block);
			briefs_free_block(&bsi->alloc, rel_block);
		}
	}

	/* Free chain blocks */
	chain_block = binfo->disk_inode.extent_inline_base;
	while (chain_block) {
		bh = sb_bread(inode->i_sb, chain_block);
		if (!bh) break;
		chain = (struct briefs_extent_chain *)bh->b_data;
		if (chain->checksum != 0 &&
		    briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
			pr_warn("briefs: chain block %llu checksum mismatch while freeing inode %lu; continuing\n",
				chain_block, inode->i_ino);
		}
		next_chain = chain->next_overflow_block;
		brelse(bh);

		/* Journal the chain block free */
		briefs_journal_extent_free(bsi->journal, inode->i_ino,
					   0, chain_block, 1);

		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, chain_block));
		chain_block = next_chain;
	}

	write_seqcount_begin(&binfo->extent_seq);
	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
	binfo->disk_inode.extent_inline_base = 0;
	memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
	write_seqcount_end(&binfo->extent_seq);
}
