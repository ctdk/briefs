// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * BrieFS observability: per-superblock debugfs tree, the global per-sb list,
 * and the stat-counter helper backing the debugfs "stats" file. The per-sb
 * list is also consumed by briefs_sysfs.c and briefs_proc.c.
 *
 * debugfs is gated to mounts that pass -o debug (BRIEFS_MF_DEBUG): only those
 * mounts get a per-sb directory under /sys/kernel/debug/briefs/<s_id>/ and
 * only they pay for the stat-counter increments (briefs_stat_inc is a skipped
 * branch otherwise). Mount-option parsing lives in briefs_super.c; this file
 * only reports the active options via briefs_show_options().
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/printk.h>
#include "briefs.h"
#include "briefs_journal.h"
#include "briefs_debug.h"

/* ---- global per-superblock list (also used by sysfs/proc) -------------- */

LIST_HEAD(briefs_sb_list);
DEFINE_SPINLOCK(briefs_sb_list_lock);

void briefs_sb_list_add(struct briefs_sb_info *bsi)
{
	INIT_LIST_HEAD(&bsi->sb_list);
	spin_lock(&briefs_sb_list_lock);
	list_add(&bsi->sb_list, &briefs_sb_list);
	spin_unlock(&briefs_sb_list_lock);
}

void briefs_sb_list_del(struct briefs_sb_info *bsi)
{
	spin_lock(&briefs_sb_list_lock);
	list_del_init(&bsi->sb_list);
	spin_unlock(&briefs_sb_list_lock);
}

/* ---- show active mount options ---------------------------------------- */

int briefs_show_options(struct seq_file *seq, struct dentry *root)
{
	struct briefs_sb_info *bsi = briefs_sb(root->d_sb);

	if (bsi && (bsi->mount_flags & BRIEFS_MF_DEBUG))
		seq_show_option(seq, "debug", NULL);
	return 0;
}

/* ---- debugfs show helpers --------------------------------------------- */

static void briefs_format_uuid(const __u8 *u, char *buf, size_t len)
{
	snprintf(buf, len,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static int briefs_df_mount_info_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	u64 msecs;

	if (!bsi)
		return 0;
	msecs = bsi->mount_jiffies;
	seq_printf(m, "device=%s\n", sb->s_id);
	seq_printf(m, "read_only=%d\n", sb_rdonly(sb) ? 1 : 0);
	seq_printf(m, "debug=%d\n", (bsi->mount_flags & BRIEFS_MF_DEBUG) ? 1 : 0);
	seq_printf(m, "mount_jiffies=%llu\n", msecs);
	seq_printf(m, "mount_secs_since_boot=%llu\n", msecs / HZ);
	return 0;
}

static int briefs_df_superblock_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct briefs_superblock *s;
	char uuid[37];

	if (!bsi || !bsi->sb)
		return 0;
	s = bsi->sb;
	briefs_format_uuid(s->uuid, uuid, sizeof(uuid));

	seq_printf(m, "magic=0x%016llx\n", le64_to_cpu(s->magic));
	seq_printf(m, "version=%llu.%llu.%llu\n", le64_to_cpu(s->major_version),
		   le64_to_cpu(s->minor_version), le64_to_cpu(s->patch_version));
#ifdef BRIEFS_BUILD_VERSION
	seq_printf(m, "build=%s\n", BRIEFS_BUILD_VERSION);
#else
	seq_puts(m, "build=unknown\n");
#endif
	seq_printf(m, "total_blocks=%llu\n", le64_to_cpu(s->total_blocks));
	seq_printf(m, "data_blocks=%llu\n", le64_to_cpu(s->data_blocks));
	seq_printf(m, "block_size=%llu\n", le64_to_cpu(s->block_size));
	seq_printf(m, "inode_size=%llu\n", le64_to_cpu(s->inode_size));
	seq_printf(m, "blocks_per_group=%llu\n", le64_to_cpu(s->blocks_per_group));
	seq_printf(m, "inodes_per_group=%llu\n", le64_to_cpu(s->inodes_per_group));
	seq_printf(m, "root_inode=%llu\n", le64_to_cpu(s->root_inode_number));
	seq_printf(m, "free_data_blocks=%llu\n", le64_to_cpu(s->free_data_blocks));
	seq_printf(m, "free_inodes=%llu\n", le64_to_cpu(s->free_inodes));
	seq_printf(m, "feature_compat=0x%016llx\n", le64_to_cpu(s->feature_compat));
	seq_printf(m, "feature_ro_compat=0x%016llx\n", le64_to_cpu(s->feature_ro_compat));
	seq_printf(m, "feature_incompat=0x%016llx\n", le64_to_cpu(s->feature_incompat));
	seq_printf(m, "uuid=%s\n", uuid);
	seq_printf(m, "label=%.64s\n", s->label);
	seq_printf(m, "fs_created=%llu\n", le64_to_cpu(s->fs_created));
	seq_printf(m, "fs_last_mounted=%llu\n", le64_to_cpu(s->fs_last_mounted));
	seq_printf(m, "fs_last_checkpoint=%llu\n", le64_to_cpu(s->fs_last_checkpoint));
	seq_printf(m, "trie_root_block=%llu\n", le64_to_cpu(s->trie_root_block));
	seq_printf(m, "trie_blocks_used=%llu\n", le64_to_cpu(s->trie_blocks_used));
	seq_printf(m, "trie_node_pool_start=%llu\n", le64_to_cpu(s->trie_node_pool_start));
	seq_printf(m, "trie_node_pool_size=%llu\n", le64_to_cpu(s->trie_node_pool_size));
	seq_printf(m, "inode_bitmap_offset=%llu\n", le64_to_cpu(s->inode_bitmap_offset));
	seq_printf(m, "inode_bitmap_blocks=%llu\n", le64_to_cpu(s->inode_bitmap_blocks));
	seq_printf(m, "inode_table_offset=%llu\n", le64_to_cpu(s->inode_table_offset));
	seq_printf(m, "journal_offset=%llu\n", le64_to_cpu(s->journal_offset));
	seq_printf(m, "journal_blocks=%llu\n", le64_to_cpu(s->journal_blocks));
	seq_printf(m, "checkpoint_seq=%llu\n", le64_to_cpu(s->checkpoint_seq));
	seq_printf(m, "journal_log_start=%llu\n", le64_to_cpu(s->journal_log_start));
	seq_printf(m, "journal_log_end=%llu\n", le64_to_cpu(s->journal_log_end));
	return 0;
}

static int briefs_df_data_alloc_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct briefs_alloc *a;
	u64 bc, fc, rover;

	if (!bsi)
		return 0;
	a = &bsi->alloc;
	mutex_lock(&a->lock);
	bc = a->block_count;
	fc = a->free_count;
	rover = a->rover_w0;
	mutex_unlock(&a->lock);

	seq_printf(m, "block_count=%llu\n", bc);
	seq_printf(m, "free_count=%llu\n", fc);
	seq_printf(m, "used_count=%llu\n", bc - fc);
	seq_printf(m, "l0_words=%llu\n", a->l0_words);
	seq_printf(m, "l1_words=%llu\n", a->l1_words);
	seq_printf(m, "l2_words=%llu\n", a->l2_words);
	seq_printf(m, "alloc_pool_start=%llu\n", a->alloc_pool_start);
	seq_printf(m, "rover_w0=%llu\n", rover);
	return 0;
}

static int briefs_df_inode_alloc_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct briefs_alloc *a;
	u64 bc, fc, rover;

	if (!bsi)
		return 0;
	a = &bsi->inode_alloc;
	mutex_lock(&a->lock);
	bc = a->block_count;
	fc = a->free_count;
	rover = a->rover_w0;
	mutex_unlock(&a->lock);

	seq_printf(m, "block_count=%llu\n", bc);
	seq_printf(m, "free_count=%llu\n", fc);
	seq_printf(m, "used_count=%llu\n", bc - fc);
	seq_printf(m, "l0_words=%llu\n", a->l0_words);
	seq_printf(m, "l1_words=%llu\n", a->l1_words);
	seq_printf(m, "l2_words=%llu\n", a->l2_words);
	seq_printf(m, "alloc_pool_start=%llu\n", a->alloc_pool_start);
	seq_printf(m, "rover_w0=%llu\n", rover);
	return 0;
}

static int briefs_df_journal_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct briefs_journal *j;
	u64 wp, sp;
	u32 rsc;
	u64 cs;
	bool dirty;

	if (!bsi)
		return 0;
	j = bsi->journal;
	if (!j) {
		seq_puts(m, "journal: (none)\n");
		return 0;
	}

	/* No journal-wide lock: indicative, racy reads (documented). */
	wp = READ_ONCE(j->write_pos);
	sp = READ_ONCE(j->synced_pos);
	rsc = READ_ONCE(j->records_since_checkpoint);
	cs = READ_ONCE(j->checkpoint_seq);
	dirty = READ_ONCE(j->dirty);

	seq_printf(m, "write_pos=%llu\n", wp);
	seq_printf(m, "synced_pos=%llu\n", sp);
	seq_printf(m, "outstanding=%llu\n", wp - sp);
	seq_printf(m, "checkpoint_seq=%llu\n", cs);
	seq_printf(m, "records_since_checkpoint=%u (interval %d)\n",
		   rsc, JRN_CHECKPOINT_INTERVAL);
	seq_printf(m, "dirty=%d\n", dirty ? 1 : 0);
	seq_printf(m, "journal_start=%llu\n", j->journal_start);
	seq_printf(m, "journal_end=%llu\n", j->journal_end);
	seq_printf(m, "checkpoint_block=%llu\n", j->checkpoint_block);
	return 0;
}

static int briefs_df_trie_pool_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;

	seq_printf(m, "partial_pages=%u\n", briefs_trie_pool_depth(sb));
	return 0;
}

static int briefs_df_stats_show(struct seq_file *m, void *v)
{
	struct super_block *sb = m->private;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct briefs_stats *s;

	if (!bsi)
		return 0;
	s = &bsi->stats;

	seq_printf(m, "data_alloc_calls=%lld\n", atomic64_read(&s->data_alloc_calls));
	seq_printf(m, "data_free_calls=%lld\n", atomic64_read(&s->data_free_calls));
	seq_printf(m, "inode_alloc_calls=%lld\n", atomic64_read(&s->inode_alloc_calls));
	seq_printf(m, "inode_free_calls=%lld\n", atomic64_read(&s->inode_free_calls));
	seq_printf(m, "journal_records=%lld\n", atomic64_read(&s->journal_records));
	seq_printf(m, "journal_checkpoints=%lld\n", atomic64_read(&s->journal_checkpoints));
	seq_printf(m, "journal_replay_records=%lld\n", atomic64_read(&s->journal_replay_records));
	seq_printf(m, "btree_splits=%lld\n", atomic64_read(&s->btree_splits));
	seq_printf(m, "btree_extents_added=%lld\n", atomic64_read(&s->btree_extents_added));
	seq_printf(m, "btree_extents_freed=%lld\n", atomic64_read(&s->btree_extents_freed));
	seq_printf(m, "dir_adds=%lld\n", atomic64_read(&s->dir_adds));
	seq_printf(m, "dir_dels=%lld\n", atomic64_read(&s->dir_dels));
	seq_printf(m, "truncate_calls=%lld\n", atomic64_read(&s->truncate_calls));
	seq_printf(m, "fallocate_calls=%lld\n", atomic64_read(&s->fallocate_calls));
	seq_printf(m, "punch_holes=%lld\n", atomic64_read(&s->punch_holes));
	return 0;
}

DEFINE_SHOW_ATTRIBUTE(briefs_df_mount_info);
DEFINE_SHOW_ATTRIBUTE(briefs_df_superblock);
DEFINE_SHOW_ATTRIBUTE(briefs_df_data_alloc);
DEFINE_SHOW_ATTRIBUTE(briefs_df_inode_alloc);
DEFINE_SHOW_ATTRIBUTE(briefs_df_journal);
DEFINE_SHOW_ATTRIBUTE(briefs_df_trie_pool);
DEFINE_SHOW_ATTRIBUTE(briefs_df_stats);

/* ---- debugfs root + per-sb directory ---------------------------------- */

static struct dentry *briefs_debugfs_root;

int briefs_debugfs_init(void)
{
	briefs_debugfs_root = debugfs_create_dir("briefs", NULL);
	if (IS_ERR(briefs_debugfs_root)) {
		int ret = PTR_ERR(briefs_debugfs_root);
		briefs_debugfs_root = NULL;
		return ret;
	}
	return 0;
}

void briefs_debugfs_exit(void)
{
	debugfs_remove_recursive(briefs_debugfs_root);
	briefs_debugfs_root = NULL;
}

int briefs_debugfs_add_sb(struct super_block *sb)
{
	struct briefs_sb_info *bsi = briefs_sb(sb);
	struct dentry *dir;

	if (!bsi || !(bsi->mount_flags & BRIEFS_MF_DEBUG))
		return 0;
	if (!briefs_debugfs_root)
		return 0;

	dir = debugfs_create_dir(sb->s_id, briefs_debugfs_root);
	if (IS_ERR(dir))
		return PTR_ERR(dir);
	bsi->debugfs_dir = dir;

	debugfs_create_file("mount_info", 0444, dir, sb, &briefs_df_mount_info_fops);
	debugfs_create_file("superblock", 0444, dir, sb, &briefs_df_superblock_fops);
	debugfs_create_file("data_alloc", 0444, dir, sb, &briefs_df_data_alloc_fops);
	debugfs_create_file("inode_alloc", 0444, dir, sb, &briefs_df_inode_alloc_fops);
	debugfs_create_file("journal", 0444, dir, sb, &briefs_df_journal_fops);
	debugfs_create_file("trie_pool", 0444, dir, sb, &briefs_df_trie_pool_fops);
	debugfs_create_file("stats", 0444, dir, sb, &briefs_df_stats_fops);
	return 0;
}

void briefs_debugfs_remove_sb(struct super_block *sb)
{
	struct briefs_sb_info *bsi = briefs_sb(sb);

	if (!bsi || !bsi->debugfs_dir)
		return;
	debugfs_remove_recursive(bsi->debugfs_dir);
	bsi->debugfs_dir = NULL;
}