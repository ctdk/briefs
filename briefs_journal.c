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
#include "briefs_debug.h"

/*
 * Locking: the three public journal mutators (briefs_journal_write_record,
 * _sync, _checkpoint) each acquire j->write_lock and delegate to the
 * corresponding __*_locked() helper, which assumes the lock is already held.
 * This lets the internal call chain (write_record -> back-pressure checkpoint
 * -> sync -> periodic checkpoint) run under a single lock acquisition instead
 * of re-taking a non-recursive mutex and deadlocking.  All three helpers are
 * file-local.
 */
static int __briefs_journal_write_record_locked(struct briefs_journal *j,
	enum journal_record_type type, void *data, u32 data_len);
static int __briefs_journal_checkpoint_locked(struct briefs_journal *j);
static int __briefs_journal_sync_locked(struct briefs_journal *j);
/*
 * Initialize journal from superblock
 */
int briefs_journal_init(struct briefs_journal *j, struct briefs_superblock *sb) {
	if (!j || !sb) return -EINVAL;

	memset(j, 0, sizeof(*j));
	mutex_init(&j->write_lock);
	j->sb = sb;
	j->journal_start = le64_to_cpu(sb->journal_offset);
	j->journal_end = le64_to_cpu(sb->journal_offset) + le64_to_cpu(sb->journal_blocks);
	j->checkpoint_block = le64_to_cpu(sb->journal_offset) +
		le64_to_cpu(sb->journal_blocks) - 1;
	j->checkpoint_seq = le64_to_cpu(sb->checkpoint_seq);
	j->write_pos = le64_to_cpu(sb->journal_log_start);
	j->synced_pos = j->write_pos;   /* nothing written yet -> nothing to sync */
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
	j->cur_hdr->magic = cpu_to_le32(JOURNAL_MAGIC);
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
 * Write a journal block to the buffer cache.
 *
 * This path is hot during metadata-heavy workloads (untar creates thousands
 * of journal records).  We deliberately avoid sync_dirty_buffer() here and
 * rely on mark_buffer_dirty() so the block can be batched with other dirty
 * buffers and flushed by pdflush / explicit sync.  The journal is explicitly
 * flushed on fsync, syncfs, periodic checkpoint, and umount.
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
	brelse(bh);

	return 0;
}

/*
 * Compute CRC32C for a journal record.
 *
 * Computes a single chained CRC over type + flags + data_len + data.
 * The previous implementation XORed four separate CRCs; that was both
 * slower (four passes over the small header fields) and not a valid CRC.
 */
static u32 compute_record_checksum(enum journal_record_type type, u32 flags,
                                    u32 data_len, const void *data) {
	__le32 le_type = cpu_to_le32((u32)type);
	__le32 le_flags = cpu_to_le32(flags);
	__le32 le_data_len = cpu_to_le32(data_len);
	u32 crc;

	crc = briefs_crc32c(0, &le_type, sizeof(le_type));
	crc = briefs_crc32c(crc, &le_flags, sizeof(le_flags));
	crc = briefs_crc32c(crc, &le_data_len, sizeof(le_data_len));
	crc = briefs_crc32c(crc, data, data_len);
	return crc;
}

/*
 * Verify a journal record checksum.
 * Returns 0 if the checksum is valid (or zero, meaning legacy/no checksum),
 * -EIO if it does not match.
 */
static int verify_record_checksum(struct journal_record_hdr *rh, const void *data)
{
	u32 computed, stored;

	stored = le32_to_cpu(rh->checksum);
	if (stored == 0)
		return 0; /* legacy record with no checksum */

	computed = compute_record_checksum(le32_to_cpu(rh->type),
					   le32_to_cpu(rh->flags),
					   le32_to_cpu(rh->data_len), data);
	if (computed != stored)
		return -EIO;

	return 0;
}

/*
 * Write a record to the journal.
 * Records are appended sequentially within the current block.
 * If a record doesn't fit, the current block is flushed to disk,
 * write_pos advances, and a new block is started.
 */
static int __briefs_journal_write_record_locked(struct briefs_journal *j,
                                 enum journal_record_type type,
                                 void *data, u32 data_len) {
	u32 total_size;
	u32 hdr_size = sizeof(struct journal_record_hdr);

	if (!j || !data || data_len == 0) return -EINVAL;

	if (type <= JRN_NONE || type >= JRN_END) {
		pr_err("briefs: invalid journal record type=%d\n", type);
		return -EINVAL;
	}

	briefs_stat_inc(briefs_sb(j->vfs_sb), journal_records);

	total_size = hdr_size + data_len;

	/* Check if record fits in remaining block space */
	if (j->write_offset + total_size > JOURNAL_BLOCK_SIZE) {
		/* Flush current block to disk */
		int ret = briefs_journal_write_block(j, j->write_pos, j->cur_block);
		if (ret) return ret;

		/* Advance write position */
		j->write_pos = briefs_journal_next_block(j, j->write_pos);

		/* Don't clobber the checkpoint block */
		if (j->write_pos == j->checkpoint_block)
			j->write_pos = briefs_journal_next_block(j, j->write_pos);

		/* Reset block buffer for new records */
		memset(j->cur_block, 0, JOURNAL_BLOCK_SIZE);
		j->cur_hdr->magic = cpu_to_le32(JOURNAL_MAGIC);
		j->cur_hdr->block_seq = cpu_to_le32(le32_to_cpu(j->cur_hdr->block_seq) + 1);
		j->write_offset = hdr_size;
		/*
		 * cur_block is now empty and the just-flushed block lives only in
		 * the buffer cache (mark_buffer_dirty, written back by pdflush /
		 * sync).  Mirror briefs_journal_sync()'s post-flush state so the
		 * back-pressure checkpoint below sees nothing pending and skips
		 * its internal sync (which would otherwise re-trigger the periodic
		 * checkpoint and recurse).  The record we are about to append
		 * re-sets this true at the bottom of this function.
		 */
		j->dirty = false;

		/*
		 * Back-pressure (0a): the live, un-checkpointed region is
		 * [journal_log_start, write_pos).  journal_log_start advances only
		 * at a checkpoint, and checkpoints otherwise fire solely on
		 * fsync/sync/umount (the periodic one lives inside
		 * briefs_journal_sync).  A metadata-heavy workload that does none of
		 * those can therefore fill the entire ring before any checkpoint
		 * retires records, and the next write would silently clobber the
		 * oldest un-checkpointed block -- broken replay / silent data loss.
		 *
		 * Having just advanced write_pos, write_pos == journal_log_start
		 * holds ONLY when the ring has wrapped completely full: in the
		 * empty state (right after a checkpoint, or at fresh mount)
		 * journal_log_start == write_pos but next_block(write_pos) !=
		 * write_pos, so the condition above is never taken there.  Force a
		 * checkpoint, which advances journal_log_start to write_pos and
		 * frees the ring, instead of overwriting live records.  This
		 * retires the same way the existing periodic checkpoint does; it
		 * takes no BrieFS lock, so it is safe to call from within any
		 * write_record caller (including under alloc->lock).
		 */
		if (j->write_pos == le64_to_cpu(j->sb->journal_log_start)) {
			pr_debug("briefs: journal ring full (back-pressure), checkpointing at write_pos=%llu\n",
				j->write_pos);
			ret = __briefs_journal_checkpoint_locked(j);
			if (ret) {
				pr_err("briefs: journal back-pressure checkpoint failed: %d\n",
				       ret);
				return ret;
			}
			/*
			 * A ring with >= 2 usable blocks is now clear
			 * (journal_log_start == write_pos, next_block != write_pos).
			 * Only a degenerate 1-block ring stays collidable; refuse
			 * rather than corrupt the log.
			 */
			if (briefs_journal_next_block(j, j->write_pos) ==
			    le64_to_cpu(j->sb->journal_log_start)) {
				pr_err("briefs: journal ring exhausted (size=%llu blocks)\n",
				       j->journal_end - j->journal_start);
				return -ENOSPC;
			}
		}
	}

	/* Write record header */
	struct journal_record_hdr *hdr = (struct journal_record_hdr *)(j->cur_block + j->write_offset);
	hdr->type = cpu_to_le32((u32)type);
	hdr->flags = cpu_to_le32(0);
	hdr->data_len = cpu_to_le32(data_len);
	hdr->checksum = cpu_to_le32(compute_record_checksum(type, 0, data_len, data));

	/* Copy record data after header */
	memcpy(j->cur_block + j->write_offset + hdr_size, data, data_len);

	/* Advance write offset */
	j->write_offset += total_size;
	j->cur_hdr->record_count = cpu_to_le32(le32_to_cpu(j->cur_hdr->record_count) + 1);
	j->dirty = true;
	j->records_since_checkpoint++;

	return 0;
}

/*
 * Public entry: serialize concurrent journal writers on j->write_lock, then
 * run the record append.  See struct briefs_journal.write_lock for why this
 * serialization is required (the 011 livelock).
 */
int briefs_journal_write_record(struct briefs_journal *j, enum journal_record_type type,
                                 void *data, u32 data_len) {
	int ret;
	if (!j) return -EINVAL;
	mutex_lock(&j->write_lock);
	ret = __briefs_journal_write_record_locked(j, type, data, data_len);
	mutex_unlock(&j->write_lock);
	return ret;
}

/*
 * Write a checkpoint
 */
static int __briefs_journal_checkpoint_locked(struct briefs_journal *j) {
	struct briefs_sb_info *bsi;

	if (!j) return -EINVAL;

	briefs_stat_inc(briefs_sb(j->vfs_sb), journal_checkpoints);

	/* Flush any pending records first */
	if (j->dirty) {
		int ret = __briefs_journal_sync_locked(j);
		if (ret) return ret;
	}

	/*
	 * Flush all dirty metadata buffers (inode blocks, trie blocks) to disk
	 * BEFORE discarding the journal records that reference them.  The records
	 * in [journal_log_start, write_pos) are about to be discarded by setting
	 * log_start = log_end = write_pos below.  briefs_write_inode() only
	 * mark_buffer_dirty()s the inode block -- it does NOT sync it, because
	 * fsync(2) relies on the JRN_INODE_FULL journal record for durability, not
	 * the inode block itself.  If we advance log_start without first writing
	 * those inode/trie blocks back, a crash leaves the on-disk metadata stale
	 * AND its journal record past log_start (unreplayed): recovery sees
	 * log_start == log_end and replays nothing, so the stale on-disk inode
	 * (e.g. a file's extent btree pointing at a pre-writeback physical block)
	 * wins -> data/metadata loss.  generic/547 hits exactly this: an fsync-all
	 * storm drives periodic checkpoints; an inode block whose JRN_INODE_FULL
	 * was checkpointed away and that background writeback hadn't yet reached
	 * disk before the drop_writes+unmount reverts to stale data on remount.
	 *
	 * sync_blockdev() flushes sb->s_bdev's mapping, which holds the
	 * sb_bread()-read inode and trie buffers.  File DATA lives in the
	 * per-inode address_space and is flushed separately by
	 * file_write_and_wait_range() at fsync time, so it is not touched here.
	 * We hold j->write_lock; no BrieFS metadata-writeback path takes it
	 * (buffer I/O submission/completion is lock-free, and briefs_write_inode
	 * acquires write_lock only when *creating* a record, not when its dirty
	 * buffer is written back), so this wait cannot self-deadlock.
	 */
	{
		int ret = sync_blockdev(j->vfs_sb->s_bdev);
		if (ret)
			return ret;
	}

	/*
	 * Persist the allocator bitmaps alongside the inode/trie buffers flushed
	 * above.  briefs_alloc_sync() is the ONLY path that marks the bitmap
	 * buffer_heads dirty -- the alloc/free hot paths mutate only the in-memory
	 * l0/l1/l2 arrays and never dirty the backing buffers -- so sync_blockdev()
	 * does NOT reach them (it writes only already-dirty buffers).  Without this,
	 * a back-pressure checkpoint mid-burst advances log_start past the
	 * JRN_TRIE_ALLOC/JRN_INODE_ALLOC records that recorded the just-allocated
	 * blocks while the on-disk bitmap still fails to mark those blocks
	 * allocated.  A crash then leaves a correct on-disk trie/inode state beside
	 * a stale bitmap; journal replay rebuilds the allocator from that stale
	 * bitmap plus the now-short live record range, treats the
	 * checkpointed-away-but-still-in-use blocks as FREE, hands one out to a
	 * replayed allocation, and briefs_trie_page_init() ZEROES an in-use trie
	 * page -> mass directory-entry loss (generic/040/041 hardlink storm; same
	 * family as the 547 #2 freed-trie-page-reuse path).  The same safety
	 * argument as sync_blockdev() above holds: briefs_alloc_sync() does
	 * sb_bread()+sync_dirty_buffer() (lock-free buffer I/O) under write_lock,
	 * and no BrieFS metadata path takes write_lock during buffer writeback.
	 */
	bsi = j->vfs_sb->s_fs_info;
	if (bsi) {
		int r2 = briefs_alloc_sync(&bsi->alloc);
		if (r2)
			return r2;
		r2 = briefs_alloc_sync(&bsi->inode_alloc);
		if (r2)
			return r2;
	}

	/*
	 * Refresh superblock free counts from the authoritative allocator state
	 * before writing the checkpoint.
	 */
	bsi = j->vfs_sb->s_fs_info;
	if (bsi) {
		j->sb->free_data_blocks = cpu_to_le64(bsi->alloc.free_count);
		j->sb->free_inodes = cpu_to_le64(bsi->inode_alloc.free_count);
	}

	/* Create checkpoint record */
	struct jrn_checkpoint cp;
	memset(&cp, 0, sizeof(cp));
	cp.checkpoint_seq = cpu_to_le64(++j->checkpoint_seq);
	cp.record_count = cpu_to_le32(le32_to_cpu(j->cur_hdr->record_count));
	cp.log_sequence_end = cpu_to_le64(j->write_pos);
	cp.trie_root_node = j->sb->trie_root_block;
	cp.free_data_count = j->sb->free_data_blocks;
	cp.free_inode_count = j->sb->free_inodes;

	/* Write checkpoint record into a fresh buffer */
	unsigned char *cp_buf = kzalloc(JOURNAL_BLOCK_SIZE, GFP_KERNEL);
	if (!cp_buf) return -ENOMEM;

	struct journal_block_header *cp_hdr = (struct journal_block_header *)cp_buf;
	cp_hdr->magic = cpu_to_le32(CHECKPOINT_MAGIC);
	cp_hdr->block_seq = j->cur_hdr->block_seq;
	cp_hdr->record_count = cpu_to_le32(1);

	struct journal_record_hdr *rec_hdr = (struct journal_record_hdr *)(cp_buf + sizeof(struct journal_block_header));
	rec_hdr->type = cpu_to_le32(JRN_CHECKPOINT);
	rec_hdr->flags = cpu_to_le32(0);
	rec_hdr->data_len = cpu_to_le32(sizeof(cp));
	rec_hdr->checksum = cpu_to_le32(compute_record_checksum(JRN_CHECKPOINT, 0, sizeof(cp), &cp));

	memcpy(cp_buf + sizeof(struct journal_block_header) + sizeof(*rec_hdr), &cp, sizeof(cp));

	/* Write checkpoint block to disk */
	int ret = briefs_journal_write_block(j, j->checkpoint_block, cp_buf);
	kfree(cp_buf);
	if (ret) {
		pr_err("briefs: checkpoint write failed (err=%d)\n", ret);
		return ret;
	}

	/*
	 * Force the checkpoint block to disk before the superblock records the
	 * new log boundaries below, so recovery never sees a superblock whose
	 * checkpoint_seq / log_end run past an unwritten checkpoint block.
	 * briefs_journal_write_block() only marks it dirty.
	 */
	{
		struct buffer_head *cpbh = sb_bread(j->vfs_sb, j->checkpoint_block);
		if (cpbh) {
			if (buffer_dirty(cpbh))
				sync_dirty_buffer(cpbh);
			brelse(cpbh);
		}
	}

	pr_debug("briefs: checkpoint written (seq=%llu, records=%u, log_end=%llu)\n",
		j->checkpoint_seq, cp.record_count, j->write_pos);

	j->sb->checkpoint_seq = cpu_to_le64(j->checkpoint_seq);
	j->sb->journal_log_start = cpu_to_le64(j->write_pos);
	j->sb->journal_log_end = cpu_to_le64(j->write_pos);

	j->dirty = false;
	j->records_since_checkpoint = 0;

	/* Persist the updated superblock so free counts are not stale. */
	return briefs_journal_sync_superblock(j);
}

/*
 * Public entry: take j->write_lock around the checkpoint.  Callers that
 * already hold the lock (the back-pressure path in write_record, the periodic
 * checkpoint in sync) call __briefs_journal_checkpoint_locked() directly.
 */
int briefs_journal_checkpoint(struct briefs_journal *j) {
	int ret;
	if (!j) return -EINVAL;
	mutex_lock(&j->write_lock);
	ret = __briefs_journal_checkpoint_locked(j);
	mutex_unlock(&j->write_lock);
	return ret;
}

/**********************************************************************
 * Replay helpers — operate on raw disk blocks, no VFS inode context.
 **********************************************************************/

/*
 * Read an on-disk inode by inode number.
 * Returns a pointer into the buffer (caller must brelse the bh).
 */

/*
 * Replay a JRN_DIR_UPDATE record (op=0 = add, op=1 = delete).
 */
static int replay_dir_update(struct super_block *sb, struct jrn_dir_update *rec)
{
	struct inode *parent;
	struct briefs_inode_info *binfo;
	int ret;
	u64 parent_ino = le64_to_cpu(rec->parent_ino);
	u64 child_ino = le64_to_cpu(rec->child_ino);
	u32 name_len = le32_to_cpu(rec->name_len);

	parent = briefs_iget(sb, parent_ino);
	if (IS_ERR(parent)) {
		pr_warn("briefs: replay can't iget parent inode %llu (skip)\n",
			parent_ino);
		return 0; /* skip — might have been freed already */
	}

	binfo = briefs_i(parent);
	mutex_lock(&binfo->trie_lock);

	if (rec->op == 0) {
		/* Add directory entry.  The d_type is carried in the record (set at
		 * create/link/rename time from the child's i_mode) so we do NOT iget()
		 * the child here: iget()ing the child during replay would load it into
		 * the VFS inode cache with whatever on-disk state is current at this
		 * point in the replay, and a later JRN_INODE_FULL for the same inode
		 * updates the on-disk block but not the cached inode — leaving a stale
		 * cached inode (e.g. size 0) that post-mount reads would use.  The
		 * child's inode block itself is reconstructed by replay_inode_full(). */
		u8 ftype = rec->ftype;

		ret = briefs_trie_insert(sb, &binfo->disk_inode,
						 rec->name, name_len,
						 child_ino, ftype);
		if (ret == -EEXIST) {
			ret = 0;
		}
	} else {
		/* Delete directory entry */
		ret = briefs_trie_remove(sb, &binfo->disk_inode, rec->name, name_len);
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
	struct briefs_disk_inode *di;
	struct buffer_head *bh;
	u64 ino = le64_to_cpu(rec->ino);
	u32 mode = le32_to_cpu(rec->mode);
	u32 nlink = le32_to_cpu(rec->nlink);
	u32 uid = le32_to_cpu(rec->uid);
	u32 gid = le32_to_cpu(rec->gid);
	u64 size = le64_to_cpu(rec->size);
	u64 atime_sec = le64_to_cpu(rec->atime_sec);
	u64 atime_nsec = le64_to_cpu(rec->atime_nsec);
	u64 mtime_sec = le64_to_cpu(rec->mtime_sec);
	u64 mtime_nsec = le64_to_cpu(rec->mtime_nsec);
	u64 ctime_sec = le64_to_cpu(rec->ctime_sec);
	u64 ctime_nsec = le64_to_cpu(rec->ctime_nsec);
	u32 flags = le32_to_cpu(rec->flags);

	bh = briefs_read_inode_block(sb, ino, &di);
	if (IS_ERR(bh)) {
		pr_warn("briefs: replay can't read inode block for %llu (skip)\n", ino);
		return 0;
	}

	di->inode_number = cpu_to_le64(ino);
	di->magic = cpu_to_le64(_BRIEFS_INODE_MAGIC);
	di->filemode = cpu_to_le32(mode);
	di->nlinks = cpu_to_le32(nlink);
	di->uid = cpu_to_le32(uid);
	di->gid = cpu_to_le32(gid);
	di->filesize = cpu_to_le64(size);
	di->atime_sec = cpu_to_le64(atime_sec);
	di->atime_nsec = cpu_to_le64(atime_nsec);
	di->mtime_sec = cpu_to_le64(mtime_sec);
	di->mtime_nsec = cpu_to_le64(mtime_nsec);
	di->ctime_sec = cpu_to_le64(ctime_sec);
	di->ctime_nsec = cpu_to_le64(ctime_nsec);
	di->flags = cpu_to_le32(flags);

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	pr_debug("briefs: replay restored inode %llu (mode=%o nlink=%u size=%llu)\n",
		ino, mode, nlink, size);
	return 0;
}

/*
 * Replay a JRN_EXTENT_ALLOC record.
 * Marks the data blocks in the extent as allocated in the bitmap.
 */
static int replay_extent_alloc(struct super_block *sb, struct jrn_extent_alloc *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 ino = le64_to_cpu(rec->ino);
	u64 length = le64_to_cpu(rec->length);
	u64 phys_start = le64_to_cpu(rec->phys_start);
	u64 b;

	for (b = 0; b < length; b++) {
		u64 abs_block = phys_start + b;
		u64 rel_block = abs_to_data(bsi->sb, abs_block);
		briefs_reserve_block(&bsi->alloc, rel_block);
	}

	pr_debug("briefs: replay reserved %llu data blocks at phys=%llu for ino=%llu\n",
		length, phys_start, ino);
	return 0;
}

/*
 * Replay a JRN_EXTENT_FREE record.
 * Marks the data blocks as free in the bitmap.
 */
static int replay_extent_free(struct super_block *sb, struct jrn_extent_free *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 length = le64_to_cpu(rec->length);
	u64 phys_start = le64_to_cpu(rec->phys_start);

	briefs_free_blocks_range(bsi, phys_start, length);

	pr_debug("briefs: replay freed %llu data blocks at phys=%llu for ino=%llu\n",
		 length, phys_start, rec->ino);
	return 0;
}

/*
 * Replay a JRN_TRIE_ALLOC record (op=0 = alloc, op=1 = free).
 * Updates the data allocator bitmap so that trie page blocks are not
 * reused for file data during recovery.
 */
static int replay_trie_alloc(struct super_block *sb, struct jrn_trie_alloc *rec)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 block = le64_to_cpu(rec->block);
	u32 op = le32_to_cpu(rec->op);

	if (op == 0) {
		u64 rel = abs_to_data(bsi->sb, block);
		briefs_reserve_block(&bsi->alloc, rel);
		pr_debug("briefs: replay reserved trie page block %llu\n", block);
	} else {
		briefs_free_blocks_range(bsi, block, 1);
		pr_debug("briefs: replay freed trie page block %llu\n", block);
	}
	return 0;
}

/*
 * briefs_journal_sync_superblock - persist the in-memory superblock fields
 * back to the on-disk superblock buffer.  Called after journal replay so a
 * second crash does not force redundant replay.
 */
int briefs_journal_sync_superblock(struct briefs_journal *j)
{
	struct super_block *vfs_sb = j->vfs_sb;
	struct buffer_head *bh;
	struct briefs_superblock *disk_sb;

	if (!vfs_sb)
		return -EINVAL;

	bh = sb_bread(vfs_sb, 0);
	if (!bh)
		return -EIO;

	disk_sb = (struct briefs_superblock *)bh->b_data;

	/*
	 * Refresh the free counts from the authoritative in-memory allocators
	 * before persisting.  The superblock's free_data_blocks / free_inodes are
	 * otherwise only refreshed inside briefs_journal_checkpoint() (and at the
	 * end of replay).  But on a clean unmount the VFS calls sync_fs() ->
	 * briefs_journal_sync() first, which clears j->dirty *without* writing a
	 * checkpoint whenever fewer than JRN_CHECKPOINT_INTERVAL records have
	 * accumulated; put_super() then sees a clean journal, skips its
	 * checkpoint, and calls us directly -- so without this refresh the
	 * on-disk superblock free counts would stay stale (whatever the last
	 * checkpoint wrote) while briefs_alloc_sync() writes the correct bitmap,
	 * and fsck would flag a free-count mismatch.  Refreshing here on every
	 * persist keeps the superblock consistent with the bitmap.  This mirrors
	 * the refresh already done in briefs_journal_replay() and checkpoint().
	 */
	{
		struct briefs_sb_info *bsi = vfs_sb->s_fs_info;
		if (bsi) {
			j->sb->free_data_blocks = cpu_to_le64(bsi->alloc.free_count);
			j->sb->free_inodes = cpu_to_le64(bsi->inode_alloc.free_count);
		}
	}

	disk_sb->checkpoint_seq = j->sb->checkpoint_seq;
	disk_sb->journal_log_start = j->sb->journal_log_start;
	disk_sb->journal_log_end = j->sb->journal_log_end;
	disk_sb->free_data_blocks = j->sb->free_data_blocks;
	disk_sb->free_inodes = j->sb->free_inodes;

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	return 0;
}

/*
 * Replay a JRN_INODE_FULL record.
 * Writes the embedded 512-byte on-disk inode back into the inode table.
 */
static int replay_inode_full(struct super_block *sb, struct jrn_inode_full *rec)
{
	struct briefs_disk_inode *di;
	struct buffer_head *bh;
	u64 ino = le64_to_cpu(rec->ino);

	bh = briefs_read_inode_block(sb, ino, &di);
	if (IS_ERR(bh)) {
		pr_warn("briefs: replay can't read inode block for %llu (skip)\n", ino);
		return 0;
	}

	memcpy(di, rec->inode_data, sizeof(struct briefs_disk_inode));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);

	pr_debug("briefs: replay restored full inode %llu\n", ino);
	return 0;
}

/*
 * Replay a JRN_SYMLINK_DATA record.
 * Writes the inline target bytes back into the symlink data block if the
 * inode still exists and its first extent still points at the recorded block.
 */
static int replay_symlink_data(struct super_block *sb, struct jrn_symlink_data *rec)
{
	struct inode *inode;
	struct briefs_inode_info *binfo;
	struct briefs_extent ext;
	struct buffer_head *bh;
	u64 ino, phys;
	u32 len;

	ino = le64_to_cpu(rec->ino);
	phys = le64_to_cpu(rec->phys);
	len = le32_to_cpu(rec->target_len);

	if (len == 0 || len > BRIEFS_BLOCK_SIZE)
		return 0;

	inode = briefs_iget(sb, ino);
	if (IS_ERR(inode)) {
		pr_debug("briefs: symlink replay skipping freed inode %llu\n", ino);
		return 0;
	}

	if (!S_ISLNK(inode->i_mode)) {
		iput(inode);
		return 0;
	}

	binfo = briefs_i(inode);
	if (binfo->disk_inode.num_extents_total == 0 ||
	    briefs_read_extent(sb, &binfo->disk_inode, 0, &ext) != 0 ||
	    ext.phys != phys || ext.len == 0) {
		iput(inode);
		return 0;
	}

	bh = sb_bread(sb, phys);
	if (!bh) {
		iput(inode);
		return -EIO;
	}

	memset(bh->b_data, 0, sb->s_blocksize);
	memcpy(bh->b_data, rec->target, len);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	iput(inode);

	pr_debug("briefs: replay restored symlink target ino=%llu phys=%llu len=%u\n",
		 ino, phys, len);
	return 0;
}

/*
 * Mount/recovery: replay journal from last checkpoint.
 *
 * Apply one journal record.
 *
 * In the reservation pre-scan (reserve_only=true) only the block/inode
 * allocator ALLOC/FREE records are applied (they reserve or free blocks and
 * are idempotent).  The write/re-derive handlers (DIR_UPDATE, INODE_FULL,
 * INODE_UPDATE, SYMLINK_DATA) are skipped, so the pre-scan touches no on-disk
 * metadata and allocates nothing.  In the apply pass (reserve_only=false) every
 * record is applied as before.
 */
static int apply_record(struct super_block *sb, u32 rec_type, void *rec_data,
			bool reserve_only)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	int ret = 0;

	switch (rec_type) {
	case JRN_DIR_UPDATE:
		if (!reserve_only)
			ret = replay_dir_update(sb, rec_data);
		break;
	case JRN_INODE_ALLOC: {
		struct jrn_inode_alloc *ia = rec_data;
		u64 inum = le64_to_cpu(ia->ino);
		if (inum > 0)
			briefs_reserve_block(&bsi->inode_alloc, inum - 1);
		break;
	}
	case JRN_INODE_FREE: {
		struct jrn_inode_free *ifree = rec_data;
		u64 inum = le64_to_cpu(ifree->ino);
		if (inum > 0)
			briefs_free_block(&bsi->inode_alloc, inum - 1);
		break;
	}
	case JRN_INODE_UPDATE:
		if (!reserve_only)
			ret = replay_inode_update(sb, rec_data);
		break;
	case JRN_EXTENT_ALLOC:
		/* reserves data blocks only; safe and idempotent in both passes */
		ret = replay_extent_alloc(sb, rec_data);
		break;
	case JRN_EXTENT_FREE:
		/* frees data blocks only; safe and idempotent in both passes */
		ret = replay_extent_free(sb, rec_data);
		break;
	case JRN_TRIE_ALLOC:
		/* reserves/frees the trie page block only; safe in both passes */
		ret = replay_trie_alloc(sb, rec_data);
		break;
	case JRN_INODE_FULL:
		if (!reserve_only)
			ret = replay_inode_full(sb, rec_data);
		break;
	case JRN_SYMLINK_DATA:
		if (!reserve_only)
			ret = replay_symlink_data(sb, rec_data);
		break;
	default:
		pr_debug("briefs: replay skipping unhandled record type=%u\n", rec_type);
		break;
	}
	return ret;
}

/*
 * Walk the journal from journal_log_start to journal_log_end once, applying
 * each record via apply_record().  When reserve_only is true this is the
 * reservation pre-scan; otherwise it is the full apply pass.  Returns 0 on
 * success or -EIO on an unreadable/corrupt journal block; *out_* accumulate
 * per-record counters.
 */
static int walk_journal(struct briefs_journal *j, struct super_block *sb,
			bool reserve_only, u32 *out_records, u32 *out_blocks,
			u32 *out_errors)
{
	u64 cur = le64_to_cpu(j->sb->journal_log_start);
	u64 end = le64_to_cpu(j->sb->journal_log_end);
	u32 records = 0, blocks = 0, errors = 0;

	while (cur != end) {
		/* The checkpoint block (journal_end - 1) is a reserved marker, not
		 * part of the replay-able record stream: the write path
		 * (briefs_journal_write_record / briefs_journal_sync) skips it when
		 * advancing write_pos, so ordinary records never land there -- only
		 * a single JRN_CHECKPOINT marker record does, and that is ignored
		 * during replay (see the JRN_CHECKPOINT case below, which continues
		 * without apply_record()).  When the live range wraps, next_block()
		 * lands on it mid-walk; reading and validating it as an ordinary
		 * record block fails whenever its on-disk content is stale from a
		 * prior mount (correct header magic, but a stale/garbage record
		 * type) -- the intermittent "invalid record type" replay failure.
		 * Skip it here, mirroring the write path.  Neither log_start nor
		 * log_end can equal it (write_pos never advances onto it), so this
		 * only fires mid-traversal and never skips a real record block. */
		if (cur == j->checkpoint_block) {
			cur = briefs_journal_next_block(j, cur);
			continue;
		}

		struct buffer_head *bh = briefs_journal_read_block(j, cur);
		if (!bh) {
			pr_err("briefs: journal replay failed at block=%llu\n", cur);
			return -EIO;
		}

		blocks++;

		struct journal_block_header *hdr = (struct journal_block_header *)bh->b_data;
		u32 block_magic = le32_to_cpu(hdr->magic);
		if (block_magic != JOURNAL_MAGIC && block_magic != CHECKPOINT_MAGIC) {
			pr_warn("briefs: invalid journal magic at block=%llu (got=0x%08x, expected=0x%08x)\n",
				cur, block_magic, JOURNAL_MAGIC);
			brelse(bh);
			break;
		}

		u64 rec_off = sizeof(struct journal_block_header);
		bool legacy_checksum_warned = false;
		u32 rec_count = le32_to_cpu(hdr->record_count);
		for (u32 i = 0; i < rec_count && rec_off < JOURNAL_BLOCK_SIZE; i++) {
			struct journal_record_hdr *rh = (struct journal_record_hdr *)(bh->b_data + rec_off);
			u32 rec_type = le32_to_cpu(rh->type);
			u32 rec_data_len = le32_to_cpu(rh->data_len);

			if (rec_type <= JRN_NONE || rec_type >= JRN_END) {
				pr_warn("briefs: invalid record type=%u at block=%llu\n", rec_type, cur);
				brelse(bh);
				return -EIO;
			}

			if (rec_off + sizeof(*rh) + rec_data_len > JOURNAL_BLOCK_SIZE) {
				pr_err("briefs: journal record overflows block at block=%llu (rec_off=%llu data_len=%u)\n",
				       cur, rec_off, rec_data_len);
				brelse(bh);
				return -EIO;
			}

			void *rec_data = bh->b_data + rec_off + sizeof(*rh);

			if (le32_to_cpu(rh->checksum) == 0) {
				if (!legacy_checksum_warned) {
					pr_warn("briefs: legacy journal record with no checksum at block=%llu; skipping CRC verification for this replay\n",
					        cur);
					legacy_checksum_warned = true;
				}
			} else if (verify_record_checksum(rh, rec_data) != 0) {
				pr_err("briefs: journal checksum mismatch at block=%llu record=%u type=%u\n",
				       cur, i, rec_type);
				brelse(bh);
				return -EIO;
			}

			if (rec_type == JRN_CHECKPOINT) {
				rec_off += sizeof(*rh) + rec_data_len;
				continue;
			}

			int apply_ret = apply_record(sb, rec_type, rec_data, reserve_only);
			if (apply_ret) {
				pr_err("briefs: replay error for record type=%u: %d\n",
				       rec_type, apply_ret);
				errors++;
			}
			records++;
			rec_off += sizeof(*rh) + rec_data_len;
		}

		brelse(bh);
		cur = briefs_journal_next_block(j, cur);
	}

	*out_records = records;
	*out_blocks = blocks;
	*out_errors = errors;
	return 0;
}

/*
 * Walks the journal from journal_log_start to journal_log_end and re-applies
 * each record.  After successful replay, the journal is marked clean.
 *
 * Two passes: (1) a reservation pre-scan that marks every block/inode claimed
 * by an ALLOC record (and frees every FREE record) so the in-memory allocators
 * reflect the full post-crash allocation state BEFORE any directory-trie
 * re-derivation runs; (2) the apply pass that writes inode/symlink blocks and
 * re-derives directory tries.  Without the pre-scan, re-derivation
 * (replay_dir_update -> briefs_trie_insert) could allocate a fresh trie page
 * out of a block that a later JRN_EXTENT_ALLOC record reserves for file data,
 * aliasing a trie page onto file data (generic/073 after the page_init sync).
 * briefs_reserve_block is idempotent, so re-reserving in the apply pass is
 * harmless.
 */
int briefs_journal_replay(struct briefs_journal *j) {
	if (!j) return -EINVAL;

	if (le64_to_cpu(j->sb->journal_log_start) == le64_to_cpu(j->sb->journal_log_end)) {
		pr_info("briefs: journal is clean (no replay needed)\n");
		return 0;
	}

	pr_info("briefs: replaying journal (start=%llu, end=%llu)\n",
		le64_to_cpu(j->sb->journal_log_start),
		le64_to_cpu(j->sb->journal_log_end));

	struct super_block *sb = j->vfs_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u32 pre_records = 0, pre_blocks = 0, pre_errors = 0;
	u32 records_replayed = 0, blocks_read = 0, errors = 0;
	int ret;

	/* Pass 1: reserve/free all allocator blocks before re-derivation. */
	ret = walk_journal(j, sb, true, &pre_records, &pre_blocks, &pre_errors);
	if (ret)
		return ret;
	if (pre_errors) {
		pr_err("briefs: journal reservation pre-scan reported %u errors\n", pre_errors);
		return -EIO;
	}

	/* Pass 2: apply all records (writes + trie re-derivation). */
	ret = walk_journal(j, sb, false, &records_replayed, &blocks_read, &errors);
	if (ret)
		return ret;

	pr_info("briefs: journal replay complete (blocks=%u, records=%u, errors=%u)\n",
		blocks_read, records_replayed, errors);

	/* Record how many records we replayed (set once, at mount). */
	if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG))
		atomic64_set(&bsi->stats.journal_replay_records, records_replayed);

	/*
	 * Replay only updated the leaf bitmaps.  Recompute the allocator summary
	 * levels from the leaves and persist them so the on-disk allocator is
	 * consistent with the replayed state.
	 */
	{
		struct briefs_sb_info *bsi = j->vfs_sb->s_fs_info;
		if (bsi) {
			int data_ret, inode_ret;

			briefs_alloc_recompute_summaries(&bsi->alloc);
			briefs_alloc_recompute_summaries(&bsi->inode_alloc);

			data_ret = briefs_alloc_sync(&bsi->alloc);
			if (data_ret)
				pr_err("briefs: failed to sync data allocator after replay: %d\n", data_ret);

			inode_ret = briefs_alloc_sync(&bsi->inode_alloc);
			if (inode_ret)
				pr_err("briefs: failed to sync inode allocator after replay: %d\n", inode_ret);
		}
	}

	/* Update superblock to mark journal clean */
	j->sb->journal_log_start = j->sb->journal_log_end;
	j->sb->checkpoint_seq = cpu_to_le64(le64_to_cpu(j->sb->checkpoint_seq) + 1);

	/* Replay cleared the live range (log_start = log_end), but j->write_pos
	 * / synced_pos were initialized in briefs_journal_init() from the OLD
	 * journal_log_start (now stale, pointing at a block behind the new
	 * log_start).  Without re-pointing them at the cleared log_end, the next
	 * briefs_journal_sync() writes its block at the stale write_pos -- a
	 * block now OUTSIDE [log_start, log_end) -- so the records it holds
	 * (e.g. the create/add entries written between mount and the first
	 * post-replay fsync) are orphaned from the replay range and silently
	 * lost on the next crash, even though they were fsync'd.  generic/321
	 * and generic/322 (rename + fsync, then dm-flakey crash) lost the
	 * renamed-into directory this way.  The cur_block was left empty by
	 * briefs_journal_init(), so only the position cursors need resetting. */
	j->write_pos = le64_to_cpu(j->sb->journal_log_end);
	j->synced_pos = j->write_pos;

	/*
	 * Replay may have changed the allocator free counts.  Copy the current
	 * allocator state into the superblock so the clean checkpoint reflects
	 * the true free space.
	 */
	{
		struct briefs_sb_info *bsi = j->vfs_sb->s_fs_info;
		if (bsi) {
			j->sb->free_data_blocks = cpu_to_le64(bsi->alloc.free_count);
			j->sb->free_inodes = cpu_to_le64(bsi->inode_alloc.free_count);
		}
	}

	/* Persist the clean superblock so a second crash skips replay. */
	{
		int sync_ret = briefs_journal_sync_superblock(j);
		if (sync_ret)
			pr_err("briefs: failed to sync superblock after replay: %d\n", sync_ret);
	}

	return errors ? -EIO : 0;
}

/*
 * Log a newly allocated inode.
 */
int briefs_journal_inode_alloc(struct briefs_journal *j, u64 ino,
				umode_t mode, u32 nlink)
{
	struct jrn_inode_alloc rec;

	if (!j)
		return 0;

	memset(&rec, 0, sizeof(rec));
	rec.ino = cpu_to_le64(ino);
	rec.mode = cpu_to_le32(mode & 07777);
	rec.nlink = cpu_to_le32(nlink);

	return briefs_journal_write_record(j, JRN_INODE_ALLOC, &rec, sizeof(rec));
}

/*
 * Log freeing of an inode.
 */
int briefs_journal_inode_free(struct briefs_journal *j, u64 ino)
{
	struct jrn_inode_free rec;

	if (!j)
		return 0;

	memset(&rec, 0, sizeof(rec));
	rec.ino = cpu_to_le64(ino);

	return briefs_journal_write_record(j, JRN_INODE_FREE, &rec, sizeof(rec));
}

/*
 * Log a directory entry change
 */
int briefs_journal_dir_update(struct briefs_journal *j, u64 parent_ino, u64 child_ino,
                              const char *name, size_t name_len, u8 op, u8 ftype)
{
	struct jrn_dir_update rec;

	if (!j || !name || name_len == 0 || name_len > 255)
		return -EINVAL;

	if (op > 1)
		return -EINVAL;

	memset(&rec, 0, sizeof(rec));
	rec.parent_ino = cpu_to_le64(parent_ino);
	rec.child_ino = cpu_to_le64(child_ino);
	rec.name_len = cpu_to_le32((u32)name_len);
	memcpy(rec.name, name, name_len);
	rec.op = op;
	rec.ftype = ftype;

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
	rec.ino = cpu_to_le64(ino);
	rec.offset = cpu_to_le64(offset);
	rec.length = cpu_to_le64(length);
	rec.phys_start = cpu_to_le64(phys_start);
	rec.extent_index = cpu_to_le32((u32)extent_index);

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
	rec.ino = cpu_to_le64(ino);
	rec.offset = cpu_to_le64(offset);
	rec.phys_start = cpu_to_le64(phys_start);
	rec.length = cpu_to_le64(length);

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
	rec.ino = cpu_to_le64(inode->i_ino);
	rec.mode = cpu_to_le32(inode->i_mode);
	rec.nlink = cpu_to_le32(inode->i_nlink);
	rec.uid = cpu_to_le32(from_kuid(&init_user_ns, inode->i_uid));
	rec.gid = cpu_to_le32(from_kgid(&init_user_ns, inode->i_gid));
	rec.size = cpu_to_le64(inode->i_size);
	rec.atime_sec = cpu_to_le64(inode->i_atime_sec);
	rec.atime_nsec = cpu_to_le64(inode->i_atime_nsec);
	rec.mtime_sec = cpu_to_le64(inode->i_mtime_sec);
	rec.mtime_nsec = cpu_to_le64(inode->i_mtime_nsec);
	rec.ctime_sec = cpu_to_le64(inode->i_ctime_sec);
	rec.ctime_nsec = cpu_to_le64(inode->i_ctime_nsec);
	rec.flags = cpu_to_le32(le32_to_cpu(binfo->disk_inode.flags));

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

	mutex_destroy(&j->write_lock);
	memset(j, 0, sizeof(*j));
}

/*
 * Sync dirty journal block to disk
 * Flushes the current block, advances write_pos.
 */
static int __briefs_journal_sync_locked(struct briefs_journal *j) {
	u64 sync_start, sync_end, pos;
	int ret;

	if (!j || !j->dirty)
		return 0;

	/*
	 * Capture the range of journal blocks dirtied since the last durable
	 * sync: every filled block flushed by briefs_journal_write_record()
	 * (from synced_pos up to write_pos) plus the partial block we are about
	 * to write at write_pos.  briefs_journal_write_block() only marks these
	 * buffers dirty; on the per-file fsync(2) path nothing else reaches the
	 * journal block -- it lives on s_bdev's mapping, which
	 * sync_inode_metadata() (inode block) and file_write_and_wait_range()
	 * (file data) do not touch -- so without syncing them here fsync(2)
	 * would not durably persist metadata.  (sync(2)/syncfs/umount already
	 * flush the journal via the VFS __sync_blockdev(); fsync was the gap.)
	 */
	sync_start = j->synced_pos;
	sync_end = j->write_pos;

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
	j->cur_hdr->magic = cpu_to_le32(JOURNAL_MAGIC);
	j->cur_hdr->block_seq = cpu_to_le32(le32_to_cpu(j->cur_hdr->block_seq) + 1);
	j->write_offset = sizeof(struct journal_block_header);

	j->dirty = false;

	/*
	 * Force every journal block dirtied since the last sync to disk.  Each
	 * sb_bread() is a cache hit returning the uptodate buffer that
	 * briefs_journal_write_block() left dirty; sync_dirty_buffer() submits
	 * the write and waits.  This is the durable flush fsync(2) was missing.
	 */
	for (pos = sync_start; ; pos = briefs_journal_next_block(j, pos)) {
		struct buffer_head *bh = sb_bread(j->vfs_sb, pos);
		if (bh) {
			if (buffer_dirty(bh))
				sync_dirty_buffer(bh);
			brelse(bh);
		}
		if (pos == sync_end)
			break;
	}
	j->synced_pos = j->write_pos;

	pr_debug("briefs: journal synced, write_pos=%llu\n", j->write_pos);

	/*
	 * Persist the journal tail so crash recovery can find the records we
	 * just durably flushed.  briefs_journal_write_block()/sync_dirty_buffer()
	 * above wrote the record blocks to disk, but the on-disk superblock's
	 * journal_log_end still points at the last checkpoint (or, on a fresh
	 * mount, the empty start) -- only briefs_journal_checkpoint() advanced it
	 * before.  So without this, an fsync that doesn't cross a checkpoint
	 * boundary (the common case) leaves the synced record blocks orphaned on
	 * disk: on the next mount briefs_journal_replay() sees log_start == log_end
	 * and replays nothing, and the fsync'd metadata is lost.  journal_log_start
	 * (head) is advanced only by checkpoint and already delimits the oldest
	 * uncheckpointed record, so only the tail needs advancing here.  Replay is
	 * idempotent (briefs_trie_insert tolerates -EEXIST, replay_inode_full
	 * overwrites a fixed snapshot, *_alloc set bitmap bits), so re-applying
	 * records that also reached regular metadata via writeback is harmless.
	 */
	j->sb->journal_log_end = cpu_to_le64(j->write_pos);
	ret = briefs_journal_sync_superblock(j);
	if (ret) {
		pr_err("briefs: failed to persist journal tail after sync: %d\n", ret);
		return ret;
	}

	/*
	 * Periodic checkpoint: if we've accumulated enough records since
	 * the last checkpoint, flush one now.  This prevents the journal
	 * from growing unbounded between unmounts and limits replay time.
	 */
	if (j->records_since_checkpoint >= JRN_CHECKPOINT_INTERVAL) {
		ret = __briefs_journal_checkpoint_locked(j);
		if (ret)
			pr_warn("briefs: periodic checkpoint failed: %d\n", ret);
	}

	return 0;
}

/*
 * Public entry: take j->write_lock around the sync.  Callers that already
 * hold the lock (the flush inside checkpoint, the fsync/sync_fs paths are the
 * public ones that go through here) call __briefs_journal_sync_locked()
 * directly.  The fsync(2) path (briefs_file.c) and sync_fs(2)/umount path
 * (briefs_super.c) arrive here without any other BrieFS lock held.
 */
int briefs_journal_sync(struct briefs_journal *j) {
	int ret;
	if (!j) return 0;
	mutex_lock(&j->write_lock);
	ret = __briefs_journal_sync_locked(j);
	mutex_unlock(&j->write_lock);
	return ret;
}

/*
 * Log allocation of a packed directory-trie page.
 */
int briefs_journal_trie_alloc(struct briefs_journal *j, u64 abs_block)
{
	struct jrn_trie_alloc rec;

	if (!j)
		return 0;

	memset(&rec, 0, sizeof(rec));
	rec.block = cpu_to_le64(abs_block);
	rec.op = cpu_to_le32(0);

	return briefs_journal_write_record(j, JRN_TRIE_ALLOC, &rec, sizeof(rec));
}

/*
 * Log freeing of a packed directory-trie page.
 */
int briefs_journal_trie_free(struct briefs_journal *j, u64 abs_block)
{
	struct jrn_trie_alloc rec;

	if (!j)
		return 0;

	memset(&rec, 0, sizeof(rec));
	rec.block = cpu_to_le64(abs_block);
	rec.op = cpu_to_le32(1);

	return briefs_journal_write_record(j, JRN_TRIE_ALLOC, &rec, sizeof(rec));
}

/*
 * Log a complete 512-byte on-disk inode snapshot.
 */
int briefs_journal_inode_full(struct briefs_journal *j, u64 ino,
                              const struct briefs_disk_inode *di)
{
	struct jrn_inode_full rec;

	if (!j)
		return 0;

	/*
	 * Make every B-tree index block the snapshot references durable before
	 * writing the JRN_INODE_FULL record (#3 ordered commit). The snapshot
	 * restores extent_inline_base + counts + flag, but NOT index-block
	 * contents, so a crash before this drain could leave the snapshot
	 * pointing at a stale/pre-modification index block.
	 *
	 * Tree-backed inodes (InodeFlagIndexed): the root lives in
	 * extent_inline_base and the index is a B+ tree, drained recursively
	 * and lock-free by briefs_btree_drain (every reachable dirty node is
	 * sync_dirty_buffer()'d). Inline-only inodes keep their extents in the
	 * inode block itself, which is persisted with the snapshot -- no
	 * separate index blocks to drain.
	 */
	if (le32_to_cpu(di->flags) & InodeFlagIndexed) {
		u64 base = le64_to_cpu(di->extent_inline_base);
		u64 total = le64_to_cpu(di->num_extents_total);
		u64 cap = total + 16;

		if (cap > (1ull << 20))
			cap = 1ull << 20;
		if (base != 0)
			briefs_btree_drain(j->vfs_sb, base, cap);
	}

	memset(&rec, 0, sizeof(rec));
	rec.ino = cpu_to_le64(ino);
	memcpy(rec.inode_data, di, sizeof(struct briefs_disk_inode));

	return briefs_journal_write_record(j, JRN_INODE_FULL, &rec, sizeof(rec));
}

/*
 * Log symlink target content inline in the journal.
 */
int briefs_journal_symlink_data(struct briefs_journal *j, u64 ino,
                                u64 phys, const char *target,
                                size_t target_len)
{
	struct jrn_symlink_data *rec;
	size_t rec_size;
	int ret;

	if (!j || !target || target_len == 0 || target_len > BRIEFS_BLOCK_SIZE)
		return -EINVAL;

	rec_size = sizeof(struct jrn_symlink_data) + target_len;
	rec = kmalloc(rec_size, GFP_KERNEL);
	if (!rec)
		return -ENOMEM;

	rec->ino = cpu_to_le64(ino);
	rec->phys = cpu_to_le64(phys);
	rec->target_len = cpu_to_le32((u32)target_len);
	memcpy(rec->target, target, target_len);

	ret = briefs_journal_write_record(j, JRN_SYMLINK_DATA, rec, rec_size);
	kfree(rec);
	return ret;
}
