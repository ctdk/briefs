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
#include <linux/sort.h>
#include <linux/pagemap.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

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

	return mpage_read_folio(folio, briefs_get_block);
}
/* cribbing from xiafs rather than use the frankly shocking no-op openclaw
 * suggested. I don't think briefs actually has trouble with running on loopback
 * devices. o_O */
static int briefs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, briefs_get_block);
}
/* bmap: map file block to physical block */
static sector_t briefs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, briefs_get_block);
}
/* cribbed from xiafs, at least for now. */
static int briefs_write_begin(struct file *file, struct address_space *mapping, loff_t pos, unsigned len, struct folio **foliop, void **fsdata) {
	int ret;
	ret = block_write_begin(mapping, pos, len, foliop, briefs_get_block);

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

	return ret;
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
	return 0;
}
/* Release file */
int briefs_release(struct inode *inode, struct file *file) {
	pr_debug("briefs: release inode %lu\n", inode->i_ino);
	return 0;
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
	/* Promotion bypasses __briefs_append_extent, so update the tail cache
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

/* briefs_setattr - set file attributes (truncate support).
 * Frees data blocks on truncation, updates the inode on disk.
 */
int briefs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_extent ext;
	u64 new_size, old_size, trunc_block;
	int ret;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	u64 chain_block;
	int ci;
	s64 i;

	/* Only handle size changes here. */
	if (!(attr->ia_valid & ATTR_SIZE))
		goto out_copy;

	new_size = attr->ia_size;
	old_size = inode->i_size;

	if (new_size == old_size)
		goto out_copy;

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

	/* Only shrinking remains; growth of extent-backed files is unchanged. */
	if (new_size > old_size) {
		mutex_unlock(&binfo->extent_lock);
		goto out_copy;
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

	/* Nothing to do if the inode has no extents. */
	if (binfo->disk_inode.num_extents_total == 0)
		goto out_unlock;

	/* Iterate extents in reverse; trunc_block may fall inside an extent
	 * or before it. Remove or shorten affected extents. */
	for (i = (s64)binfo->disk_inode.num_extents_total - 1; i >= 0; i--) {
		ret = briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext);
		if (ret != 0)
			break;

		u64 ext_start = ext.offset;
		u64 ext_end = ext.offset + ext.len;

		if (trunc_block >= ext_end) {
			/* Extent is entirely before truncation point - keep */
			continue;
		}

		if (trunc_block > ext_start && trunc_block < ext_end) {
			/* Truncation falls inside this extent - shorten it */
			u64 blocks_to_free = ext_end - trunc_block;

			/* Journal the partial extent free */
			briefs_journal_extent_free(bsi->journal, inode->i_ino,
					   ext.offset + ext.len - blocks_to_free,
					   ext.phys + ext.len - blocks_to_free,
					   blocks_to_free);

			briefs_free_blocks_range(bsi,
						 ext.phys + ext.len - blocks_to_free,
						 blocks_to_free);

			ext.len -= blocks_to_free;

			/* Update the extent in place (inline or chain) */
			write_seqcount_begin(&binfo->extent_seq);
			if (i < binfo->disk_inode.num_extents_inline) {
				binfo->disk_inode.inline_extents[i] = ext;
			} else {
				ci = i - binfo->disk_inode.num_extents_inline;
				chain_block = binfo->disk_inode.extent_inline_base;
				while (chain_block && ci >= 0) {
					u32 num_in_block;

					bh = sb_bread(inode->i_sb, chain_block);
					if (!bh) break;
					chain = (struct briefs_extent_chain *)bh->b_data;
					if (le64_to_cpu(chain->checksum) != 0 &&
					    briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
						pr_warn("briefs: truncate ino=%lu chain block %llu checksum mismatch; aborting\n",
							inode->i_ino, chain_block);
						brelse(bh);
						break;
					}
					num_in_block = le32_to_cpu(chain->num_extents_in_block);
					if (ci < num_in_block) {
						briefs_cpu_extent_to_disk(&ext, &chain->extents[ci]);
						chain->checksum = cpu_to_le64(briefs_chain_checksum(bh->b_data));
						set_buffer_verified(bh);
						write_seqcount_begin(&binfo->extent_seq);
						mark_buffer_dirty(bh);
						write_seqcount_end(&binfo->extent_seq);
						brelse(bh);
						break;
					}
					ci -= num_in_block;
					chain_block = le64_to_cpu(chain->next_overflow_block);
					brelse(bh);
				}
			}
			write_seqcount_end(&binfo->extent_seq);
			/* Done - all later extents already removed */
			break;
		}

		/* trunc_block <= ext_start - free the entire extent */
		{
			/* Journal the full extent free */
			briefs_journal_extent_free(bsi->journal, inode->i_ino,
					   ext.offset, ext.phys, ext.len);
			briefs_free_blocks_range(bsi, ext.phys, ext.len);
		}

		/* Remove the extent: shift remaining extents down */
		write_seqcount_begin(&binfo->extent_seq);
		if (i < binfo->disk_inode.num_extents_inline) {
			/* Extent is inline - remove it */
			int j;
			for (j = i; j < binfo->disk_inode.num_extents_inline - 1; j++)
				binfo->disk_inode.inline_extents[j] = binfo->disk_inode.inline_extents[j + 1];
			binfo->disk_inode.num_extents_inline--;
		} else {
			/* Extent is in a chain block — remove it and
			 * shift remaining extents in the block down. */
			struct buffer_head *tbh;
			struct briefs_extent_chain *tc;
			u64 calias = binfo->disk_inode.extent_inline_base;
			int cidx = i - binfo->disk_inode.num_extents_inline;
			u64 prev_block = 0;

			while (calias && cidx >= 0) {
				u32 num_in_block;

				tbh = sb_bread(inode->i_sb, calias);
				if (!tbh)
					break;
				tc = (struct briefs_extent_chain *)tbh->b_data;
				if (le64_to_cpu(tc->checksum) != 0 &&
				    briefs_verify_chain_checksum(tbh->b_data, tc->checksum) != 0) {
					pr_warn("briefs: truncate ino=%lu chain block %llu checksum mismatch; aborting\n",
						inode->i_ino, calias);
					brelse(tbh);
					break;
				}

				num_in_block = le32_to_cpu(tc->num_extents_in_block);
				if (cidx < num_in_block) {
					/* Found the block — shift extents down */
					int j;
					struct briefs_extent tmp;

					for (j = cidx; j < num_in_block - 1; j++) {
						briefs_disk_extent_to_cpu(&tc->extents[j + 1], &tmp);
						briefs_cpu_extent_to_disk(&tmp, &tc->extents[j]);
					}
					tc->num_extents_in_block = cpu_to_le32(num_in_block - 1);
					tc->checksum = cpu_to_le64(briefs_chain_checksum(tbh->b_data));
					set_buffer_verified(tbh);

					write_seqcount_begin(&binfo->extent_seq);
					mark_buffer_dirty(tbh);
					write_seqcount_end(&binfo->extent_seq);

					/* Free empty non-first chain block */
					if (num_in_block - 1 == 0 && prev_block != 0) {
						struct buffer_head *pbh;
						struct briefs_extent_chain *pc;

						pbh = sb_bread(inode->i_sb, prev_block);
						if (pbh) {
							pc = (struct briefs_extent_chain *)pbh->b_data;
							pc->next_overflow_block = cpu_to_le64(0);
							pc->checksum = cpu_to_le64(briefs_chain_checksum(pbh->b_data));
							set_buffer_verified(pbh);
							write_seqcount_begin(&binfo->extent_seq);
							mark_buffer_dirty(pbh);
							write_seqcount_end(&binfo->extent_seq);
							brelse(pbh);
						}
						briefs_free_block(&bsi->alloc,
							abs_to_data(bsi->sb, calias));
					}

					brelse(tbh);
					break;
				}

				cidx -= num_in_block;
				prev_block = calias;
				brelse(tbh);
				calias = le64_to_cpu(tc->next_overflow_block);
			}
		}

		binfo->disk_inode.num_extents_total--;
		write_seqcount_end(&binfo->extent_seq);
	}

	/* If trunc_block is 0 (truncate to empty), use the full cleanup path */
	if (new_size == 0) {
		/* Free all remaining chain blocks */
		briefs_free_chain_blocks(inode->i_sb, binfo->disk_inode.extent_inline_base);
		write_seqcount_begin(&binfo->extent_seq);
		binfo->disk_inode.extent_inline_base = 0;
		binfo->disk_inode.num_extents_inline = 0;
		binfo->disk_inode.num_extents_total = 0;
		memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
		/* All extents gone -> invalidate the tail cache (0 = unknown). */
		binfo->cached_max_end = 0;
		write_seqcount_end(&binfo->extent_seq);
	}

	/* Update inode metadata */
	inode->i_size = new_size;
	binfo->disk_inode.filesize = new_size;

	/* Recompute i_blocks */
	{
		u64 total_blocks = 0;
		struct briefs_extent ee;
		int j;
		for (j = 0; j < binfo->disk_inode.num_extents_total; j++) {
			if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, j, &ee) == 0)
				total_blocks += ee.len;
		}
		inode->i_blocks = total_blocks * (BRIEFS_BLOCK_SIZE / 512);
	}

	/* Let VFS handle page cache truncation */
	truncate_setsize(inode, new_size);

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
	/* For non-size changes, just copy attributes */
	setattr_copy(idmap, inode, attr);
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
		/* Recompute i_blocks from all extents (inline + chain) */
		struct briefs_extent ext;
		u64 total_blocks = 0;
		int j;
		for (j = 0; j < binfo->disk_inode.num_extents_total; j++) {
			if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, j, &ext) == 0)
				total_blocks += ext.len;
		}
		i_blocks = total_blocks * (BRIEFS_BLOCK_SIZE / 512);
	}
	inode->i_blocks = i_blocks;
	stat->blocks = i_blocks;
	pr_debug("briefs: getattr ino=%lu i_blocks=%llu\n", inode->i_ino, i_blocks);
	return 0;
}

/* Compare two briefs_extents by logical offset, for sort(). */
static int briefs_extent_cmp_offset(const void *a, const void *b)
{
	const struct briefs_extent *ea = a, *eb = b;

	if (ea->offset < eb->offset)
		return -1;
	if (ea->offset > eb->offset)
		return 1;
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
	u64 snap_total, end;
	struct briefs_extent *arr;
	unsigned int seq;
	int ret, i;

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

	/* Snapshot the extent count under seqcount (briefs_get_block style):
	 * chain blocks are append-only for the inode's lifetime, so walking
	 * by index within snap_total is safe against concurrent appends. */
	do {
		seq = read_seqcount_begin(&binfo->extent_seq);
		snap_total = di->num_extents_total;
	} while (read_seqcount_retry(&binfo->extent_seq, seq));

	if (snap_total == 0)
		return 0;   /* fully sparse / empty extent-backed file */

	arr = kmalloc_array(snap_total, sizeof(*arr), GFP_NOFS);
	if (!arr)
		return -ENOMEM;

	/* Collect every extent (reuse briefs_read_extent, as
	 * briefs_free_inode_data does at briefs_extent.c:700-712). */
	for (i = 0; i < snap_total; i++) {
		if (briefs_read_extent(sb, di, i, &arr[i])) {
			ret = -EIO;
			goto out;
		}
	}

	/* fiemap requires ascending logical order; the on-disk list is in
	 * append order and may be unsorted after out-of-order writes. */
	sort(arr, snap_total, sizeof(arr[0]), briefs_extent_cmp_offset, NULL);

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
	kfree(arr);
	return ret;
}

/*
 * briefs_block_mapped - return true if logical block @iblock already has a
 * physical mapping in the inode's extent list.
 */
static bool briefs_block_mapped(struct inode *inode, u64 iblock)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;
	int i;

	for (i = 0; i < binfo->disk_inode.num_extents_total; i++) {
		if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext) != 0)
			continue;
		if (iblock >= ext.offset && iblock < ext.offset + ext.len)
			return true;
	}
	return false;
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
 * briefs_replace_extent_list - atomically replace an inode's entire extent
 * list with @new_count extents from @new_exts.  Allocates and writes new
 * chain blocks if needed, updates the in-memory disk_inode, and returns the
 * old chain base in *@old_chain_base_out so the caller can free it after the
 * new inode has been persisted.
 *
 * Returns 0 on success or a negative errno.  On error the inode's extent
 * metadata is unchanged and no data blocks have been freed.
 */
static int briefs_replace_extent_list(struct super_block *sb,
                                      struct briefs_inode *di,
                                      int new_count,
                                      struct briefs_extent *new_exts,
                                      u64 *old_chain_base_out)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_inode_info *binfo = container_of(di,
						       struct briefs_inode_info,
						       disk_inode);
	struct briefs_extent_chain *chain;
	struct buffer_head *bh;
	u64 *chain_blocks = NULL;
	int chain_count = 0;
	int num_inline;
	int i, j, ret = 0;

	*old_chain_base_out = di->extent_inline_base;

	if (new_count > 8) {
		chain_count = (new_count - 8 + BRIEFS_CHAIN_EXTENTS - 1)
			      / BRIEFS_CHAIN_EXTENTS;
		chain_blocks = kmalloc_array(chain_count, sizeof(*chain_blocks),
					     GFP_KERNEL);
		if (!chain_blocks)
			return -ENOMEM;

		/* Allocate new chain blocks first; if we run out of space the
		 * caller can back out without having modified anything. */
		for (i = 0; i < chain_count; i++) {
			u64 rel = briefs_alloc_block(&bsi->alloc);

			if (rel == 0) {
				ret = -ENOSPC;
				goto free_chain_blocks;
			}
			chain_blocks[i] = data_to_abs(bsi->sb, rel);
		}

		/* Write the new chain blocks before making them reachable. */
		for (i = 0; i < chain_count; i++) {
			int ext_idx = 8 + i * BRIEFS_CHAIN_EXTENTS;
			int extents_in_block = min(new_count - ext_idx,
						   BRIEFS_CHAIN_EXTENTS);
			u64 next_block = (i < chain_count - 1)
					 ? chain_blocks[i + 1] : 0;

			bh = briefs_get_zero_block(sb, chain_blocks[i]);
			if (!bh) {
				ret = -EIO;
				goto free_chain_blocks;
			}
			chain = (struct briefs_extent_chain *)bh->b_data;
			chain->next_overflow_block = cpu_to_le64(next_block);
			chain->num_extents_in_block =
				cpu_to_le32(extents_in_block);
			for (j = 0; j < extents_in_block; j++)
				briefs_cpu_extent_to_disk(&new_exts[ext_idx + j],
							  &chain->extents[j]);
			chain->checksum =
				cpu_to_le64(briefs_chain_checksum(bh->b_data));
			set_buffer_verified(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
		}
	}

	mutex_lock(&binfo->extent_lock);

	write_seqcount_begin(&binfo->extent_seq);
	num_inline = min(new_count, 8);
	di->num_extents_inline = num_inline;
	di->num_extents_total = new_count;
	di->extent_inline_base = chain_blocks ? chain_blocks[0] : 0;
	memset(di->inline_extents, 0, sizeof(di->inline_extents));
	for (i = 0; i < num_inline; i++)
		di->inline_extents[i] = new_exts[i];
	/* The extent list was rebuilt from new_exts[]; recompute the tail cache
	 * exactly from the in-memory list (cheap, no I/O) so the fast path stays
	 * accurate after compaction. */
	{
		u64 mx = 0;
		for (i = 0; i < new_count; i++) {
			u64 end = new_exts[i].offset + new_exts[i].len;
			if (end > mx)
				mx = end;
		}
		binfo->cached_max_end = mx;
	}
	write_seqcount_end(&binfo->extent_seq);

	mutex_unlock(&binfo->extent_lock);

	/* Journal the chain-block allocations so replay marks them used. */
	for (i = 0; i < chain_count; i++)
		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, chain_blocks[i], 1, -1);

	kfree(chain_blocks);
	return 0;

free_chain_blocks:
	for (i = 0; i < chain_count; i++) {
		if (chain_blocks[i]) {
			briefs_journal_extent_free(bsi->journal, 0, 0,
						   chain_blocks[i], 1);
			briefs_free_block(&bsi->alloc,
					abs_to_data(bsi->sb, chain_blocks[i]));
		}
	}
	kfree(chain_blocks);
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
	u64 old_chain_base = 0;
	bool changed = false;
	bool need_partial_start, need_partial_end;
	u32 partial_start_off, partial_end_off;
	int ret = 0;
	int i;

	need_partial_start = (offset & (BRIEFS_BLOCK_SIZE - 1)) != 0;
	need_partial_end = (end & (BRIEFS_BLOCK_SIZE - 1)) != 0;
	partial_start_off = offset & (BRIEFS_BLOCK_SIZE - 1);
	partial_end_off = end & (BRIEFS_BLOCK_SIZE - 1);

	if (binfo->disk_inode.flags & InodeFlagInlineData) {
		loff_t punch_start = max_t(loff_t, offset, 0);
		loff_t punch_end = min_t(loff_t, end, inode->i_size);

		if (punch_start < punch_end) {
			memset(binfo->disk_inode.inline_data + punch_start, 0,
			       punch_end - punch_start);
			changed = true;
		}

		if (changed) {
			briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
						  &binfo->disk_inode, false);
			briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
			briefs_journal_inode_full(bsi->journal, inode->i_ino,
						  &disk_di);
		}
		goto out_update;
	}

	if (binfo->disk_inode.num_extents_total > INT_MAX / 2) {
		ret = -ENOMEM;
		goto out_free;
	}
	old_count = (int)binfo->disk_inode.num_extents_total;
	if (old_count == 0)
		goto out_update;

	old_exts = kmalloc_array(old_count, sizeof(*old_exts), GFP_KERNEL);
	new_exts = kmalloc_array(old_count * 2, sizeof(*new_exts), GFP_KERNEL);
	if (!old_exts || !new_exts) {
		ret = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < old_count; i++) {
		if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, i,
				       &old_exts[i]) != 0) {
			ret = -EIO;
			goto out_free;
		}
	}

	/*
	 * Build the new extent list and zero any partially-holed boundary
	 * blocks that remain allocated.
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

		if (need_partial_start && start_blk >= ext.offset &&
		    start_blk < ext_end) {
			u64 abs_block = ext.phys + (start_blk - ext.offset);

			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      partial_start_off,
						      BRIEFS_BLOCK_SIZE);
			if (ret)
				goto out_free;
		}

		if (need_partial_end && (end_blk - 1) >= ext.offset &&
		    (end_blk - 1) < ext_end) {
			u64 abs_block = ext.phys + ((end_blk - 1) - ext.offset);

			ret = briefs_zero_block_range(inode->i_sb, abs_block,
						      0,
						      partial_end_off);
			if (ret)
				goto out_free;
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
	 * Replace the extent list atomically.  This allocates chain blocks if
	 * necessary, writes them, and updates the in-memory inode metadata.
	 * No data blocks have been freed yet, so failure here is harmless.
	 */
	ret = briefs_replace_extent_list(inode->i_sb, &binfo->disk_inode,
					 new_count, new_exts, &old_chain_base);
	if (ret)
		goto out_free;

	/*
	 * The inode now references only kept blocks.  It is safe to free the
	 * removed data blocks and the old chain blocks.
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

	if (old_chain_base)
		briefs_free_chain_blocks(inode->i_sb, old_chain_base);

	inode->i_blocks = briefs_compute_i_blocks(inode->i_sb,
						 &binfo->disk_inode);

	briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
				  &binfo->disk_inode, false);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

out_update:
	if (changed) {
		ktime_get_real_ts64(&now);
		inode->i_ctime_sec = now.tv_sec;
		inode->i_ctime_nsec = now.tv_nsec;
		briefs_sync_inode_times(inode, &binfo->disk_inode);
		mark_inode_dirty(inode);
		truncate_pagecache_range(inode, offset, end - 1);
	}

out_free:
	kfree(old_exts);
	kfree(new_exts);
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

	if (mode & FALLOC_FL_PUNCH_HOLE) {
		ret = briefs_do_punch_hole(file, offset, len);
		return ret;
	}

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
				ext.flags = 0;
				ret = briefs_append_extent_nojournal(inode->i_sb,
								     &binfo->disk_inode,
								     &ext);
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
			ext.flags = 0;

			ret = briefs_append_extent_nojournal(inode->i_sb,
							     &binfo->disk_inode,
							     &ext);
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
		ktime_get_real_ts64(&now);
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

	inode = briefs_new_inode(dir, dentry, S_IFLNK | 0777, 0);
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

	inode = briefs_new_inode(dir, dentry, mode, rdev);
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
