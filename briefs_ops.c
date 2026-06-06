/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs VFS operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

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
static inline u64 briefs_compute_i_blocks(struct briefs_inode *di)
{
	u64 blocks = 0;
	int i;
	for (i = 0; i < di->num_extents_inline; i++)
		blocks += di->inline_extents[i].len;
	return blocks * (BRIEFS_BLOCK_SIZE / 512);
}

/* Forward declaration */
static void briefs_free_inode_num(struct super_block *sb, u64 ino);

/* Inode operations for directories */
const struct inode_operations briefs_dir_inode_ops = {
	.create = briefs_create,
	.lookup = briefs_lookup,
	.mkdir = briefs_mkdir,
	.unlink = briefs_unlink,
	.rmdir = briefs_unlink,
	.rename = briefs_rename,
};

/* Inode operations for files */
const struct inode_operations briefs_file_inode_ops = {
	.setattr = briefs_setattr,
	.getattr = briefs_getattr,
};

/* File operations for directories */
const struct file_operations briefs_dir_operations = {
	.llseek = generic_file_llseek,
	.iterate_shared = briefs_readdir,
	.release = NULL,
	.fsync = briefs_fsync,
};

/* File operations for regular files */
const struct file_operations briefs_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = briefs_read_iter,
	.write_iter = briefs_write_iter,
	.open = briefs_open,
	.release = briefs_release,
	.mmap = generic_file_mmap,
	.fsync = briefs_fsync,
};

/* Superblock operations */
const struct super_operations briefs_super_ops = {
	.put_super = briefs_put_super,
	.statfs = briefs_statfs,
	.write_inode = briefs_write_inode,
	.evict_inode = briefs_evict_inode,
	.umount_begin = briefs_umount_begin,
	.alloc_inode = briefs_alloc_vfs_inode,
	.free_inode = briefs_free_inode,
};

/* briefs_readdir - enumerate directory contents */
int briefs_readdir(struct file *file, struct dir_context *ctx) {
	struct inode *dir = file_inode(file);
	struct briefs_inode_info *binfo;
	
	pr_debug("briefs: readdir offset=%llu (trie)\n", ctx->pos);

	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	/* Emit . and .. via dir_emit_dots */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return -EIO;
		ctx->pos = 2;
	}

	/* Iterate the directory trie starting from ctx->pos */
	{
		struct trie_iter *iter;
		int trie_idx = ctx->pos > 2 ? ctx->pos - 2 : 0;
		int target_idx = 0;

		iter = kmalloc(sizeof(struct trie_iter), GFP_KERNEL);
		if (!iter)
			return -ENOMEM;

		binfo = briefs_i(dir);
		briefs_trie_iter_init(iter, &binfo->disk_inode);

		while (1) {
			char entry_name_buf[BRIEFS_NAME_LEN + 1];
			int entry_name_len;
			u64 entry_ino;
			u8 entry_type;

			if (briefs_trie_iter_next(dir->i_sb, iter, &entry_ino, &entry_type,
			                         entry_name_buf, &entry_name_len) != 0)
				break;

			if (target_idx < trie_idx) {
				target_idx++;
				ctx->pos++;
				continue;
			}

			unsigned int file_type = (entry_type << 12) & S_IFMT;

			if (!dir_emit(ctx, entry_name_buf, entry_name_len,
			              entry_ino, fs_umode_to_dtype(file_type))) {
				kfree(iter);
				return 0;
			}

			ctx->pos++;
			target_idx++;
		}

		kfree(iter);
	}

	return 0;
}

/* Extents per chain block (see struct briefs_extent_chain) */
#define CHAIN_EXTENTS 256

/*
 * briefs_read_extent - read an extent by logical index from inline extents,
 * walking chain blocks as needed. Returns the extent data in *ext.
 * Returns 0 on success, -ENOENT if index >= num_extents_total.
 */
static int briefs_read_extent(struct super_block *sb, struct briefs_inode *di,
                               int idx, struct briefs_extent *ext)
{
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	int chain_idx;
	u64 chain_block;

	if (idx < 0 || idx >= di->num_extents_total)
		return -ENOENT;

	/* Check if the extent is inline */
	if (idx < di->num_extents_inline) {
		*ext = di->inline_extents[idx];
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
 * briefs_append_extent - append an extent to the extent list, creating
 * chain blocks if the 8 inline slots are full.
 * Returns 0 on success, -ENOSPC if no blocks available for chain.
 */
static int briefs_append_extent(struct super_block *sb, struct briefs_inode *di,
                                 struct briefs_extent *ext)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_extent_chain *chain;
	u64 rel, chain_block;
	int slot;

	if (di->num_extents_inline < 8) {
		/* Still fits inline */
		int n = di->num_extents_inline;
		di->inline_extents[n] = *ext;
		di->num_extents_inline++;
		di->num_extents_total++;
		return 0;
	}

	/* Inline is full - append to chain blocks */

	/* Allocate first chain block if needed */
	chain_block = di->extent_inline_base;
	if (chain_block == 0) {
		rel = briefs_alloc_block(&bsi->alloc);
		if (rel == 0)
			return -ENOSPC;
		chain_block = data_to_abs(bsi->sb, rel);
		di->extent_inline_base = chain_block;

		bh = sb_getblk(sb, chain_block);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			return -EIO;
		}
		memset(bh->b_data, 0, sb->s_blocksize);
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
	}

	/* Walk to the last chain block */
	while (1) {
		bh = sb_bread(sb, chain_block);
		if (!bh)
			return -EIO;
		chain = (struct briefs_extent_chain *)bh->b_data;

		if (chain->num_extents_in_block < CHAIN_EXTENTS) {
			/* Room in this block */
			slot = chain->num_extents_in_block;
			chain->extents[slot] = *ext;
			chain->num_extents_in_block++;
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);
			di->num_extents_total++;
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
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		/* Set up new chain block */
		bh = sb_getblk(sb, new_block);
		if (!bh) {
			briefs_free_block(&bsi->alloc, rel);
			return -EIO;
		}
		memset(bh->b_data, 0, sb->s_blocksize);
		chain = (struct briefs_extent_chain *)bh->b_data;
		chain->extents[0] = *ext;
		chain->num_extents_in_block = 1;
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);
		di->num_extents_total++;
		return 0;
	}
}


/*
 * briefs_find_block - resolve file block offset to physical block
 * via inline extents and extent chain blocks. Returns 0 if unmapped (hole).
 */
static u64 briefs_find_block(struct inode *inode, u64 file_block)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;
	int i;

	for (i = 0; i < binfo->disk_inode.num_extents_total; i++) {
		if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext) != 0)
			break;
		if (file_block >= ext.offset && file_block < ext.offset + ext.len)
			return ext.phys + (file_block - ext.offset);
	}
	return 0;
}

/*
 * briefs_find_or_create_block - like find_block, but allocates a new
 * physical block if none is mapped and appends a new extent.
 * Returns the ABSOLUTE block number, or 0 on failure.
 * Supports extent chain blocks for overflow beyond 8 inline extents.
 */
static u64 briefs_find_or_create_block(struct inode *inode, u64 file_block)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent ext;
	struct briefs_extent new_ext;
	u64 rel, abs_block;
	int i, ret;

	/* Check existing extents via read_extent (handles inline + chain) */
	for (i = 0; i < binfo->disk_inode.num_extents_total; i++) {
		if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, i, &ext) != 0)
			break;
		if (file_block >= ext.offset && file_block < ext.offset + ext.len)
			return ext.phys + (file_block - ext.offset);
	}

	/* Check if we can extend the last extent */
	if (binfo->disk_inode.num_extents_total > 0) {
		ret = briefs_read_extent(inode->i_sb, &binfo->disk_inode,
		                         binfo->disk_inode.num_extents_total - 1, &ext);
		if (ret == 0 && file_block == ext.offset + ext.len) {
			rel = briefs_alloc_block(&bsi->alloc);
			if (rel == 0)
				return 0;
			abs_block = data_to_abs(bsi->sb, rel);
			if (abs_block == ext.phys + ext.len && ext.len < 0xFFFFFFFF) {
				/* Physically contiguous - extend last extent */
				struct buffer_head *cbh;
				struct briefs_extent_chain *chain;
				u64 chain_block;
				int ci;

				if (binfo->disk_inode.num_extents_total <= binfo->disk_inode.num_extents_inline) {
					/* Last extent is inline */
					int n = binfo->disk_inode.num_extents_inline - 1;
					binfo->disk_inode.inline_extents[n].len++;
				} else {
					/* Last extent is in a chain block */
					chain_block = binfo->disk_inode.extent_inline_base;
					ci = binfo->disk_inode.num_extents_total - 1 - binfo->disk_inode.num_extents_inline;
					while (chain_block) {
						cbh = sb_bread(inode->i_sb, chain_block);
						if (!cbh) break;
						chain = (struct briefs_extent_chain *)cbh->b_data;
						if (ci < chain->num_extents_in_block) {
							chain->extents[ci].len++;
							mark_buffer_dirty(cbh);
							sync_dirty_buffer(cbh);
							brelse(cbh);
							break;
						}
						ci -= chain->num_extents_in_block;
						chain_block = chain->next_overflow_block;
						brelse(cbh);
					}
				}
				return abs_block;
			}
			/* Not contiguous - start a new extent */
			new_ext.offset = file_block;
			new_ext.phys = abs_block;
			new_ext.len = 1;
			new_ext.flags = 0;
			ret = briefs_append_extent(inode->i_sb, &binfo->disk_inode, &new_ext);
			if (ret != 0) return 0;
			return abs_block;
		}
	}

	/* Need a brand new extent */
	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return 0;
	abs_block = data_to_abs(bsi->sb, rel);

	new_ext.offset = file_block;
	new_ext.phys = abs_block;
	new_ext.len = 1;
	new_ext.flags = 0;
	ret = briefs_append_extent(inode->i_sb, &binfo->disk_inode, &new_ext);
	if (ret != 0) return 0;
	return abs_block;
}

/* Read file */
ssize_t briefs_read_iter(struct kiocb *iocb, struct iov_iter *to) {
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(to);
	size_t done = 0;
	ssize_t ret = 0;

	pr_debug("briefs: read_iter pos=%lld count=%zu\n", pos, count);

	if (pos < 0)
		return -EINVAL;
	if (pos >= inode->i_size)
		return 0;
	if (pos + count > inode->i_size)
		count = inode->i_size - pos;

	while (count > 0) {
		u64 file_block = pos / BRIEFS_BLOCK_SIZE;
		u64 offset_in_block = pos % BRIEFS_BLOCK_SIZE;
		u64 phys_block = briefs_find_block(inode, file_block);

		if (phys_block == 0) {
			/* Hole — fill with zeros */
			size_t hole = min(count, BRIEFS_BLOCK_SIZE - offset_in_block);
			if (!iov_iter_zero(hole, to)) {
				ret = -EFAULT;
				break;
			}
			done += hole;
			pos += hole;
			count -= hole;
			continue;
		}

		struct buffer_head *bh = sb_bread(inode->i_sb, phys_block);
		if (!bh) {
			ret = -EIO;
			break;
		}

		size_t chunk = min(count, BRIEFS_BLOCK_SIZE - offset_in_block);
		size_t copied = copy_to_iter(bh->b_data + offset_in_block, chunk, to);
		brelse(bh);

		if (copied == 0) {
			ret = -EFAULT;
			break;
		}

		done += copied;
		pos += copied;
		count -= copied;
	}

	if (done > 0) {
		iocb->ki_pos = pos;
		ret = done;
	}

	return ret;
}

/* Write file */
ssize_t briefs_write_iter(struct kiocb *iocb, struct iov_iter *from) {
	struct inode *inode = file_inode(iocb->ki_filp);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	loff_t pos = iocb->ki_pos;
	size_t count = iov_iter_count(from);
	size_t done = 0;
	ssize_t ret = 0;
	struct timespec64 now;

	pr_debug("briefs: write_iter pos=%lld count=%zu\n", pos, count);

	if (pos < 0)
		return -EINVAL;

	while (count > 0) {
		u64 file_block = pos / BRIEFS_BLOCK_SIZE;
		u64 offset_in_block = pos % BRIEFS_BLOCK_SIZE;
		u64 phys_block = briefs_find_or_create_block(inode, file_block);

		if (phys_block == 0) {
			pr_err("briefs: write_iter: no free block for file_block %llu\n", file_block);
			ret = -ENOSPC;
			break;
		}

		struct buffer_head *bh = sb_bread(inode->i_sb, phys_block);
		if (!bh) {
			ret = -EIO;
			break;
		}

		size_t chunk = min(count, BRIEFS_BLOCK_SIZE - offset_in_block);
		size_t copied = copy_from_iter(bh->b_data + offset_in_block, chunk, from);
		if (copied == 0) {
			brelse(bh);
			ret = -EFAULT;
			break;
		}

		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		done += copied;
		pos += copied;
		count -= copied;
	}

	if (done > 0) {
		iocb->ki_pos = pos;

		if (pos > inode->i_size) {
			inode->i_size = pos;
			binfo->disk_inode.filesize = pos;
		}

		/* Compute i_blocks from all extents (inline + chain) */
		{
			struct briefs_extent ext;
			u64 total_blocks = 0;
			int j;
			for (j = 0; j < binfo->disk_inode.num_extents_total; j++) {
				if (briefs_read_extent(inode->i_sb, &binfo->disk_inode, j, &ext) == 0)
					total_blocks += ext.len;
			}
			inode->i_blocks = total_blocks * (BRIEFS_BLOCK_SIZE / 512);
		}

		ktime_get_real_ts64(&now);
		inode->i_mtime_sec = now.tv_sec;
		inode->i_mtime_nsec = now.tv_nsec;
		binfo->disk_inode.mtime_sec = inode->i_mtime_sec;
		binfo->disk_inode.mtime_nsec = inode->i_mtime_nsec;

		/* Persist inode */
		{
			u64 ino = inode->i_ino;
			u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
			u64 idx = ino - 1;
			u64 blk = idx / (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
			u64 off = (idx % (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
			struct buffer_head *ibh = sb_bread(inode->i_sb, inodeTableBlock + blk);
			if (ibh) {
				struct briefs_inode *di = (struct briefs_inode *)(ibh->b_data + off);
				memcpy(di, &binfo->disk_inode, sizeof(struct briefs_inode));
				mark_buffer_dirty(ibh);
				sync_dirty_buffer(ibh);
				brelse(ibh);
			}
		}

		ret = done;

	}

	pr_debug("briefs: write_iter done=%zu\n", done);
	return ret;
}

/* briefs_fsync - sync file data and metadata to disk */
int briefs_fsync(struct file *file, loff_t start, loff_t end, int datasync) {
	struct inode *inode = file->f_mapping->host;
	int ret;

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		return ret;

	return sync_inode_metadata(inode, 1);
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

/* briefs_write_inode - persist VFS inode to disk */
int briefs_write_inode(struct inode *inode, struct writeback_control *wbc) {
	struct briefs_sb_info *bsi;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	u64 inodeTableBlock, inodeBlock, inodeOffset, inodeIndex;

	if (!inode)
		return -EINVAL;

	bsi = inode->i_sb->s_fs_info;
	binfo = briefs_i(inode);

	pr_debug("briefs: write_inode %lu\n", inode->i_ino);

	/* Calculate inode location */
	inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
	inodeIndex = inode->i_ino - 1;
	inodeBlock = inodeIndex / (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	/* Read the block containing this inode */
	bh = sb_bread(inode->i_sb, inodeTableBlock + inodeBlock);
	if (!bh) {
		pr_err("briefs: failed to read inode block for write_inode %lu\n", inode->i_ino);
		return -EIO;
	}

	disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);

	/* Sync VFS timestamp fields into in-memory copy first */
	binfo->disk_inode.atime_sec = inode->i_atime_sec;
	binfo->disk_inode.atime_nsec = inode->i_atime_nsec;
	binfo->disk_inode.mtime_sec = inode->i_mtime_sec;
	binfo->disk_inode.mtime_nsec = inode->i_mtime_nsec;
	binfo->disk_inode.ctime_sec = inode->i_ctime_sec;
	binfo->disk_inode.ctime_nsec = inode->i_ctime_nsec;

	/* Copy in-memory disk_inode to the on-disk buffer */
	memcpy(disk_inode, &binfo->disk_inode, sizeof(struct briefs_inode));

	/* Update VFS-derived fields */
	disk_inode->filemode = inode->i_mode;
	disk_inode->uid = from_kuid(&init_user_ns, inode->i_uid);
	disk_inode->gid = from_kgid(&init_user_ns, inode->i_gid);
	disk_inode->filesize = inode->i_size;
	disk_inode->nlinks = inode->i_nlink;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

/*
 * briefs_add_dir_entry - insert a directory entry into the directory trie.
 */
int briefs_add_dir_entry(struct inode *dir, const char *name, size_t name_len, u64 child_ino, u8 type) {
	struct briefs_inode_info *binfo;
	int ret;

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);

	if (binfo->disk_inode.dir_trie_root == 0) {
		ret = briefs_trie_create_root(dir->i_sb, &binfo->disk_inode);
		if (ret)
			return ret;
	}

	ret = briefs_trie_insert(dir->i_sb, &binfo->disk_inode,
	                          name, name_len, child_ino, type);
	if (ret == -EEXIST)
		return 0;

	return ret;
}

/*
 * briefs_remove_dir_entry - remove a directory entry from the trie.
 */
int briefs_remove_dir_entry(struct inode *dir, const char *name, size_t name_len)
{
	struct briefs_inode_info *binfo;

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);

	if (binfo->disk_inode.dir_trie_root == 0)
		return -ENOENT;

	return briefs_trie_remove(dir->i_sb, &binfo->disk_inode, name, name_len);
}

/* briefs_fill_super - entry point for mount */
/* Keeping things simple, at least at first. Cribbing off of xiafs_fill_super
 * since it's reasonably simple but gets the job done. */
int briefs_fill_super(struct super_block *sb, void *data, int flags) {
	struct buffer_head *bh;
	struct briefs_superblock *bsb;
	struct inode *root_inode;
	struct briefs_sb_info *bsi;
	int ret = -EINVAL;

	pr_info("briefs: fill_super enter\n");

	bsi = kzalloc(sizeof(struct briefs_sb_info), GFP_KERNEL);
	if (!bsi)
		return -ENOMEM;
	sb->s_fs_info = bsi;

	if (!sb_set_blocksize(sb, 4096)) {
		pr_err("briefs: blocksize too small\n");
		goto out_bad_hblock;
	}

	if (!(bh = sb_bread(sb, 0))) {
		pr_err("briefs: unable to read superblock\n");
		goto out_bad_sb;
	}

	bsb = (struct briefs_superblock *) bh->b_data;

	pr_info("briefs: superblock magic=0x%016llx, data_bitmap_offset=%llu, data_bitmap_blocks=%llu\n", bsb->magic, bsb->data_bitmap_offset, bsb->data_bitmap_blocks);

	sb->s_magic = bsb->magic;

	pr_info("briefs: magic=0x%08lx\n", sb->s_magic);

	if (sb->s_magic != _BRIEFS_SUPER_MAGIC) {
		pr_err("briefs: invalid magic\n");
		sb->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

	bsi->sb = bsb;

	/* Initialize data block allocator */
	ret = briefs_alloc_init(&bsi->alloc, sb, bsb);
	if (ret) {
		pr_err("briefs: failed to initialize data block allocator: %d\n", ret);
		goto out_no_root;
	}

	/* Initialize inode allocator */
	ret = briefs_alloc_init_at(&bsi->inode_alloc, sb, bsb->inode_bitmap_offset);
	if (ret) {
		pr_err("briefs: failed to initialize inode allocator: %d\n", ret);
		goto out_no_root;
	}

	/* Initialize journal */
	bsi->journal = kzalloc(sizeof(struct briefs_journal), GFP_KERNEL);
	if (!bsi->journal) {
		ret = -ENOMEM;
		goto out_no_journal;
	}

	ret = briefs_journal_open(bsi->journal, bsb, sb);
	if (ret) {
		pr_err("briefs: failed to initialize journal: %d\n", ret);
		goto out_no_journal;
	}

	/* Replay journal on mount (if not clean) */
	ret = briefs_journal_replay(bsi->journal);
	if (ret) {
		pr_err("briefs: journal replay failed: %d\n", ret);
		goto out_no_journal;
	}

	sb->s_op = &briefs_super_ops;

	root_inode = briefs_iget(sb, _BRIEFS_ROOT_INO);
	if (IS_ERR(root_inode)) {
		pr_err("BrieFS: error getting root inode.\n");
		ret = PTR_ERR(root_inode);
		goto out_no_root;
	}

	ret = -ENOMEM;

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		pr_err("briefs: d_make_root failed\n");
		goto out_iput;
	}

	bsi->data_blocks = bsb->data_blocks;
	bsi->free_data_blocks = bsb->free_data_blocks;
	/* Compute inode counts from the inode allocator pyramid */
	bsi->num_inodes = bsi->inode_alloc.block_count;
	bsi->free_inodes = bsi->inode_alloc.free_count;

	pr_info("briefs: superblock loaded, mounting successful\n");

	if (!sb_rdonly(sb))
		mark_buffer_dirty(bh);

	return 0;

	out_no_journal:
	pr_err("briefs: journal init failed.\n");
	if (bsi->journal) {
		briefs_journal_cleanup(bsi->journal);
		kfree(bsi->journal);
		bsi->journal = NULL;
	}
	goto out_release;

out_no_root:
	pr_err("briefs: get root inode failed.\n");
	if (bsi->journal) {
		briefs_journal_cleanup(bsi->journal);
		kfree(bsi->journal);
		bsi->journal = NULL;
	}
	goto out_release;

out_iput:
	iput(root_inode);
	if (bsi->journal) {
		briefs_journal_cleanup(bsi->journal);
		kfree(bsi->journal);
		bsi->journal = NULL;
	}
	goto out_release;

out_no_fs:
	pr_err("VFS: Can't find a BrieFS filesystem on device %s.\n", sb->s_id);

out_release:
	brelse(bh);
	goto out;

out_bad_hblock:
	pr_err("BrieFS: blocksize too small for device.\n");
	goto out;

out_bad_sb:
	pr_err("BrieFS: unable to read superblock.\n");

out:
	/* Clean up allocators if initialized */
	if (bsi) {
		if (bsi->alloc.l0)
			briefs_alloc_cleanup(&bsi->alloc);
		if (bsi->inode_alloc.l0)
			briefs_alloc_cleanup(&bsi->inode_alloc);
	}
	sb->s_fs_info = NULL;
	kfree(bsi);
	pr_info("briefs: fill_super returning %d\n", ret);
	return ret;
}

/* briefs_alloc_inode - allocate a VFS inode (called by VFS inode cache) */
struct inode *briefs_alloc_vfs_inode(struct super_block *sb) {
	struct briefs_inode_info *binfo;

	binfo = alloc_inode_sb(sb, briefs_inode_cachep, GFP_KERNEL);
	if (!binfo)
		return NULL;

	return &binfo->vfs_inode;
}

/* briefs_free_inode - free a VFS inode (called by VFS inode cache) */
void briefs_free_inode(struct inode *inode) {
	struct briefs_inode_info *binfo = briefs_i(inode);
	kmem_cache_free(briefs_inode_cachep, binfo);
}

/* briefs_iget - get an inode by number */
struct inode *briefs_iget(struct super_block *sb, u64 ino) {
	struct inode *inode;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	struct briefs_sb_info *bsi;
	u64 inodeTableBlock;
	u64 inodeBlock;
	u64 inodeOffset;

	pr_debug("briefs: iget inode %llu\n", ino);

	bsi = sb->s_fs_info;
	if (!bsi || !bsi->sb) {
		pr_err("briefs: no sb_info for ino %llu\n", ino);
		return ERR_PTR(-EIO);
	}

	/* Calculate inode location: inode table follows data bitmap */
	inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
	/* Inode table starts at inodeTableBlock. Each block holds 8 inodes (512 bytes each).
	 * inodeIndex is the 0-based index into the inode table.
	 * inodeBlock is the block offset within the inode table.
	 * inodeOffset is the byte offset within that block.
	 */
	u64 inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	inode = iget_locked(sb, ino);
	if (!inode) {
		pr_err("briefs: iget_locked failed\n");
		return ERR_PTR(-ENOMEM);
	}

	if (inode->i_state & I_NEW) {
		/* Recompute inode location inside the locked block */
		u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
		u64 inodeIndex = ino - 1;
		inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
		inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
		pr_info("briefs: data_bitmap_offset=%llu, data_bitmap_blocks=%llu, inodeTableBlock=%llu, inodeIndex=%llu, inodeBlock=%llu, inodeOffset=%llu\n",
			bsi->sb->data_bitmap_offset, bsi->sb->data_bitmap_blocks, inodeTableBlock, inodeIndex, inodeBlock, inodeOffset);
		pr_info("briefs: reading inode %llu from block %llu (inodeTableBlock=%llu, inodeBlock=%llu)\n",
			ino, inodeTableBlock + inodeBlock, inodeTableBlock, inodeBlock);
		/* Read inode from disk */
		bh = sb_bread(sb, inodeTableBlock + inodeBlock);
		if (!bh) {
			pr_err("briefs: unable to read inode block for ino %llu\n", ino);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EIO);
		}

		disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);

		if (disk_inode->magic != 0x494E4F44) {
			pr_err("briefs: invalid inode magic for ino %llu: 0x%08llx\n", ino, disk_inode->magic);
			brelse(bh);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(-EINVAL);
		}

		/* Copy disk inode to VFS inode */
		binfo = (struct briefs_inode_info *)inode;
		memcpy(&binfo->disk_inode, disk_inode, sizeof(struct briefs_inode));
		binfo->inode_number = ino;

		/* Set VFS inode fields from disk inode */
		inode->i_mode = disk_inode->filemode;
		inode->i_uid = make_kuid(&init_user_ns, disk_inode->uid);
		inode->i_gid = make_kgid(&init_user_ns, disk_inode->gid);
		inode->i_size = disk_inode->filesize;
		inode->i_blocks = briefs_compute_i_blocks(disk_inode);

		set_nlink(inode, disk_inode->nlinks);

		inode->i_atime_sec = disk_inode->atime_sec;
		inode->i_atime_nsec = disk_inode->atime_nsec;
		inode->i_mtime_sec = disk_inode->mtime_sec;
		inode->i_mtime_nsec = disk_inode->mtime_nsec;
		inode->i_ctime_sec = disk_inode->ctime_sec;
		inode->i_ctime_nsec = disk_inode->ctime_nsec;

		/* Set VFS operations based on inode type */
		if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &briefs_dir_inode_ops;
			inode->i_fop = &briefs_dir_operations;
		} else if (S_ISREG(inode->i_mode)) {
			inode->i_op = &briefs_file_inode_ops;
			inode->i_fop = &briefs_file_operations;
		}

		pr_info("briefs: inode %llu: mode=0x%04x, uid=%u, gid=%u, size=%llu, nlink=%u\n",
			ino, inode->i_mode, from_kuid(&init_user_ns, inode->i_uid),
			from_kgid(&init_user_ns, inode->i_gid), inode->i_size, inode->i_nlink);

		brelse(bh);
		unlock_new_inode(inode);
	}

	return inode;
}

/* briefs_put_super - cleanup superblock */
void briefs_put_super(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;

	pr_info("briefs: put_super enter\n");

	if (bsi) {
		/* Sync journal before unmount */
		if (bsi->journal && bsi->journal->dirty) {
			briefs_journal_checkpoint(bsi->journal);
		}

		/* Sync allocation trie to disk */
		if (!sb_rdonly(sb)) {
			pr_info("briefs: syncing allocation trie\n");
			briefs_alloc_sync(&bsi->alloc);

			/* Update superblock free counts on disk */
			{
				struct buffer_head *sbh = sb_bread(sb, 0);
				if (sbh) {
					struct briefs_superblock *bsb = (struct briefs_superblock *)sbh->b_data;
					bsb->free_data_blocks = bsi->alloc.free_count;
					bsb->free_inodes = bsi->inode_alloc.free_count;
					mark_buffer_dirty(sbh);
					sync_dirty_buffer(sbh);
					brelse(sbh);
				}
			}
		}

		pr_info("briefs: cleaning up journal\n");
		if (bsi->journal) {
			briefs_journal_cleanup(bsi->journal);
			kfree(bsi->journal);
			bsi->journal = NULL;
		}

		pr_info("briefs: syncing inode allocator\n");
		briefs_alloc_sync(&bsi->inode_alloc);

		pr_info("briefs: calling alloc_cleanup\n");
		briefs_alloc_cleanup(&bsi->alloc);
		briefs_alloc_cleanup(&bsi->inode_alloc);
		pr_info("briefs: alloc_cleanup done\n");
	}

	pr_info("briefs: put_super\n");
}

/* briefs_statfs - filesystem statistics */
/* shamelessly yoinking from xiafs - incomplete */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct briefs_sb_info *sbi = briefs_sb(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->alloc.block_count;
	buf->f_bfree = sbi->alloc.free_count;
	buf->f_bavail = sbi->alloc.free_count;
	buf->f_files = sbi->inode_alloc.block_count;
	buf->f_ffree = sbi->inode_alloc.free_count;
	buf->f_namelen = BRIEFS_NAME_LEN;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
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
			u64 b;

			for (b = 0; b < blocks_to_free; b++) {
				u64 abs = ext.phys + ext.len - blocks_to_free + b;
				u64 rel = abs_to_data(bsi->sb, abs);
				briefs_free_block(&bsi->alloc, rel);
			}

			ext.len -= blocks_to_free;

			/* Update the extent in place (inline or chain) */
			if (i < binfo->disk_inode.num_extents_inline) {
				binfo->disk_inode.inline_extents[i] = ext;
			} else {
				ci = i - binfo->disk_inode.num_extents_inline;
				chain_block = binfo->disk_inode.extent_inline_base;
				while (chain_block && ci >= 0) {
					bh = sb_bread(inode->i_sb, chain_block);
					if (!bh) break;
					chain = (struct briefs_extent_chain *)bh->b_data;
					if (ci < chain->num_extents_in_block) {
						chain->extents[ci] = ext;
						mark_buffer_dirty(bh);
						sync_dirty_buffer(bh);
						brelse(bh);
						break;
					}
					ci -= chain->num_extents_in_block;
					chain_block = chain->next_overflow_block;
					brelse(bh);
				}
			}
			/* Done - all later extents already removed */
			break;
		}

		/* trunc_block <= ext_start - free the entire extent */
		{
			u64 b;
			for (b = 0; b < ext.len; b++) {
				u64 abs = ext.phys + b;
				u64 rel = abs_to_data(bsi->sb, abs);
				briefs_free_block(&bsi->alloc, rel);
			}
		}

		/* Remove the extent: shift remaining extents down */
		if (i < binfo->disk_inode.num_extents_inline) {
			/* Extent is inline - remove it */
			int j;
			for (j = i; j < binfo->disk_inode.num_extents_inline - 1; j++)
				binfo->disk_inode.inline_extents[j] = binfo->disk_inode.inline_extents[j + 1];
			binfo->disk_inode.num_extents_inline--;
		} else {
			/* Extent is in a chain block - we'll rebuild later */
		}

		binfo->disk_inode.num_extents_total--;
	}

	/* If trunc_block is 0 (truncate to empty), use the full cleanup path */
	if (new_size == 0) {
		/* Free all remaining extents (they're all before trunc_block=0) and chain blocks */
		chain_block = binfo->disk_inode.extent_inline_base;
		while (chain_block) {
			bh = sb_bread(inode->i_sb, chain_block);
			if (!bh) break;
			chain = (struct briefs_extent_chain *)bh->b_data;
			u64 next = chain->next_overflow_block;
			brelse(bh);
			briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, chain_block));
			chain_block = next;
		}
		binfo->disk_inode.extent_inline_base = 0;
		binfo->disk_inode.num_extents_inline = 0;
		binfo->disk_inode.num_extents_total = 0;
		memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
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
	{
		u64 ino = inode->i_ino;
		u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
		u64 idx = ino - 1;
		u64 blk = idx / (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
		u64 off = (idx % (inode->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
		struct buffer_head *ibh = sb_bread(inode->i_sb, inodeTableBlock + blk);
		if (ibh) {
			struct briefs_inode *di = (struct briefs_inode *)(ibh->b_data + off);
			memcpy(di, &binfo->disk_inode, sizeof(struct briefs_inode));
			mark_buffer_dirty(ibh);
			sync_dirty_buffer(ibh);
			brelse(ibh);
		}
	}

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

/* briefs_free_inode_data - free all data blocks owned by an inode */
static void briefs_free_inode_data(struct inode *inode)
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
		for (b = 0; b < ext.len; b++) {
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
		next_chain = chain->next_overflow_block;
		brelse(bh);
		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, chain_block));
		chain_block = next_chain;
	}

	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
	binfo->disk_inode.extent_inline_base = 0;
	memset(binfo->disk_inode.inline_extents, 0, sizeof(binfo->disk_inode.inline_extents));
}

/* briefs_evict_inode - cleanup inode on eviction */
void briefs_evict_inode(struct inode *inode) {
	pr_debug("briefs: evict_inode inode %lu\n", inode->i_ino);
	/* When nlink drops to 0, free allocated blocks and the inode number */
	if (inode->i_nlink == 0) {
		briefs_free_inode_data(inode);
		briefs_free_inode_num(inode->i_sb, inode->i_ino);
	}
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/* briefs_umount_begin - called before unmount */
void briefs_umount_begin(struct super_block *sb) {
	pr_info("briefs: umount_begin\n");
}

/* briefs_kill_sb - called when sb is being destroyed */
void briefs_kill_sb(struct super_block *sb) {
	pr_info("briefs: kill_sb\n");
	if (sb) {
		pr_info("briefs: killing sb %p\n", sb);
	}
	kill_block_super(sb);
}

/* briefs_mount - mount callback */
struct dentry *briefs_mount(struct file_system_type *fs_type, int flags,
                           const char *dev_name, void *data) {
	pr_info("briefs: mount callback\n");
	return mount_bdev(fs_type, flags, dev_name, data, briefs_fill_super);
}

/* briefs_alloc_inode - allocate a new inode number using the bitmap pyramid */
static u64 briefs_alloc_inode(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 inum = briefs_alloc_block(&bsi->inode_alloc);
	if (inum == 0) {
		pr_err("briefs: no free inodes\n");
		return 0;
	}
	/* inum is 0-based index, convert to 1-based inode number */
	u64 ino = inum + 1;
	pr_debug("briefs: allocated inode %llu\n", ino);
	return ino;
}

static void briefs_free_inode_num(struct super_block *sb, u64 ino) {
	struct briefs_sb_info *bsi = sb->s_fs_info;
	if (ino == 0)
		return;
	briefs_free_block(&bsi->inode_alloc, ino - 1);
	pr_debug("briefs: freed inode %llu\n", ino);
}

/* briefs_lookup - find inode by name in directory trie */
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
	struct briefs_inode_info *binfo;
	const char *name;
	unsigned int name_len;
	u64 found_ino;
	u8 found_type;
	int ret;
	
	if (!dir || !S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);
	
	name = dentry->d_name.name;
	name_len = dentry->d_name.len;
	
	pr_debug("briefs: trie lookup for %pd in dir inode %lu\n", dentry, dir->i_ino);
	
	/* Check for . and .. */
	if (name_len == 1 && name[0] == '.') {
		igrab(dir);
		return d_splice_alias(dir, dentry);
	}
	if (name_len == 2 && name[0] == '.' && name[1] == '.') {
		if (dir->i_ino == _BRIEFS_ROOT_INO) {
			igrab(dir);
			return d_splice_alias(dir, dentry);
		}
		binfo = briefs_i(dir);
		if (binfo->disk_inode.parent_inode == 0)
			return ERR_PTR(-EIO);
		{
			struct inode *parent = briefs_iget(dir->i_sb, binfo->disk_inode.parent_inode);
			if (IS_ERR(parent))
				return ERR_CAST(parent);
			return d_splice_alias(parent, dentry);
		}
	}
	
	/* Search the directory trie */
	binfo = briefs_i(dir);
	ret = briefs_trie_lookup(dir->i_sb, &binfo->disk_inode, name, name_len, &found_ino, &found_type);
	if (ret == -ENOENT)
		return NULL;
	if (ret != 0)
		return ERR_PTR(ret);
	
	{
		struct inode *inode = briefs_iget(dir->i_sb, found_ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
		return d_splice_alias(inode, dentry);
	}
}

/* briefs_create - create a new file or directory */
int briefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	struct briefs_sb_info *bsi;
	struct briefs_superblock *sbk;
	struct inode *inode;
	struct buffer_head *bh;
	struct briefs_inode *disk_inode;
	u64 ino;
	u64 inodeTableBlock, inodeBlock, inodeOffset, inodeIndex;
	int ret;
	bool is_dir = S_ISDIR(mode);
	struct timespec64 now;

	pr_debug("briefs: create %pd (mode=%o) in dir %lu\n", dentry, mode, dir->i_ino);

	bsi = dir->i_sb->s_fs_info;
	sbk = bsi->sb;

	/* Allocate new inode */
	ino = briefs_alloc_inode(dir->i_sb);
	if (ino == 0)
		return -ENOSPC;

	/* Log inode allocation */
	if (bsi->journal) {
		struct jrn_inode_alloc irec;
		memset(&irec, 0, sizeof(irec));
		irec.ino = ino;
		irec.mode = mode & 07777;
		irec.nlink = is_dir ? 2 : 1;
		ret = briefs_journal_write_record(bsi->journal, JRN_INODE_ALLOC, &irec, sizeof(irec));
		if (ret) return ret;
	}

	/* Log directory entry */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, dir->i_ino, ino,
						dentry->d_name.name, dentry->d_name.len, 0);
		if (ret) return ret;
	}

	/* Create new inode — use iget_locked directly, skip disk read for new inodes */
	inode = iget_locked(dir->i_sb, ino);
	if (!inode)
		return -ENOMEM;

	if (inode->i_state & I_NEW) {
		/* Set up the inode without reading from disk */
		struct briefs_inode_info *binfo = briefs_i(inode);
		memset(&binfo->disk_inode, 0, sizeof(struct briefs_inode));
		binfo->inode_number = ino;

		inode->i_mode = mode;
		inode->i_uid = current_fsuid();
		inode->i_gid = current_fsgid();
		inode->i_size = 0;
		inode->i_blocks = briefs_compute_i_blocks(&binfo->disk_inode);
		set_nlink(inode, is_dir ? 2 : 1);

		ktime_get_real_ts64(&now);
		inode->i_atime_sec = inode->i_mtime_sec = inode->i_ctime_sec = now.tv_sec;
		inode->i_atime_nsec = inode->i_mtime_nsec = inode->i_ctime_nsec = now.tv_nsec;

		/* Set up briefs_inode fields */
		binfo->disk_inode.inode_number = ino;
		binfo->disk_inode.magic = 0x494E4F44;
		binfo->disk_inode.filemode = mode;
		binfo->disk_inode.uid = from_kuid(&init_user_ns, inode->i_uid);
		binfo->disk_inode.gid = from_kgid(&init_user_ns, inode->i_gid);
		binfo->disk_inode.filesize = 0;
		binfo->disk_inode.nlinks = is_dir ? 2 : 1;
		binfo->disk_inode.num_extents_inline = 0;
		binfo->disk_inode.num_extents_total = 0;
		binfo->disk_inode.atime_sec = inode->i_atime_sec;
		binfo->disk_inode.atime_nsec = inode->i_atime_nsec;
		binfo->disk_inode.mtime_sec = inode->i_mtime_sec;
		binfo->disk_inode.mtime_nsec = inode->i_mtime_nsec;
		binfo->disk_inode.ctime_sec = inode->i_ctime_sec;
		binfo->disk_inode.ctime_nsec = inode->i_ctime_nsec;
		binfo->disk_inode.creation_time_sec = inode->i_ctime_sec;
		binfo->disk_inode.creation_time_nsec = inode->i_ctime_nsec;

		if (is_dir) {
			inode->i_op = &briefs_dir_inode_ops;
			inode->i_fop = &briefs_dir_operations;
		} else {
			inode->i_op = &briefs_file_inode_ops;
			inode->i_fop = &briefs_file_operations;
		}

		unlock_new_inode(inode);
	} else {
		/* Inode already in cache — shouldn't happen for fresh alloc */
		iput(inode);
		return -EEXIST;
	}

	/* For directories: create the directory trie root */
	if (is_dir) {
		struct briefs_inode_info *binfo = briefs_i(inode);

		ret = briefs_trie_create_root(dir->i_sb, &binfo->disk_inode);
		if (ret) {
			pr_err("briefs: failed to create dir trie root for ino %llu\n", ino);
			iput(inode);
			return ret;
		}

		pr_debug("briefs: created dir trie root block=%llu for inode %llu\n",
			binfo->disk_inode.dir_trie_root, ino);
	}

	/* Write the inode to disk */
	inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
	inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	bh = sb_bread(dir->i_sb, inodeTableBlock + inodeBlock);
	if (!bh) {
		pr_err("briefs: failed to read inode block for create %llu\n", ino);
		iput(inode);
		return -EIO;
	}

	disk_inode = (struct briefs_inode *)(bh->b_data + inodeOffset);
	memcpy(disk_inode, &briefs_i(inode)->disk_inode, sizeof(struct briefs_inode));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	/* Add directory entry to parent */
	u8 ftype = (mode & S_IFMT) >> 12;
	ret = briefs_add_dir_entry(dir, dentry->d_name.name, dentry->d_name.len, ino, ftype);
	if (ret) {
		pr_err("briefs: failed to add dir entry %pd: %d\n", dentry, ret);
		iput(inode);
		return ret;
	}

	/* Update parent inode */
	{
		struct briefs_inode_info *pbinfo = briefs_i(dir);
		if (is_dir)
			inc_nlink(dir);
		dir->i_size += sizeof(struct briefs_dir_entry) + 2 + dentry->d_name.len;
		pbinfo->disk_inode.filesize = dir->i_size;
		pbinfo->disk_inode.nlinks = dir->i_nlink;
	}

	/* Write parent inode */
	{u64 pIno = dir->i_ino;
	u64 pIdx = pIno - 1;
	u64 pBlk = pIdx / (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
	u64 pOff = (pIdx % (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
	struct buffer_head *pbh = sb_bread(dir->i_sb, inodeTableBlock + pBlk);
	if (pbh) {
		struct briefs_inode *pdi = (struct briefs_inode *)(pbh->b_data + pOff);
		pdi->filesize = dir->i_size;
		pdi->nlinks = dir->i_nlink;
		mark_buffer_dirty(pbh);
		sync_dirty_buffer(pbh);
		brelse(pbh);
	}}

	/* Sync journal */
	if (bsi->journal && bsi->journal->dirty) {
		ret = briefs_journal_sync(bsi->journal);
		if (ret) {
			iput(inode);
			return ret;
		}
	}

	d_instantiate(dentry, inode);

	pr_debug("briefs: created inode %llu, added to dir\n", ino);
	return 0;
}

/* briefs_mkdir - create a new directory */
int briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
	/* Directories at least aren't getting the proper mode set. Let's do
	 * so now, although we may want to split directories and non-directories
	 * into their own functions. */
	mode |= S_IFDIR;
	if (dir->i_mode & S_ISGID)
		mode |= S_ISGID;

	pr_debug("briefs: mkdir %pd (mode=%o)\n", dentry, mode);
	return briefs_create(idmap, dir, dentry, mode, false);
}

/* briefs_unlink - remove a directory entry */
int briefs_unlink(struct inode *dir, struct dentry *dentry) {
	struct briefs_sb_info *bsi;
	struct inode *inode = d_inode(dentry);
	int ret;

	pr_debug("briefs: unlink %pd\n", dentry);

	if (!inode)
		return -ENOENT;

	bsi = dir->i_sb->s_fs_info;

	/* Log directory entry deletion */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, dir->i_ino, 0,
						dentry->d_name.name, dentry->d_name.len, 1);
		if (ret) return ret;
	}

	/* Remove the directory entry */
	ret = briefs_remove_dir_entry(dir, dentry->d_name.name, dentry->d_name.len);
	if (ret)
		return ret;

	/* Drop nlink on the inode */
	if (S_ISDIR(inode->i_mode)) {
		/* Directory: clear nlink completely (2 -> 0) */
		clear_nlink(inode);
	} else {
		drop_nlink(inode);
	}

	/* If directory, also drop parent's nlink (.. reference) */
	if (S_ISDIR(inode->i_mode))
		drop_nlink(dir);

	/* Update parent inode on disk */
	{
		u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
		u64 pIdx = dir->i_ino - 1;
		u64 pBlk = pIdx / (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE);
		u64 pOff = (pIdx % (dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;
		struct buffer_head *pbh = sb_bread(dir->i_sb, inodeTableBlock + pBlk);
		if (pbh) {
			struct briefs_inode *pdi = (struct briefs_inode *)(pbh->b_data + pOff);
			pdi->nlinks = dir->i_nlink;
			mark_buffer_dirty(pbh);
			sync_dirty_buffer(pbh);
			brelse(pbh);
		}
	}

	/* Sync journal */
	if (bsi->journal && bsi->journal->dirty) {
		ret = briefs_journal_sync(bsi->journal);
		if (ret) return ret;
	}

	pr_debug("briefs: unlinked %pd\n", dentry);
	return 0;
}

/* briefs_rename - rename a directory entry */
int briefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
	struct briefs_sb_info *bsi;
	struct inode *inode = d_inode(old_dentry);
	int ret;

	pr_debug("briefs: rename %pd -> %pd\n", old_dentry, new_dentry);

	if (!inode)
		return -ENOENT;

	bsi = old_dir->i_sb->s_fs_info;

	/* Log old entry deletion */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, old_dir->i_ino, 0,
						old_dentry->d_name.name, old_dentry->d_name.len, 1);
		if (ret) return ret;
	}

	/* Log new entry creation */
	if (bsi->journal) {
		ret = briefs_journal_dir_update(bsi->journal, new_dir->i_ino, inode->i_ino,
						new_dentry->d_name.name, new_dentry->d_name.len, 0);
		if (ret) return ret;
	}

	/* Remove old entry */
	ret = briefs_remove_dir_entry(old_dir, old_dentry->d_name.name, old_dentry->d_name.len);
	if (ret)
		return ret;

	/* Add new entry */
	u8 ftype = (inode->i_mode & S_IFMT) >> 12;
	ret = briefs_add_dir_entry(new_dir, new_dentry->d_name.name, new_dentry->d_name.len,
				   inode->i_ino, ftype);
	if (ret)
		return ret;

	/* Handle cross-directory rename: update nlink */
	if (old_dir != new_dir) {
		if (S_ISDIR(inode->i_mode)) {
			drop_nlink(old_dir);
			inc_nlink(new_dir);
		}
	}

	/* Update parent inodes on disk */
	{
		u64 inodeTableBlock = bsi->sb->data_bitmap_offset + bsi->sb->data_bitmap_blocks;
		u64 inodes_per_block = old_dir->i_sb->s_blocksize / BRIEFS_INODE_SIZE;

		/* Write old_dir */
		{
			u64 idx = old_dir->i_ino - 1;
			u64 blk = idx / inodes_per_block;
			u64 off = (idx % inodes_per_block) * BRIEFS_INODE_SIZE;
			struct buffer_head *pbh = sb_bread(old_dir->i_sb, inodeTableBlock + blk);
			if (pbh) {
				struct briefs_inode *pdi = (struct briefs_inode *)(pbh->b_data + off);
				pdi->nlinks = old_dir->i_nlink;
				mark_buffer_dirty(pbh);
				sync_dirty_buffer(pbh);
				brelse(pbh);
			}
		}

		/* Write new_dir if different */
		if (old_dir != new_dir) {
			u64 idx = new_dir->i_ino - 1;
			u64 blk = idx / inodes_per_block;
			u64 off = (idx % inodes_per_block) * BRIEFS_INODE_SIZE;
			struct buffer_head *pbh = sb_bread(new_dir->i_sb, inodeTableBlock + blk);
			if (pbh) {
				struct briefs_inode *pdi = (struct briefs_inode *)(pbh->b_data + off);
				pdi->nlinks = new_dir->i_nlink;
				mark_buffer_dirty(pbh);
				sync_dirty_buffer(pbh);
				brelse(pbh);
			}
		}
	}

	/* Sync journal */
	if (bsi->journal && bsi->journal->dirty) {
		ret = briefs_journal_sync(bsi->journal);
		if (ret) return ret;
	}

	pr_debug("briefs: renamed %pd -> %pd\n", old_dentry, new_dentry);
	return 0;
}
