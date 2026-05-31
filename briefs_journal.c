/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/stddef.h>
#include <linux/types.h>
#include <linux/blkdev.h>

#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

#define JOURNAL_BLOCK_SIZE 4096
#define JRN_RECORD_MAX_SIZE 320  /* max record size (JRN_DIR_UPDATE) */

/*
 * Initialize journal from superblock
 */
int briefs_journal_init(struct briefs_journal *j, struct briefs_superblock *sb) {
	if (!j || !sb) return -EINVAL;

	memset(j, 0, sizeof(*j));
	j->sb = sb;
	j->journal_start = sb->journal_offset;
	j->journal_end = sb->journal_offset + sb->journal_blocks;
	j->checkpoint_block = sb->journal_offset + sb->journal_blocks - 1;  /* last block */
	j->checkpoint_seq = sb->checkpoint_seq;
	j->write_pos = sb->journal_log_start;
	j->dirty = false;

	/* Allocate current block buffer */
	j->cur_block = kzalloc(sizeof(struct journal_block), GFP_KERNEL);
	if (!j->cur_block) return -ENOMEM;

	/* TODO: load current block from disk if not at start */
	/* TODO: read_block(j, j->write_pos, j->cur_block); */

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
int briefs_journal_read_block(struct briefs_journal *j, u64 block_offset, struct journal_block *block) {
	if (!j || !j->bdev || !block) return -EINVAL;

	struct bio *bio;
	int ret;

	bio = bio_alloc(j->bdev, 1, REQ_OP_READ, GFP_KERNEL);
	if (!bio) return -ENOMEM;

	if (!bio_add_page(bio, virt_to_page(block), JOURNAL_BLOCK_SIZE, 0)) {
		bio_put(bio);
		return -EIO;
	}

	bio->bi_iter.bi_sector = block_offset * (JOURNAL_BLOCK_SIZE / 512);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	return ret;
}

/*
 * Write a journal block to disk
 */
int briefs_journal_write_block(struct briefs_journal *j, u64 block_offset, struct journal_block *block) {
	if (!j || !j->bdev || !block) return -EINVAL;

	struct bio *bio;
	int ret;

	bio = bio_alloc(j->bdev, 1, REQ_OP_WRITE, GFP_KERNEL);
	if (!bio) return -ENOMEM;

	if (!bio_add_page(bio, virt_to_page(block), JOURNAL_BLOCK_SIZE, 0)) {
		bio_put(bio);
		return -EIO;
	}

	bio->bi_iter.bi_sector = block_offset * (JOURNAL_BLOCK_SIZE / 512);
	ret = submit_bio_wait(bio);
	bio_put(bio);

	return ret;
}

/*
 * Compute CRC32C for record data
 */
static u32 compute_record_checksum(enum journal_record_type type, u32 flags,
                                    u32 data_len, const void *data) {
	/* Simplified: return 0 for now */
	/* Full implementation would use briefs_crc32c */
	return 0;
}

/*
 * Write a record to the journal
 */
int briefs_journal_write_record(struct briefs_journal *j, enum journal_record_type type,
                                 void *data, u32 data_len) {
	if (!j || !data || data_len == 0) return -EINVAL;

	struct journal_record_hdr *hdr = (struct journal_record_hdr *)j->cur_block->records;
	u32 record_size = sizeof(struct journal_record_hdr) + data_len;

	/* Check if we need to flush current block */
	if (j->cur_block->header.record_count >= 255) {
		/* TODO: write block to disk and allocate new */
		return -ENOSPC;  /* For now, fail if block full */
	}

	/* Create record header */
	hdr->type = type;
	hdr->flags = 0;
	hdr->data_len = data_len;
	hdr->checksum = compute_record_checksum(type, 0, data_len, data);

	/* Copy record data after header */
	memcpy((unsigned char *)hdr + sizeof(*hdr), data, data_len);

	/* Update block header */
	j->cur_block->header.record_count++;
	j->dirty = true;

	return 0;
}

/*
 * Write a checkpoint
 */
int briefs_journal_checkpoint(struct briefs_journal *j) {
	if (!j) return -EINVAL;

	/* Create checkpoint record */
	struct jrn_checkpoint cp;
	memset(&cp, 0, sizeof(cp));
	cp.checkpoint_seq = ++j->checkpoint_seq;
	cp.record_count = j->cur_block->header.record_count;
	cp.log_sequence_end = j->write_pos;
	cp.trie_root_node = j->sb->trie_root_block;
	cp.free_data_count = j->sb->data_blocks;  /* TODO: get from trie root */
	cp.free_inode_count = j->sb->free_inodes;

	/* Write checkpoint record */
	int ret = briefs_journal_write_record(j, JRN_CHECKPOINT, &cp, sizeof(cp));
	if (ret) return ret;

	/* Write checkpoint block to disk */
	ret = briefs_journal_write_block(j, j->checkpoint_block, j->cur_block);
	if (ret) return ret;

	/* Update superblock */
	j->sb->checkpoint_seq = j->checkpoint_seq;
	j->sb->journal_log_start = briefs_journal_next_block(j, j->write_pos);
	j->sb->journal_log_end = j->sb->journal_log_start;

	/* Reset block for next use */
	memset(j->cur_block, 0, sizeof(struct journal_block));
	j->dirty = false;

	return 0;
}

/*
 * Mount/recovery: replay journal from last checkpoint
 */
int briefs_journal_replay(struct briefs_journal *j) {
	if (!j) return -EINVAL;

	/* Check if journal is empty */
	if (j->sb->journal_log_start == j->sb->journal_log_end) {
		/* Journal is clean */
		return 0;
	}

	/* Walk journal from log_start to log_end */
	u64 cur = j->sb->journal_log_start;
	u64 end = j->sb->journal_log_end;
	struct journal_block block;

	while (cur != end) {
		/* Read journal block from disk */
		int ret = briefs_journal_read_block(j, cur, &block);
		if (ret) return ret;

		/* For each record in block: */
		for (u32 i = 0; i < block.header.record_count; i++) {
			struct journal_record_hdr *hdr = (struct journal_record_hdr *)block.records;
			hdr = (struct journal_record_hdr *)((unsigned char *)hdr + i * JRN_RECORD_MAX_SIZE);

			/* Validate checksum */
			/* if (hdr->checksum != compute_checksum(...)) return -EIO; */

			/* Replay based on record type */
			switch (hdr->type) {
			case JRN_EXTENT_ALLOC: {
				struct jrn_extent_alloc *rec = (struct jrn_extent_alloc *)(((unsigned char *)hdr) + sizeof(*hdr));
				/* Update trie and inode */
				break;
			}
			case JRN_EXTENT_FREE: {
				struct jrn_extent_free *rec = (struct jrn_extent_free *)(((unsigned char *)hdr) + sizeof(*hdr));
				/* Update trie */
				break;
			}
			case JRN_INODE_UPDATE: {
				struct jrn_inode_update *rec = (struct jrn_inode_update *)(((unsigned char *)hdr) + sizeof(*hdr));
				/* Update inode */
				break;
			}
			case JRN_INODE_ALLOC: {
				struct jrn_inode_alloc *rec = (struct jrn_inode_alloc *)(((unsigned char *)hdr) + sizeof(*hdr));
				/* Allocate inode */
				break;
			}
			case JRN_INODE_FREE: {
				struct jrn_inode_free *rec = (struct jrn_inode_free *)(((unsigned char *)hdr) + sizeof(*hdr));
				/* Free inode */
				break;
			}
			case JRN_CHECKPOINT:
				/* Skip - already committed */
				break;
			default:
				/* Unknown record type - skip */
				break;
			}
		}

		cur = briefs_journal_next_block(j, cur);
	}

	/* Reset journal after replay */
	j->sb->journal_log_start = j->sb->journal_log_end;
	j->sb->checkpoint_seq++;

	return 0;
}

/*
 * Cleanup journal
 */
void briefs_journal_cleanup(struct briefs_journal *j) {
	if (!j) return;

	if (j->cur_block) {
		kfree(j->cur_block);
		j->cur_block = NULL;
	}

	memset(j, 0, sizeof(*j));
}

/*
 * Sync dirty journal block to disk
 */
int briefs_journal_sync(struct briefs_journal *j) {
	if (!j || !j->dirty) return 0;

	/* Write current block to disk */
	int ret = briefs_journal_write_block(j, j->write_pos, j->cur_block);
	if (ret) return ret;

	j->dirty = false;
	return 0;
}
