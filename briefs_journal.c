/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/bio.h>

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
 * Open journal with block device
 */
int briefs_journal_open(struct briefs_journal *j, struct briefs_superblock *sb, struct block_device *bdev) {
	int ret;

	ret = briefs_journal_init(j, sb);
	if (ret) return ret;

	j->bdev = bdev;
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
 * Read a journal block from disk
 */
int briefs_journal_read_block(struct briefs_journal *j, u64 block_offset, unsigned char *buf) {
	struct bio *bio;
	int ret;

	if (!j || !j->bdev || !buf) return -EINVAL;

	if (block_offset < j->journal_start || block_offset >= j->journal_end) {
		pr_err("briefs: journal read out of range (offset=%llu, range=[%llu,%llu))\n",
			block_offset, j->journal_start, j->journal_end);
		return -ERANGE;
	}

	bio = bio_alloc(j->bdev, 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) return -ENOMEM;

	if (!bio_add_page(bio, virt_to_page(buf), JOURNAL_BLOCK_SIZE, 0)) {
		pr_err("briefs: bio_add_page failed for read at offset=%llu\n", block_offset);
		bio_put(bio);
		return -EIO;
	}

	bio->bi_iter.bi_sector = block_offset * (JOURNAL_BLOCK_SIZE / 512);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret)
		pr_err("briefs: journal read failed at offset=%llu (err=%d)\n", block_offset, ret);

	return ret;
}

/*
 * Write a journal block to disk
 */
int briefs_journal_write_block(struct briefs_journal *j, u64 block_offset, unsigned char *buf) {
	struct bio *bio;
	int ret;

	if (!j || !j->bdev || !buf) return -EINVAL;

	if (block_offset < j->journal_start || block_offset >= j->journal_end) {
		pr_err("briefs: journal write out of range (offset=%llu, range=[%llu,%llu))\n",
			block_offset, j->journal_start, j->journal_end);
		return -ERANGE;
	}

	bio = bio_alloc(j->bdev, 1, REQ_OP_WRITE, GFP_KERNEL);
	if (!bio) return -ENOMEM;

	if (!bio_add_page(bio, virt_to_page(buf), JOURNAL_BLOCK_SIZE, 0)) {
		pr_err("briefs: bio_add_page failed for write at offset=%llu\n", block_offset);
		bio_put(bio);
		return -EIO;
	}

	bio->bi_iter.bi_sector = block_offset * (JOURNAL_BLOCK_SIZE / 512);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	if (ret)
		pr_err("briefs: journal write failed at offset=%llu (err=%d)\n", block_offset, ret);

	return ret;
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

	return 0;
}

/*
 * Mount/recovery: replay journal from last checkpoint
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
	unsigned char *buf = kmalloc(JOURNAL_BLOCK_SIZE, GFP_KERNEL);
	if (!buf) return -ENOMEM;

	u32 records_replayed = 0;
	u32 blocks_read = 0;

	while (cur != end) {
		int ret = briefs_journal_read_block(j, cur, buf);
		if (ret) {
			pr_err("briefs: journal replay failed at block=%llu (err=%d)\n", cur, ret);
			kfree(buf);
			return ret;
		}

		blocks_read++;

		struct journal_block_header *hdr = (struct journal_block_header *)buf;
		if (hdr->magic != JOURNAL_MAGIC && hdr->magic != CHECKPOINT_MAGIC) {
			pr_warn("briefs: invalid journal magic at block=%llu (got=0x%08x, expected=0x%08x)\n",
				cur, hdr->magic, JOURNAL_MAGIC);
			break;
		}

		/* Walk records by their actual sizes */
		u64 rec_off = sizeof(struct journal_block_header);
		for (u32 i = 0; i < hdr->record_count && rec_off < JOURNAL_BLOCK_SIZE; i++) {
			struct journal_record_hdr *rh = (struct journal_record_hdr *)(buf + rec_off);

			if (rh->type <= JRN_NONE || rh->type >= JRN_END) {
				pr_warn("briefs: invalid record type=%u at block=%llu\n", rh->type, cur);
				break;
			}

			if (rh->type != JRN_CHECKPOINT) {
				records_replayed++;
			}

			rec_off += sizeof(*rh) + rh->data_len;
		}

		cur = briefs_journal_next_block(j, cur);
	}

	kfree(buf);

	pr_info("briefs: journal replay complete (blocks=%u, records=%u)\n",
		blocks_read, records_replayed);

	j->sb->journal_log_start = j->sb->journal_log_end;
	j->sb->checkpoint_seq++;

	return 0;
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

	return 0;
}