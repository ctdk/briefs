/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

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

/* address_space_operations wrappers (kernel 6.12 folio-based APIs). */

/* read_folio: wrapper calling mpage_read_folio with our get_block */
static int briefs_read_folio(struct file *file, struct folio *folio)
{
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
	int i, ret;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	u64 chain_block;
	int ci;
	

	/* Only handle truncation (i_attr_valid & ATTR_SIZE, new size < old) */
	if (!(attr->ia_valid & ATTR_SIZE))
		goto out_copy;

	new_size = attr->ia_size;
	old_size = inode->i_size;

	if (new_size >= old_size)
		goto out_copy;

	pr_debug("briefs: setattr truncate ino=%lu %llu -> %llu\n",
		inode->i_ino, old_size, new_size);

	trunc_block = (new_size + BRIEFS_BLOCK_SIZE - 1) / BRIEFS_BLOCK_SIZE;

	/* Iterate extents in reverse; trunc_block may fall inside an extent
	 * or before it. Remove or shorten affected extents. */
	for (i = binfo->disk_inode.num_extents_total - 1; i >= 0; i--) {
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
					bh = sb_bread(inode->i_sb, chain_block);
					if (!bh) break;
					chain = (struct briefs_extent_chain *)bh->b_data;
					if (chain->checksum != 0 &&
					    briefs_verify_chain_checksum(bh->b_data, chain->checksum) != 0) {
						pr_warn("briefs: truncate ino=%lu chain block %llu checksum mismatch; aborting\n",
							inode->i_ino, chain_block);
						brelse(bh);
						break;
					}
					if (ci < chain->num_extents_in_block) {
						chain->extents[ci] = ext;
						chain->checksum = briefs_chain_checksum(bh->b_data);
						mark_buffer_dirty(bh);
						brelse(bh);
						break;
					}
					ci -= chain->num_extents_in_block;
					chain_block = chain->next_overflow_block;
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
				tbh = sb_bread(inode->i_sb, calias);
				if (!tbh)
					break;
				tc = (struct briefs_extent_chain *)tbh->b_data;
				if (tc->checksum != 0 &&
				    briefs_verify_chain_checksum(tbh->b_data, tc->checksum) != 0) {
					pr_warn("briefs: truncate ino=%lu chain block %llu checksum mismatch; aborting\n",
						inode->i_ino, calias);
					brelse(tbh);
					break;
				}

				if (cidx < tc->num_extents_in_block) {
					/* Found the block — shift extents down */
					int j;
					for (j = cidx; j < tc->num_extents_in_block - 1; j++)
						tc->extents[j] = tc->extents[j + 1];
					tc->num_extents_in_block--;
					tc->checksum = briefs_chain_checksum(tbh->b_data);

					mark_buffer_dirty(tbh);

					/* Free empty non-first chain block */
					if (tc->num_extents_in_block == 0 &&
					    prev_block != 0) {
						struct buffer_head *pbh;
						struct briefs_extent_chain *pc;

						pbh = sb_bread(inode->i_sb, prev_block);
						if (pbh) {
							pc = (struct briefs_extent_chain *)pbh->b_data;
							pc->next_overflow_block = 0;
							pc->checksum = briefs_chain_checksum(pbh->b_data);
							mark_buffer_dirty(pbh);
							brelse(pbh);
						}
						briefs_free_block(&bsi->alloc,
							abs_to_data(bsi->sb, calias));
					}

					brelse(tbh);
					break;
				}

				cidx -= tc->num_extents_in_block;
				prev_block = calias;
				brelse(tbh);
				calias = tc->next_overflow_block;
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

	return 0;

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

	/* Recompute i_blocks from all extents (inline + chain) */
	{
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
	 * Store the symlink target as file data.  Use the same
	 * data-block + extent mechanism as regular file writes.
	 * For short targets this fits in a single inline extent.
	 */
	if (len > 0) {
		struct buffer_head *bh;
		u64 rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0) {
			iput(inode);
			return -ENOSPC;
		}
		u64 phys = data_to_abs(bsi->sb, rel);

		bh = sb_bread(dir->i_sb, phys);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			iput(inode);
			return -EIO;
		}
		memset(bh->b_data, 0, dir->i_sb->s_blocksize);
		memcpy(bh->b_data, symname, len);
		mark_buffer_dirty(bh);
		brelse(bh);

		struct briefs_extent ext;
		ext.offset = 0;
		ext.phys = phys;
		ext.len = 1;
		ext.flags = 0;
		ret = briefs_append_extent(dir->i_sb, &briefs_i(inode)->disk_inode, &ext);
		if (ret != 0) {
			briefs_free_block(&bsi->alloc, rel);
			iput(inode);
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
	if (ret) {
		iput(inode);
		return ret;
	}

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
	if (ret) {
		iput(inode);
		return ret;
	}

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

	/* Read the target from the first extent */
	if (binfo->disk_inode.num_extents_total > 0) {
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
