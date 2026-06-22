/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs superblock lifecycle operations */

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
 * briefs_sync_fs - sync the filesystem (called by sync(2), syncfs(2), umount).
 * Flushes the journal to ensure crash-recovery ordering is up to date.
 */
int briefs_sync_fs(struct super_block *sb, int wait)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;

	if (bsi->journal && bsi->journal->dirty)
		return briefs_journal_sync(bsi->journal);
	return 0;
}
/* briefs_fill_super - entry point for mount */
/* Keeping things simple, at least at first. Cribbing off of xiafs_fill_super
 * since it's reasonably simple but gets the job done. */
/*
 * Clean up the journal attached to a briefs_sb_info during mount error paths.
 */
static void briefs_cleanup_bsi_journal(struct briefs_sb_info *bsi)
{
	if (!bsi || !bsi->journal)
		return;

	briefs_journal_cleanup(bsi->journal);
	kfree(bsi->journal);
	bsi->journal = NULL;
}

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

	/* Parse mount options (legacy mount API forwards the raw options string
	 * in @data). Only "debug" is recognized today; unknown tokens are warned
	 * about and ignored so future/VFS-injected options don't break mounting. */
	bsi->mount_jiffies = get_jiffies_64();
	ret = briefs_parse_options(data, bsi);
	if (ret)
		goto out;

	if (!sb_set_blocksize(sb, 4096)) {
		pr_err("briefs: blocksize too small\n");
		goto out_bad_hblock;
	}

	if (!(bh = sb_bread(sb, 0))) {
		pr_err("briefs: unable to read superblock\n");
		goto out_bad_sb;
	}

	bsb = (struct briefs_superblock *) bh->b_data;

	pr_info("briefs: superblock magic=0x%016llx, inode_table_offset=%llu\n",
	        le64_to_cpu(bsb->magic), le64_to_cpu(bsb->inode_table_offset));

	sb->s_magic = le64_to_cpu(bsb->magic);

	pr_info("briefs: magic=0x%08lx\n", sb->s_magic);

	if (sb->s_magic != _BRIEFS_SUPER_MAGIC) {
		pr_err("briefs: invalid magic\n");
		sb->s_dev = 0;
		ret = -EINVAL;
		goto out_no_fs;
	}

	/* On-disk format gate (clean break, v0.9.0 B+ tree extent index). */
	if (le64_to_cpu(bsb->minor_version) != _BRIEFS_MINOR_VER) {
		pr_err("briefs: incompatible on-disk minor version %llu (need %d)\n",
		       le64_to_cpu(bsb->minor_version), _BRIEFS_MINOR_VER);
		ret = -EINVAL;
		goto out_release;
	}
	if (!(le64_to_cpu(bsb->feature_incompat) & BRIEFS_FEATURE_INCOMPAT_BTREE)) {
		pr_err("briefs: image is not B-tree formatted (feature_incompat 0x%llx)\n",
		       le64_to_cpu(bsb->feature_incompat));
		ret = -EINVAL;
		goto out_release;
	}
	if (le64_to_cpu(bsb->feature_incompat) & ~BRIEFS_FEATURE_INCOMPAT_BTREE) {
		pr_err("briefs: unknown incompatible feature bits 0x%llx\n",
		       le64_to_cpu(bsb->feature_incompat) & ~BRIEFS_FEATURE_INCOMPAT_BTREE);
		ret = -EINVAL;
		goto out_release;
	}

	bsi->sb_bh = bh;
	bsi->sb = bsb;

	/* Initialize data block allocator */
	ret = briefs_alloc_init(&bsi->alloc, sb, bsb);
	if (ret) {
		pr_err("briefs: failed to initialize data block allocator: %d\n", ret);
		goto out_no_root;
	}

	/* Initialize inode allocator */
	ret = briefs_alloc_init_at(&bsi->inode_alloc, sb,
	                           le64_to_cpu(bsb->inode_bitmap_offset));
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

	/*
	 * Install the super_operations and mark the superblock active BEFORE
	 * running journal replay.  Replay applies JRN_DIR_UPDATE / JRN_SYMLINK_DATA
	 * records by iget()/iput() of the affected inodes to read/modify their
	 * on-disk metadata.  iput_final() only takes its benign __inode_add_lru()
	 * early-return path (which simply caches the inode on the sb LRU, touching
	 * no other list) when (sb->s_flags & SB_ACTIVE); without SB_ACTIVE it instead
	 * falls into the evict branch, which runs inode_lru_list_del()/inode_sb_list_del()
	 * and destroy_inode() on every replayed inode.  Doing that alloc/evict/destroy
	 * cycling repeatedly (pre-SB_ACTIVE, before the inode cache is warm) corrupts
	 * the VFS inode lists: freshly-iget'd I_NEW inodes have i_lru/i_sb_list left at
	 * stale slab-reuse values (inode_init_always() does not initialize them; only the
	 * one-time slab ctor does), and evicting them trips list-debug BUGs ("next is
	 * NULL" / "next->prev should be X but was NULL").  Setting SB_ACTIVE here makes
	 * replay's iput()s cache inodes instead of evicting them, sidestepping the whole
	 * fragile pre-mount eviction path.  The VFS ORs SB_ACTIVE in again later; this
	 * is not undone on a clean mount.  s_op must be installed first so that any
	 * writeback during replay has a valid super_operations.
	 */
	sb->s_op = &briefs_super_ops;
	/* Enable NFS export with generation-based file handles. */
	sb->s_export_op = &briefs_export_ops;
	sb->s_flags |= SB_ACTIVE;

	/* Replay journal on mount (if not clean) */
	ret = briefs_journal_replay(bsi->journal);
	if (ret) {
		pr_err("briefs: journal replay failed: %d\n", ret);
		goto out_no_journal;
	}

	/* BrieFS tracks file size, extent offsets/lengths, and extent counts in
	 * 64-bit fields throughout, so the only file-size gate is the VFS s_maxbytes
	 * default (MAX_NON_LFS, ~2 GiB).  Raise it to the page-cache ceiling; the
	 * allocator returns ENOSPC well before this binds. */
	sb->s_maxbytes = MAX_LFS_FILESIZE;

	/* On-disk timestamps are __le64 sec + __le64 nsec (briefs.h:314-321) and
	 * every conversion preserves nanoseconds, so match that granularity here.
	 * The inherited default (NSEC_PER_SEC = 1 s) truncates the generic VFS
	 * timestamp updates (atime/mtime/ctime via current_time()) to whole
	 * seconds; s_time_gran = 1 keeps sub-second resolution. All BrieFS time
	 * writes use current_time() (the same coarse clock the VFS uses in
	 * notify_change/file_update_time) so a BrieFS-set time can never run
	 * ahead of a later VFS-set time -- mixing current_time() (coarse) with
	 * ktime_get_real_ts64() (fine) let a post-create chmod record a ctime
	 * nominally before the fine-grained creation mtime (generic/423).
	 * ext4/xfs/btrfs/f2fs use the same value. */
	sb->s_time_gran = 1;

	/* Drop every inode the replay pass cached.
	 *
	 * Journal replay applies records in commit order, but a directory-add
	 * record (replay_dir_update) must iget() the *parent* to operate on its
	 * trie, and that iget happens BEFORE the parent's own inode_full/inode_update
	 * record is replayed.  The iget therefore reads the parent's pre-update
	 * on-disk state and caches it; the subsequent inode_full write updates the
	 * on-disk inode block (sync_dirty_buffer) but leaves the cached VFS inode
	 * stale -- most importantly nlinks, which the cached copy never sees
	 * bumped.  fill_super's briefs_iget(root) below would then return that stale
	 * cached root, and across crash-remount mkdir/rmdir cycles root's nlink
	 * never advanced (2->2->2->1->1->1->0), eventually hitting 0 on umount and
	 * freeing root's inode slot -> the shared inode-table block (ino 1/2/3)
	 * gets zeroed, and the next mount cannot iget root (generic/321 subtest 3).
	 *
	 * evict_inodes() is the standard umount-style sweep: it disposes every
	 * i_count==0, non-I_NEW inode on sb->s_inodes.  All replay-cached parents
	 * are unreferenced (replay_dir_update iput()s them) and not I_DIRTY (the
	 * replay path persists inode blocks via sync_dirty_buffer, not
	 * mark_inode_dirty), so eviction is lossless -- the synced inode blocks
	 * live in the block device's buffer cache, not the inode mappings, and the
	 * root iget below re-reads the now-correct nlinks.  SB_ACTIVE remains set;
	 * it only governed the during-replay iput() path (see the comment above),
	 * not this post-replay sweep.
	 */
	evict_inodes(sb);

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

	bsi->data_blocks = le64_to_cpu(bsb->data_blocks);
	bsi->free_data_blocks = le64_to_cpu(bsb->free_data_blocks);
	/* Compute inode counts from the inode allocator pyramid */
	bsi->num_inodes = bsi->inode_alloc.block_count;
	bsi->free_inodes = bsi->inode_alloc.free_count;

	/* Observability surfaces: register this sb on the global list (consumed
	 * by /proc/fs/briefs/mounts), add its per-sb sysfs attributes (always on),
	 * and — only under -o debug — its per-sb debugfs tree. All best-effort;
	 * the put_super teardown counterparts are guarded so a failed add is a
	 * no-op on remove. Done after d_make_root succeeds. */
	briefs_sb_list_add(bsi);
	briefs_sysfs_add_sb(sb);
	briefs_debugfs_add_sb(sb);

	pr_info("briefs: superblock loaded, mounting successful\n");

	if (!sb_rdonly(sb))
		mark_buffer_dirty(bh);

	return 0;

	out_no_journal:
	pr_err("briefs: journal init failed.\n");
	briefs_cleanup_bsi_journal(bsi);
	goto out_release;

out_no_root:
	pr_err("briefs: get root inode failed.\n");
	briefs_cleanup_bsi_journal(bsi);
	goto out_release;

out_iput:
	iput(root_inode);
	briefs_cleanup_bsi_journal(bsi);
	goto out_release;

out_no_fs:
	pr_err("VFS: Can't find a BrieFS filesystem on device %s.\n", sb->s_id);

out_release:
	if (bsi)
		bsi->sb_bh = NULL;
	brelse(bh);
	goto out;

out_bad_hblock:
	ret = -EINVAL;
	pr_err("BrieFS: blocksize too small for device.\n");
	goto out;

out_bad_sb:
	/*
	 * sb_bread() of the superblock failed.  ret may be 0 here:
	 * briefs_parse_options() ran successfully and reset ret to 0, and the
	 * -EINVAL initializer at the top of fill_super was overwritten.  Without
	 * an explicit error here, fill_super returns 0 without ever setting
	 * sb->s_root, so mount_bdev() returns dget(NULL) == NULL -- a NULL
	 * root dentry.  legacy_get_tree() then dereferences root->d_sb and
	 * oopses (a superblock I/O error becomes a kernel NULL-deref instead
	 * of a clean -EIO mount failure).  Set the error explicitly.
	 */
	ret = -EIO;
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

/* briefs_put_super - cleanup superblock */
void briefs_put_super(struct super_block *sb) {
	struct briefs_sb_info *bsi = sb->s_fs_info;

	pr_info("briefs: put_super enter\n");

	if (bsi) {
		/* Quiesce observability surfaces before tearing bsi down so no
		 * debugfs/sysfs/proc reader races with the journal/alloc/sb_bh
		 * cleanup below. sysfs kobject_del drains in-flight kernfs show
		 * ops (and blocks new ones); the debugfs dir and the /proc list
		 * entry are removed first. */
		briefs_debugfs_remove_sb(sb);
		if (!list_empty(&bsi->sb_list))
			briefs_sb_list_del(bsi);
		briefs_sysfs_remove_sb(sb);

		/*
		 * Always checkpoint the journal before unmount.
		 *
		 * briefs_journal_sync() (called by sync(2)/syncfs(2) via
		 * briefs_sync_fs) flushes pending record buffers to disk and
		 * clears ->dirty, but it does NOT advance journal_log_start:
		 * only a checkpoint does that.  If we gate the unmount
		 * checkpoint on ->dirty, a workload that ran sync but not
		 * fsync leaves the journal with log_start < log_end and
		 * ->dirty == false, so this checkpoint is skipped and the
		 * uncheckpointed records survive into the next mount.  Remount
		 * then replays them, and replay_dir_update() ->
		 * briefs_trie_insert() -> briefs_trie_page_init() can allocate
		 * a fresh trie page out of a block the (stale, replay-rebuilt)
		 * allocator considers free but that is actually in use as file
		 * data -- briefs_trie_page_init() zeroes it and writes the
		 * TRNP magic, clobbering the file's data block.  generic/029,
		 * 030, and 032 all hit this on a clean unmount+remount (the
		 * file's block-at-offset-0 becomes a trie page; 030 subtest 3
		 * loses the whole file).  Dropping the ->dirty guard makes a
		 * clean unmount always advance log_start to log_end, so the
		 * journal is clean on disk and remount replays nothing.
		 * __briefs_journal_checkpoint_locked() is a no-op-plus-redundant
		 * checkpoint-block-write when the journal is already clean, so
		 * this is safe for the nothing-pending case.  This fixes the
		 * clean-unmount replay family only; the crash-replay family
		 * (generic/547 #2, dm-thin drop-writes) is separate and still
		 * needs defer-trie-free-until-checkpoint.
		 */
		if (bsi->journal && !sb_rdonly(sb)) {
			int ckret = briefs_journal_checkpoint(bsi->journal);
			if (ckret)
				pr_err("briefs: unmount checkpoint failed (err=%d); journal may replay on next mount\n",
				       ckret);
		}

		/* Sync allocation trie to disk */
		if (!sb_rdonly(sb)) {
			pr_info("briefs: syncing allocation trie\n");
			briefs_alloc_sync(&bsi->alloc);

			/*
			 * Persist superblock free counts. briefs_journal_sync_superblock
			 * refreshes them from the allocator state first.
			 */
			if (bsi->journal)
				briefs_journal_sync_superblock(bsi->journal);
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

		if (bsi->sb_bh) {
			brelse(bsi->sb_bh);
			bsi->sb_bh = NULL;
			bsi->sb = NULL;
		}
	}

	briefs_trie_cleanup_state(sb);

	pr_info("briefs: put_super\n");
}
/* shamelessly yoinking from xiafs - incomplete */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf) {
	struct super_block *sb = dentry->d_sb;
	struct briefs_sb_info *sbi = briefs_sb(sb);
	u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
	buf->f_type = sb->s_magic;
	buf->f_bsize = sb->s_blocksize;
	/*
	 * The allocator state is authoritative for free space; the superblock
	 * cached values may be stale between checkpoints.
	 */
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
