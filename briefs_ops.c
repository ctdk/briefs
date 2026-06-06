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
static inline u64 data_region_start(struct briefs_superblock *sb)
{
	return sb->trie_node_pool_start + sb->trie_blocks_used;
}

/* Convert a data-relative block to an absolute block number */

/* Convert a data-relative block to an absolute block number */
static inline u64 data_to_abs(struct briefs_superblock *sb, u64 rel_block)
{
	return data_region_start(sb) + rel_block;
}

/* Convert an absolute block number back to data-relative (inverse of data_to_abs) */
static inline u64 abs_to_data(struct briefs_superblock *sb, u64 abs_block)
{
	return abs_block - data_region_start(sb);
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
	.setattr = simple_setattr,
	.getattr = simple_getattr,
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
	struct buffer_head *bh;
	struct briefs_dir_block *dir_block;
	struct briefs_dir_entry *entry;
	u64 dir_block_num;
	int i;
	
	pr_debug("briefs: readdir offset=%llu\n", ctx->pos);
	
	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;
	
	/* Emit . and .. via dir_emit_dots (handles both) */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return -EIO;
		ctx->pos = 2;
	}
	
	/* Get directory data block from inline extent */
	binfo = briefs_i(dir);
	if (binfo->disk_inode.num_extents_inline == 0)
		return 0;
	
	dir_block_num = binfo->disk_inode.inline_extents[0].phys;
	
	bh = sb_bread(dir->i_sb, dir_block_num);
	if (!bh) {
		pr_err("briefs: failed to read dir block %llu for readdir\n", dir_block_num);
		return 0;
	}
	
	dir_block = (struct briefs_dir_block *)bh->b_data;
	if (dir_block->magic != BRIEFS_DIR_MAGIC) {
		pr_err("briefs: invalid dir block magic in readdir: 0x%08x\n", dir_block->magic);
		brelse(bh);
		return 0;
	}
	
	/*
	 * Scan directory entries, mapping ctx->pos to on-disk array index.
	 *
	 * ctx->pos 0-1 are . and .. handled by dir_emit_dots above.
	 * ctx->pos 2 -> on-disk entry index 0 (first real entry after . and ..)
	 * ctx->pos 3 -> on-disk entry index 1, etc.
	 *
	 * ctx->pos advances monotonically even for deleted entries so the VFS
	 * can seek() back to the right position after revalidating.
	 */
	u64 start_idx = ctx->pos > 2 ? ctx->pos : 2;
	for (i = start_idx; i < dir_block->num_entries; i++) {
		entry = (struct briefs_dir_entry *)(bh->b_data + sizeof(struct briefs_dir_block) +
			                                    i * sizeof(struct briefs_dir_entry));

		/* Skip deleted entries but advance ctx->pos so accounting doesn't break */
		if (entry->name_len == 0 || entry->name_off == 0 ||
		    entry->name_off > BRIEFS_BLOCK_SIZE) {
			ctx->pos++;
			continue;
		}

		char *entry_name = bh->b_data + BRIEFS_BLOCK_SIZE - entry->name_off + 2;
		unsigned int file_type = (entry->type << 12) & S_IFMT;
		
		if (!dir_emit(ctx, entry_name, entry->name_len,
		              entry->inode, fs_umode_to_dtype(file_type))) {
			brelse(bh);
			return 0;
		}
		
		ctx->pos++;
	}
	
	brelse(bh);
	return 0;
}

/*
 * briefs_find_block - resolve file block offset to physical block
 * via inline extents. Returns 0 if unmapped (hole).
 */
static u64 briefs_find_block(struct inode *inode, u64 file_block)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	int i;

	for (i = 0; i < binfo->disk_inode.num_extents_inline; i++) {
		struct briefs_extent *ext = &binfo->disk_inode.inline_extents[i];
		if (file_block >= ext->offset && file_block < ext->offset + ext->len)
			return ext->phys + (file_block - ext->offset);
	}
	return 0;
}

/*
 * briefs_find_or_create_block - like find_block, but allocates a new
 * physical block if none is mapped and appends a new extent.
 * Returns the ABSOLUTE block number, or 0 on failure.
 */
static u64 briefs_find_or_create_block(struct inode *inode, u64 file_block)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_extent *ext;
	int i;

	/* Check existing extents (extent phys values are absolute) */
	for (i = 0; i < binfo->disk_inode.num_extents_inline; i++) {
		ext = &binfo->disk_inode.inline_extents[i];
		if (file_block >= ext->offset && file_block < ext->offset + ext->len)
			return ext->phys + (file_block - ext->offset);
	}

	/* Check if we can extend the last extent */
	if (binfo->disk_inode.num_extents_inline > 0) {
		ext = &binfo->disk_inode.inline_extents[binfo->disk_inode.num_extents_inline - 1];
		if (file_block == ext->offset + ext->len) {
			u64 rel = briefs_alloc_block(&bsi->alloc);
			if (rel == 0)
				return 0;
			u64 abs = data_to_abs(bsi->sb, rel);
			if (abs == ext->phys + ext->len && ext->len < 0xFFFFFFFF) {
				/* Physically contiguous — extend */
				ext->len++;
				return abs;
			}
			/* Not contiguous — start a new extent */
			if (binfo->disk_inode.num_extents_inline < 8) {
				int n = binfo->disk_inode.num_extents_inline;
				binfo->disk_inode.inline_extents[n].offset = file_block;
				binfo->disk_inode.inline_extents[n].phys = abs;
				binfo->disk_inode.inline_extents[n].len = 1;
				binfo->disk_inode.num_extents_inline++;
				binfo->disk_inode.num_extents_total++;
				return abs;
			}
			return 0;
		}
	}

	/* Need a brand new extent */
	if (binfo->disk_inode.num_extents_inline >= 8)
		return 0; /* overflow not yet supported */

	u64 rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return 0;
	u64 abs_block = data_to_abs(bsi->sb, rel);

	int n = binfo->disk_inode.num_extents_inline;
	binfo->disk_inode.inline_extents[n].offset = file_block;
	binfo->disk_inode.inline_extents[n].phys = abs_block;
	binfo->disk_inode.inline_extents[n].len = 1;
	binfo->disk_inode.inline_extents[n].flags = 0;
	binfo->disk_inode.num_extents_inline++;
	binfo->disk_inode.num_extents_total++;
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

		inode->i_blocks = 0; /* TODO: compute from extents */
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
 * briefs_add_dir_entry - append a directory entry to a parent directory.
 *
 * Reads the directory's data block, appends a new DirEntry, and packs the
 * name into the trailing name region.
 */
int briefs_add_dir_entry(struct inode *dir, const char *name, size_t name_len, u64 child_ino, u8 type) {
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_dir_block *dir_block;
	struct briefs_dir_entry *entry;
	u64 dir_block_num;
	int i, entry_off;
	u32 entry_name_len = (u32)name_len;
	u32 hdr_size = sizeof(struct briefs_dir_block);
	u32 entry_sz = sizeof(struct briefs_dir_entry);

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);

	if (binfo->disk_inode.num_extents_inline == 0) {
		pr_err("briefs: dir inode %lu has no extents\n", dir->i_ino);
		return -EIO;
	}

	dir_block_num = binfo->disk_inode.inline_extents[0].phys;

	bh = sb_bread(dir->i_sb, dir_block_num);
	if (!bh) {
		pr_err("briefs: failed to read dir block %llu\n", dir_block_num);
		return -EIO;
	}

	dir_block = (struct briefs_dir_block *)bh->b_data;
	if (dir_block->magic != BRIEFS_DIR_MAGIC) {
		pr_err("briefs: invalid dir block magic: 0x%08x\n", dir_block->magic);
		brelse(bh);
		return -EIO;
	}

	/* Check if entry already exists */
	for (i = 0; i < dir_block->num_entries; i++) {
		entry = (struct briefs_dir_entry *)(bh->b_data + hdr_size + i * entry_sz);
		if (entry->name_len == entry_name_len && entry->name_off > 0 &&
		    entry->name_off <= BRIEFS_BLOCK_SIZE) {
			char *ename = bh->b_data + BRIEFS_BLOCK_SIZE - entry->name_off + 2;
			if (memcmp(ename, name, entry_name_len) == 0) {
				/* Already exists - update inode */
				entry->inode = child_ino;
				entry->type = type;
				mark_buffer_dirty(bh);
				brelse(bh);
				return 0;
			}
		}
	}

	/* Calculate position for new entry */
	i = dir_block->num_entries;
	entry_off = hdr_size + i * entry_sz;

	/* Name region grows downward from block end */
	u32 name_region_start = BRIEFS_BLOCK_SIZE - dir_block->names_size;
	u32 new_name_start = name_region_start - (2 + entry_name_len);

	if (entry_off + entry_sz > new_name_start) {
		brelse(bh);
		pr_err("briefs: dir block full for ino %lu\n", dir->i_ino);
		return -ENOSPC;
	}

	/* Write name entry (length prefix + name bytes) */
	*(__u16 *)(bh->b_data + new_name_start) = (__u16)entry_name_len;
	memcpy(bh->b_data + new_name_start + 2, name, entry_name_len);

	/* Write DirEntry */
	entry = (struct briefs_dir_entry *)(bh->b_data + entry_off);
	memset(entry, 0, entry_sz);
	entry->inode = child_ino;
	entry->type = type;
	entry->name_len = (__u16)entry_name_len;
	entry->name_off = (__u16)(BRIEFS_BLOCK_SIZE - new_name_start);

	/* Update header */
	dir_block->num_entries++;
	dir_block->data_size = entry_off + entry_sz;
	dir_block->names_size += (2 + entry_name_len);

	mark_buffer_dirty(bh);
	brelse(bh);

	return 0;
}

/*
 * briefs_remove_dir_entry - remove a directory entry by name.
 * Marks the entry as deleted (zeroes name_len/name_off).
 */
int briefs_remove_dir_entry(struct inode *dir, const char *name, size_t name_len)
{
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_dir_block *dir_block;
	struct briefs_dir_entry *entry;
	u64 dir_block_num;
	int i;
	u32 hdr_size = sizeof(struct briefs_dir_block);
	u32 entry_sz = sizeof(struct briefs_dir_entry);

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);

	if (binfo->disk_inode.num_extents_inline == 0)
		return -ENOENT;

	dir_block_num = binfo->disk_inode.inline_extents[0].phys;

	bh = sb_bread(dir->i_sb, dir_block_num);
	if (!bh)
		return -EIO;

	dir_block = (struct briefs_dir_block *)bh->b_data;
	if (dir_block->magic != BRIEFS_DIR_MAGIC) {
		brelse(bh);
		return -EIO;
	}

	for (i = 0; i < dir_block->num_entries; i++) {
		entry = (struct briefs_dir_entry *)(bh->b_data + hdr_size + i * entry_sz);

		if (entry->name_len == 0 || entry->name_off == 0)
			continue;

		if (entry->name_len != name_len)
			continue;

		if (entry->name_off > 0 && entry->name_off <= BRIEFS_BLOCK_SIZE) {
			char *ename = bh->b_data + BRIEFS_BLOCK_SIZE - entry->name_off + 2;
			if (memcmp(ename, name, name_len) == 0) {
				/* Found it — mark as deleted */
				entry->name_len = 0;
				entry->name_off = 0;
				entry->inode = 0;
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
				return 0;
			}
		}
	}

	brelse(bh);
	return -ENOENT;
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
		inode->i_blocks = 0;
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

/* briefs_free_inode_data - free all data blocks owned by an inode */
static void briefs_free_inode_data(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	int i;
	u64 b;

	for (i = 0; i < binfo->disk_inode.num_extents_inline; i++) {
		struct briefs_extent *ext = &binfo->disk_inode.inline_extents[i];
		for (b = 0; b < ext->len; b++) {
			u64 abs_block = ext->phys + b;
			u64 rel_block = abs_to_data(bsi->sb, abs_block);
			briefs_free_block(&bsi->alloc, rel_block);
		}
	}
	binfo->disk_inode.num_extents_inline = 0;
	binfo->disk_inode.num_extents_total = 0;
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

/* briefs_lookup - find inode by name in directory */
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
	struct briefs_sb_info *bsi;
	struct briefs_inode_info *binfo;
	struct buffer_head *bh;
	struct briefs_dir_block *dir_block;
	struct briefs_dir_entry *entry;
	const char *name;
	unsigned int name_len;
	u64 dir_block_num;
	int i;
	
	if (!dir || !S_ISDIR(dir->i_mode)) {
		return ERR_PTR(-ENOTDIR);
	}
	
	bsi = dir->i_sb->s_fs_info;
	name = dentry->d_name.name;
	name_len = dentry->d_name.len;
	
	pr_debug("briefs: lookup for %pd in dir inode %lu\n", dentry, dir->i_ino);
	
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
		/* Look up parent inode from dir inode's parent_inode field */
		struct briefs_inode_info *dinfo = briefs_i(dir);
		u64 parent_ino = dinfo->disk_inode.parent_inode;
		if (parent_ino == 0)
			return ERR_PTR(-EIO);
		struct inode *parent = briefs_iget(dir->i_sb, parent_ino);
		if (IS_ERR(parent))
			return ERR_CAST(parent);
		return d_splice_alias(parent, dentry);
	}
	
	/* Get directory data block from inline extent */
	binfo = briefs_i(dir);
	if (binfo->disk_inode.num_extents_inline == 0) {
		return NULL;
	}
	
	/* First inline extent points to the directory block */
	dir_block_num = binfo->disk_inode.inline_extents[0].phys;
	
	bh = sb_bread(dir->i_sb, dir_block_num);
	if (!bh) {
		pr_err("briefs: failed to read dir block %llu\n", dir_block_num);
		return ERR_PTR(-EIO);
	}
	
	dir_block = (struct briefs_dir_block *)bh->b_data;
	if (dir_block->magic != BRIEFS_DIR_MAGIC) {
		pr_err("briefs: invalid dir block magic: 0x%08x\n", dir_block->magic);
		brelse(bh);
		return ERR_PTR(-EIO);
	}
	
	/* Scan entries */
	for (i = 0; i < dir_block->num_entries; i++) {
		entry = (struct briefs_dir_entry *)(bh->b_data + sizeof(struct briefs_dir_block) +
			                                    i * sizeof(struct briefs_dir_entry));
		
		if (entry->name_len != name_len)
			continue;
		
		/* Compare name */
		if (entry->name_off > 0 && entry->name_off <= BRIEFS_BLOCK_SIZE) {
			char *entry_name = bh->b_data + BRIEFS_BLOCK_SIZE - entry->name_off + 2;
			if (memcmp(entry_name, name, name_len) == 0) {
				/* Found it */
				struct inode *inode = briefs_iget(dir->i_sb, entry->inode);
				brelse(bh);
				if (IS_ERR(inode))
					return ERR_CAST(inode);
				return d_splice_alias(inode, dentry);
			}
		}
	}
	
	brelse(bh);
	/* Name not found - return NULL for negative dentry */
	return NULL;
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
		inode->i_blocks = 0;
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

	/* For directories: allocate a data block and write . and .. entries */
	if (is_dir) {
		struct briefs_inode_info *binfo = briefs_i(inode);
		u64 rel_block = briefs_alloc_block(&bsi->alloc);
		if (rel_block == 0) {
			pr_err("briefs: no free blocks for new dir\n");
			iput(inode);
			return -ENOSPC;
		}
		u64 dir_block = data_to_abs(bsi->sb, rel_block);

		/* Build directory block with . and .. entries */
		struct buffer_head *dbh = sb_getblk(dir->i_sb, dir_block);
		if (!dbh) {
			pr_err("briefs: failed to get dir block %llu\n", dir_block);
			iput(inode);
			return -EIO;
		}

		struct briefs_dir_block *dblk = (struct briefs_dir_block *)dbh->b_data;
		memset(dblk, 0, BRIEFS_BLOCK_SIZE);
		dblk->magic = BRIEFS_DIR_MAGIC;
		dblk->num_entries = 2;

		/* Entry 0: "." */
		struct briefs_dir_entry *de = (struct briefs_dir_entry *)(dbh->b_data + sizeof(struct briefs_dir_block));
		memset(de, 0, sizeof(*de));
		de->inode = ino;
		de->type = 4; /* S_IFDIR >> 12 */
		de->name_len = 1;
		de->name_off = 3; /* 2 bytes len + 1 byte name from block end */

		/* Entry 1: ".." */
		de = (struct briefs_dir_entry *)(dbh->b_data + sizeof(struct briefs_dir_block) + sizeof(struct briefs_dir_entry));
		memset(de, 0, sizeof(*de));
		de->inode = dir->i_ino;
		de->type = 4;
		de->name_len = 2;
		de->name_off = 7; /* 2 bytes len + 2 bytes name from block end */

		/* Pack names at end of block */
		u32 name_pos = BRIEFS_BLOCK_SIZE;
		/* "." first (closest to end) — matches Go mkfs NewDirBlock layout */
		name_pos -= 3;
		*(__u16 *)(dbh->b_data + name_pos) = 1;
		memcpy(dbh->b_data + name_pos + 2, ".", 1);
		/* ".." second (further from end) */
		name_pos -= 4;
		*(__u16 *)(dbh->b_data + name_pos) = 2;
		memcpy(dbh->b_data + name_pos + 2, "..", 2);

		dblk->data_size = sizeof(struct briefs_dir_block) + 2 * sizeof(struct briefs_dir_entry);
		dblk->names_size = 7;

		mark_buffer_dirty(dbh);
		sync_dirty_buffer(dbh);
		brelse(dbh);

		/* Set up the new inode's extent to point to this dir block */
		binfo->disk_inode.num_extents_inline = 1;
		binfo->disk_inode.num_extents_total = 1;
		binfo->disk_inode.inline_extents[0].offset = 0;
		binfo->disk_inode.inline_extents[0].phys = dir_block;
		binfo->disk_inode.inline_extents[0].len = 1;
		binfo->disk_inode.inline_extents[0].flags = 0;

		inode->i_size = dblk->data_size + dblk->names_size;
		binfo->disk_inode.filesize = inode->i_size;

		pr_debug("briefs: allocated dir block abs=%llu (rel=%llu) for inode %llu\n", dir_block, rel_block, ino);
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
