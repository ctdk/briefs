/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>

#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

/*
 * Initialize journal from superblock
 */
int briefs_journal_init(struct briefs_journal *j, struct briefs_superblock *sb) {
	if (!j || !sb) return -EINVAL;

	memset(j, 0, sizeof(*j));
	j->sb = sb;
	j->journal_start = sb->journal_offset;
	j->journal_end = sb->journal_offset + sb->journal_blocks;
	j->checkpoint_block = sb->journal_offset + sb->journal_blocks - 1;
	j->checkpoint_seq = sb->checkpoint_seq;
	j->write_pos = sb->journal_log_start;
	j->dirty = false;

	/* Validate journal geometry */
	if (j->journal_end <= j->journal_start) {
		pr_err("briefs: invalid journal geometry (start=%llu, end=%llu)\n",
			j->journal_start, j->journal_end);
		return -EINVAL;
	}

	/* Allocate 4096-byte block buffer */
	j->cur_block = kzalloc(JOURNAL_BLOCK_SIZE, GFP_KERNEL);
	if (!j->cur_block) return -ENOMEM;

	j->cur_hdr = (struct journal_block_header *)j->cur_block;
	j->cur_hdr->magic = JOURNAL_MAGIC;
	j->write_offset = sizeof(struct journal_block_header); /* 16 bytes */

	pr_debug("briefs: journal initialized (start=%llu, end=%llu, checkpoint=%llu)\n",
		j->journal_start, j->journal_end, j->checkpoint_block);

	return 0;
}

/*
 * Open journal
 */
int briefs_journal_open(struct briefs_journal *j, struct briefs_superblock *sb, struct super_block *vfs_sb) {
	int ret;

	ret = briefs_journal_init(j, sb);
	if (ret) return ret;

	j->vfs_sb = vfs_sb;
	return 0;
}

/*
 * Get next journal block (with wrapping)
 */
u64 briefs_journal_next_block(struct briefs_journal *j, u64 cur) {
	u64 next = cur + 1;
	if (next >= j->journal_end) {
		return j->journal_start;  /* wrap around */
	}
	return next;
}

/*
 * Get previous journal block (for reading backwards)
 */
u64 briefs_journal_prev_block(struct briefs_journal *j, u64 cur) {
	if (cur <= j->journal_start) {
		return j->journal_end - 1;  /* wrap around */
	}
	return cur - 1;
}

/*
 * Read a journal block from disk using the buffer cache.
 * Returns a buffer_head (caller must brelse), or NULL on error.
 */
struct buffer_head *briefs_journal_read_block(struct briefs_journal *j, u64 block_offset) {
	if (!j || !j->vfs_sb)
		return NULL;

	if (block_offset < j->journal_start || block_offset >= j->journal_end) {
		pr_err("briefs: journal read out of range (offset=%llu, range=[%llu,%llu))\n",
			block_offset, j->journal_start, j->journal_end);
		return NULL;
	}

	return sb_bread(j->vfs_sb, block_offset);
}

/*
 * Write a journal block to disk using the buffer cache.
 * Copies data into a buffer_head, marks it dirty, and syncs.
 */
int briefs_journal_write_block(struct briefs_journal *j, u64 block_offset, unsigned char *data) {
	struct buffer_head *bh;

	if (!j || !j->vfs_sb || !data) return -EINVAL;

	if (block_offset < j->journal_start || block_offset >= j->journal_end) {
		pr_err("briefs: journal write out of range (offset=%llu, range=[%llu,%llu))\n",
			block_offset, j->journal_start, j->journal_end);
		return -ERANGE;
	}

	bh = sb_getblk(j->vfs_sb, block_offset);
	if (!bh) {
		pr_err("briefs: sb_getblk failed for journal write at offset=%llu\n", block_offset);
		return -EIO;
	}

	/* sb_getblk may return an unmapped buffer on loop devices.
	 * Ensure the buffer is mapped before using it. */
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block_offset;
		set_buffer_mapped(bh);
	}

	memcpy(bh->b_data, data, JOURNAL_BLOCK_SIZE);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	return 0;
}

/*
 * Compute CRC32C for record data
 */
static u32 compute_record_checksum(enum journal_record_type type, u32 flags,
                                    u32 data_len, const void *data) {
	return briefs_crc32c(0, &type, sizeof(type)) ^
	       briefs_crc32c(0, &flags, sizeof(flags)) ^
	       briefs_crc32c(0, &data_len, sizeof(data_len)) ^
	       briefs_crc32c(0, data, data_len);
}

/*
 * Write a record to the journal.
 * Records are appended sequentially within the current block.
 * If a record doesn't fit, the current block is flushed to disk,
 * write_pos advances, and a new block is started.
 */
int briefs_journal_write_record(struct briefs_journal *j, enum journal_record_type type,
                                 void *data, u32 data_len) {
	u32 total_size;
	u32 hdr_size = sizeof(struct journal_record_hdr);

	if (!j || !data || data_len == 0) return -EINVAL;

	if (type <= JRN_NONE || type >= JRN_END) {
		pr_err("briefs: invalid journal record type=%d\n", type);
		return -EINVAL;
	}

	total_size = hdr_size + data_len;

	/* Check if record fits in remaining block space */
	if (j->write_offset + total_size > JOURNAL_BLOCK_SIZE) {
		/* Flush current block to disk */
		j->cur_hdr->record_count = j->cur_hdr->record_count;
		int ret = briefs_journal_write_block(j, j->write_pos, j->cur_block);
		if (ret) return ret;

		/* Advance write position */
		j->write_pos = briefs_journal_next_block(j, j->write_pos);

		/* Don't clobber the checkpoint block */
		if (j->write_pos == j->checkpoint_block)
			j->write_pos = briefs_journal_next_block(j, j->write_pos);

		/* Reset block buffer for new records */
		memset(j->cur_block, 0, JOURNAL_BLOCK_SIZE);
		j->cur_hdr->magic = JOURNAL_MAGIC;
		j->cur_hdr->block_seq++;
		j->write_offset = hdr_size;
	}

	/* Write record header */
	struct journal_record_hdr *hdr = (struct journal_record_hdr *)(j->cur_block + j->write_offset);
	hdr->type = type;
	hdr->flags = 0;
	hdr->data_len = data_len;
	hdr->checksum = compute_record_checksum(type, 0, data_len, data);

	/* Copy record data after header */
	memcpy(j->cur_block + j->write_offset + hdr_size, data, data_len);

	/* Advance write offset */
	j->write_offset += total_size;
	j->cur_hdr->record_count++;
	j->dirty = true;
	j->records_since_checkpoint++;

	return 0;
}

/*
 * Write a checkpoint
 */
int briefs_journal_checkpoint(struct briefs_journal *j) {
	if (!j) return -EINVAL;

	/* Flush any pending records first */
	if (j->dirty) {
		int ret = briefs_journal_sync(j);
		if (ret) return ret;
	}

	/* Create checkpoint record */
	struct jrn_checkpoint cp;
	memset(&cp, 0, sizeof(cp));
	cp.checkpoint_seq = ++j->checkpoint_seq;
	cp.record_count = j->cur_hdr->record_count;
	cp.log_sequence_end = j->write_pos;
	cp.trie_root_node = j->sb->trie_root_block;
	cp.free_data_count = j->sb->free_data_blocks;
	cp.free_inode_count = j->sb->free_inodes;

	/* Write checkpoint record into a fresh buffer */
	unsigned char *cp_buf = kzalloc(JOURNAL_BLOCK_SIZE, GFP_KERNEL);
	if (!cp_buf) return -ENOMEM;

	struct journal_block_header *cp_hdr = (struct journal_block_header *)cp_buf;
	cp_hdr->magic = CHECKPOINT_MAGIC;
	cp_hdr->block_seq = j->cur_hdr->block_seq;
	cp_hdr->record_count = 1;

	struct journal_record_hdr *rec_hdr = (struct journal_record_hdr *)(cp_buf + sizeof(struct journal_block_header));
	rec_hdr->type = JRN_CHECKPOINT;
	rec_hdr->flags = 0;
	rec_hdr->data_len = sizeof(cp);
	rec_hdr->checksum = compute_record_checksum(JRN_CHECKPOINT, 0, sizeof(cp), &cp);

	memcpy(cp_buf + sizeof(struct journal_block_header) + sizeof(*rec_hdr), &cp, sizeof(cp));

	/* Write checkpoint block to disk */
	int ret = briefs_journal_write_block(j, j->checkpoint_block, cp_buf);
	kfree(cp_buf);
	if (ret) {
		pr_err("briefs: checkpoint write failed (err=%d)\n", ret);
		return ret;
	}

	pr_debug("briefs: checkpoint written (seq=%llu, records=%u, log_end=%llu)\n",
		j->checkpoint_seq, cp.record_count, j->write_pos);

	j->sb->checkpoint_seq = j->checkpoint_seq;
	j->sb->journal_log_start = j->write_pos;
	j->sb->journal_log_end = j->write_pos;

	j->dirty = false;
	j->records_since_checkpoint = 0;

	return 0;
}

/**********************************************************************
 * Replay helpers — operate on raw disk blocks, no VFS inode context.
 **********************************************************************/

/*
 * Read an on-disk inode by inode number.
 * Returns a pointer into the buffer (caller must brelse the bh).
 */
static struct briefs_inode *replay_read_inode(struct super_block *sb,
					       u64 ino,
					       struct buffer_head **bh_out)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 inodeTableBlock, inodeBlock, inodeOffset, inodeIndex;
	struct briefs_inode *di;

	inodeTableBlock = briefs_inode_table_start(bsi->sb);
	inodeIndex = ino - 1;
	inodeBlock = inodeIndex / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (inodeIndex % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	*bh_out = sb_bread(sb, inodeTableBlock + inodeBlock);
	if (!*bh_out)
		return NULL;

	di = (struct briefs_inode *)((*bh_out)->b_data + inodeOffset);
	if (di->magic != _BRIEFS_INODE_MAGIC) {
		brelse(*bh_out);
		*bh_out = NULL;
		return NULL;
	}
	return di;
}

/*
 * Replay a JRN_DIR_UPDATE record (op=0 = add, op=1 = delete).
 */
static int replay_dir_update(struct super_block *sb, struct jrn_dir_update *rec)
{
	struct inode *parent;
	struct briefs_inode_info *binfo;
	int ret;

	parent = briefs_iget(sb, rec->parent_ino);
	if (IS_ERR(parent)) {
		pr_warn("briefs: replay can't iget parent inode %llu (skip)\n",
			rec->parent_ino);
		return 0; /* skip — might have been freed already */
	}

	binfo = briefs_i(parent);
	mutex_lock(&binfo->trie_lock);

	if (rec->op == 0) {
		/* Add directory entry */
		u8 ftype = 0;

		/* Read child inode to get file type */
		struct inode *child = briefs_iget(sb, rec->child_ino);
		if (!IS_ERR(child)) {
			ftype = (child->i_mode & S_IFMT) >> 12;
			iput(child);
		} else {
			pr_warn("briefs: replay can't read child inode %llu, using ftype=0\n",
				rec->child_ino);
		}

		ret = briefs_trie_insert(sb, &binfo->disk_inode,
						 rec->name, rec->name_len,
						 rec->child_ino, ftype);
		if (ret == -EEXIST) {
			ret = 0;
		}
	} else {
		/* Delete directory entry */
		ret = briefs_trie_remove(sb, &binfo->disk_inode, rec->name, rec->name_len);
		if (ret == -ENOENT) {
			ret = 0;
		}
	}

	mutex_unlock(&binfo->trie_lock);
	iput(parent);
	return ret;
}

/*
 * Replay a JRN_INODE_UPDATE record.
 * Writes the recorded inode state back to the on-disk inode block.
 */
static int replay_inode_update(struct super_block *sb, struct jrn_inode_update *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh = NULL;
	struct briefs_inode *di;
	u64 inodeTableBlock, inodeBlock, inodeOffset;

	inodeTableBlock = briefs_inode_table_start(bsi->sb);
	u64 idx = rec->ino - 1;
	inodeBlock = idx / (sb->s_blocksize / BRIEFS_INODE_SIZE);
	inodeOffset = (idx % (sb->s_blocksize / BRIEFS_INODE_SIZE)) * BRIEFS_INODE_SIZE;

	bh = sb_bread(sb, inodeTableBlock + inodeBlock);
	if (!bh) {
		pr_warn("briefs: replay can't read inode block for %llu (skip)\n", rec->ino);
		return 0;
	}

	di = (struct briefs_inode *)(bh->b_data + inodeOffset);

	di->inode_number = rec->ino;
	di->magic = _BRIEFS_INODE_MAGIC;
	di->filemode = rec->mode;
	di->nlinks = rec->nlink;
	di->uid = rec->uid;
	di->gid = rec->gid;
	di->filesize = rec->size;
	di->atime_sec = rec->atime_sec;
	di->atime_nsec = rec->atime_nsec;
	di->mtime_sec = rec->mtime_sec;
	di->mtime_nsec = rec->mtime_nsec;
	di->ctime_sec = rec->ctime_sec;
	di->ctime_nsec = rec->ctime_nsec;
	di->flags = rec->flags;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	pr_debug("briefs: replay restored inode %llu (mode=%o nlink=%u size=%llu)\n",
		rec->ino, rec->mode, rec->nlink, rec->size);
	return 0;
}

/*
 * Replay a JRN_EXTENT_ALLOC record.
 * Marks the data blocks in the extent as allocated in the bitmap.
 */
static int replay_extent_alloc(struct super_block *sb, struct jrn_extent_alloc *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < rec->length; b++) {
		u64 abs_block = rec->phys_start + b;
		u64 rel_block = abs_to_data(bsi->sb, abs_block);
		briefs_reserve_block(&bsi->alloc, rel_block);
	}

	pr_debug("briefs: replay reserved %llu data blocks at phys=%llu for ino=%llu\n",
		rec->length, rec->phys_start, rec->ino);
	return 0;
}

/*
 * Replay a JRN_EXTENT_FREE record.
 * Marks the data blocks as free in the bitmap.
 */
static int replay_extent_free(struct super_block *sb, struct jrn_extent_free *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 b;

	for (b = 0; b < rec->length; b++) {
		u64 abs_block = rec->phys_start + b;
		u64 rel_block = abs_to_data(bsi->sb, abs_block);
		briefs_free_block(&bsi->alloc, rel_block);
	}

	pr_debug("briefs: replay freed %llu data blocks at phys=%llu for ino=%llu\n",
		 rec->length, rec->phys_start, rec->ino);
	return 0;
}

/*
 * Mount/recovery: replay journal from last checkpoint.
 *
 * Walks the journal from journal_log_start to journal_log_end and re-applies
 * each record.  After successful replay, the journal is marked clean.
 */
int briefs_journal_replay(struct briefs_journal *j) {
	if (!j) return -EINVAL;

	if (j->sb->journal_log_start == j->sb->journal_log_end) {
		pr_info("briefs: journal is clean (no replay needed)\n");
		return 0;
	}

	pr_info("briefs: replaying journal (start=%llu, end=%llu)\n",
		j->sb->journal_log_start, j->sb->journal_log_end);

	u64 cur = j->sb->journal_log_start;
	u64 end = j->sb->journal_log_end;
	struct super_block *sb = j->vfs_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;

	u32 records_replayed = 0;
	u32 blocks_read = 0;
	u32 errors = 0;

	while (cur != end) {
		struct buffer_head *bh = briefs_journal_read_block(j, cur);
		if (!bh) {
			pr_err("briefs: journal replay failed at block=%llu\n", cur);
			return -EIO;
		}

		blocks_read++;

		struct journal_block_header *hdr = (struct journal_block_header *)bh->b_data;
		if (hdr->magic != JOURNAL_MAGIC && hdr->magic != CHECKPOINT_MAGIC) {
			pr_warn("briefs: invalid journal magic at block=%llu (got=0x%08x, expected=0x%08x)\n",
				cur, hdr->magic, JOURNAL_MAGIC);
			brelse(bh);
			break;
		}

		/* Walk records by their actual sizes */
		u64 rec_off = sizeof(struct journal_block_header);
		for (u32 i = 0; i < hdr->record_count && rec_off < JOURNAL_BLOCK_SIZE; i++) {
			struct journal_record_hdr *rh = (struct journal_record_hdr *)(bh->b_data + rec_off);

			if (rh->type <= JRN_NONE || rh->type >= JRN_END) {
				pr_warn("briefs: invalid record type=%u at block=%llu\n", rh->type, cur);
				brelse(bh);
				break;
			}

			if (rh->type == JRN_CHECKPOINT) {
				rec_off += sizeof(*rh) + rh->data_len;
				continue;
			}

			void *rec_data = bh->b_data + rec_off + sizeof(*rh);
			int apply_ret = 0;

			switch (rh->type) {
			case JRN_DIR_UPDATE: {
				struct jrn_dir_update *du = rec_data;
				apply_ret = replay_dir_update(sb, du);
				break;
			}
			case JRN_INODE_ALLOC: {
				struct jrn_inode_alloc *ia = rec_data;
				u64 inum = ia->ino;
				if (inum > 0)
					briefs_reserve_block(&bsi->inode_alloc, inum - 1);
				break;
			}
			case JRN_INODE_UPDATE: {
				struct jrn_inode_update *iu = rec_data;
				apply_ret = replay_inode_update(sb, iu);
				break;
			}
			case JRN_EXTENT_ALLOC: {
				struct jrn_extent_alloc *ea = rec_data;
				apply_ret = replay_extent_alloc(sb, ea);
				break;
			}
			case JRN_EXTENT_FREE: {
				struct jrn_extent_free *ef = rec_data;
				apply_ret = replay_extent_free(sb, ef);
				break;
			}
			default:
				pr_debug("briefs: replay skipping unhandled record type=%u\n", rh->type);
				break;
			}

			if (apply_ret) {
				pr_err("briefs: replay error for record type=%u: %d\n",
				       rh->type, apply_ret);
				errors++;
			}
			records_replayed++;
			rec_off += sizeof(*rh) + rh->data_len;
		}

		brelse(bh);
		cur = briefs_journal_next_block(j, cur);
	}

	pr_info("briefs: journal replay complete (blocks=%u, records=%u, errors=%u)\n",
		blocks_read, records_replayed, errors);

	/* Update superblock to mark journal clean */
	j->sb->journal_log_start = j->sb->journal_log_end;
	j->sb->checkpoint_seq++;

	return errors ? -EIO : 0;
}

/*
 * Log a directory entry change
 */
int briefs_journal_dir_update(struct briefs_journal *j, u64 parent_ino, u64 child_ino,
                              const char *name, size_t name_len, u8 op)
{
	struct jrn_dir_update rec;

	if (!j || !name || name_len == 0 || name_len > 251)
		return -EINVAL;

	if (op > 1)
		return -EINVAL;

	memset(&rec, 0, sizeof(rec));
	rec.parent_ino = parent_ino;
	rec.child_ino = child_ino;
	rec.name_len = (u32)name_len;
	memcpy(rec.name, name, name_len);
	rec.op = op;

	return briefs_journal_write_record(j, JRN_DIR_UPDATE, &rec, sizeof(rec));
}

/*
 * Log an extent allocation or extent append.
 * ino: the inode the extent belongs to
 * offset: logical block offset in the file
 * phys_start: starting physical block (absolute)
 * length: number of blocks
 * extent_index: which inline extent slot this affects, or -1 for chain
 */
int briefs_journal_extent_alloc(struct briefs_journal *j, u64 ino,
				 u64 offset, u64 phys_start,
				 u64 length, int extent_index)
{
	struct jrn_extent_alloc rec;

	if (!j) return 0; /* journal not active */

	memset(&rec, 0, sizeof(rec));
	rec.ino = ino;
	rec.offset = offset;
	rec.length = length;
	rec.phys_start = phys_start;
	rec.extent_index = (u32)extent_index;

	return briefs_journal_write_record(j, JRN_EXTENT_ALLOC, &rec, sizeof(rec));
}

/*
 * Log an extent free (block removal).
 */
int briefs_journal_extent_free(struct briefs_journal *j, u64 ino,
			       u64 offset, u64 phys_start, u64 length)
{
	struct jrn_extent_free rec;

	if (!j) return 0;

	memset(&rec, 0, sizeof(rec));
	rec.ino = ino;
	rec.offset = offset;
	rec.phys_start = phys_start;
	rec.length = length;

	return briefs_journal_write_record(j, JRN_EXTENT_FREE, &rec, sizeof(rec));
}

/*
 * Log an inode metadata update (mode, nlink, size, timestamps).
 * Reads the current state from binfo->disk_inode and writes a
 * JRN_INODE_UPDATE record.  Safe to call with NULL journal (no-op).
 */
int briefs_journal_inode_update(struct briefs_journal *j,
				 struct inode *inode)
{
	struct jrn_inode_update rec;
	struct briefs_inode_info *binfo;

	if (!j) return 0;

	binfo = briefs_i(inode);

	memset(&rec, 0, sizeof(rec));
	rec.ino = inode->i_ino;
	rec.mode = inode->i_mode;
	rec.nlink = inode->i_nlink;
	rec.uid = from_kuid(&init_user_ns, inode->i_uid);
	rec.gid = from_kgid(&init_user_ns, inode->i_gid);
	rec.size = inode->i_size;
	rec.atime_sec = inode->i_atime_sec;
	rec.atime_nsec = inode->i_atime_nsec;
	rec.mtime_sec = inode->i_mtime_sec;
	rec.mtime_nsec = inode->i_mtime_nsec;
	rec.ctime_sec = inode->i_ctime_sec;
	rec.ctime_nsec = inode->i_ctime_nsec;
	rec.flags = binfo->disk_inode.flags;

	return briefs_journal_write_record(j, JRN_INODE_UPDATE, &rec, sizeof(rec));
}

/*
 * Cleanup journal
 */
void briefs_journal_cleanup(struct briefs_journal *j) {
	if (!j) return;

	kfree(j->cur_block);
	j->cur_block = NULL;
	j->cur_hdr = NULL;

	memset(j, 0, sizeof(*j));
}

/*
 * Sync dirty journal block to disk
 * Flushes the current block, advances write_pos.
 */
int briefs_journal_sync(struct briefs_journal *j) {
	int ret;

	if (!j || !j->dirty) return 0;

	j->cur_hdr->record_count = j->cur_hdr->record_count;

	/* Write current block to disk */
	ret = briefs_journal_write_block(j, j->write_pos, j->cur_block);
	if (ret) {
		pr_err("briefs: journal sync failed (err=%d)\n", ret);
		return ret;
	}

	/* Advance write position */
	j->write_pos = briefs_journal_next_block(j, j->write_pos);

	/* Don't clobber the checkpoint block */
	if (j->write_pos == j->checkpoint_block)
		j->write_pos = briefs_journal_next_block(j, j->write_pos);

	/* Reset for next block */
	memset(j->cur_block, 0, JOURNAL_BLOCK_SIZE);
	j->cur_hdr->magic = JOURNAL_MAGIC;
	j->cur_hdr->block_seq++;
	j->write_offset = sizeof(struct journal_block_header);

	j->dirty = false;

	pr_debug("briefs: journal synced, write_pos=%llu\n", j->write_pos);

	/*
	 * Periodic checkpoint: if we've accumulated enough records since
	 * the last checkpoint, flush one now.  This prevents the journal
	 * from growing unbounded between unmounts and limits replay time.
	 */
	if (j->records_since_checkpoint >= JRN_CHECKPOINT_INTERVAL) {
		ret = briefs_journal_checkpoint(j);
		if (ret)
			pr_warn("briefs: periodic checkpoint failed: %d\n", ret);
	}

	return 0;
}
