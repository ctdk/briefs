// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * BrieFS /proc surface: /proc/fs/briefs/mounts, one line per mounted BrieFS
 * superblock. Matches the conventional /proc/fs/<fstype>/ home used by other
 * filesystems; the directory lets more files be added later without moving the
 * path.
 *
 * The line is a cheap, indicative snapshot: it reads scalar fields cached in
 * bsi (free_data_blocks/free_inodes are updated racy during normal operation)
 * and the immutable allocator block_count totals, all under
 * briefs_sb_list_lock. Authoritative live free counts live in sysfs
 * (free_blocks/free_inodes, which lock the allocators) and the rich per-sb
 * dump lives in debugfs under -o debug. So /proc/mounts is the at-a-glance
 * index; sysfs/debugfs are the detailed views.
 *
 * Snapshot design: seq_file show must not sleep (seq_printf can kmalloc/sleep)
 * while briefs_sb_list_lock (spinlock) is held. So start() walks the list once
 * under the spinlock, copying scalars into a kmalloc'd array, drops the lock,
 * and next()/show() iterate that array lock-free. The array is freed in the
 * file release(), so it survives across multi-page start()/stop() resumes
 * within a single open().
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "briefs.h"
#include "briefs_debug.h"

struct briefs_proc_entry {
	char s_id[64];
	u64 total_blocks;
	u64 free_blocks;
	u64 total_inodes;
	u64 free_inodes;
	u64 mount_time;		/* secs since boot at mount */
	int debug;
};

struct briefs_proc_snap {
	unsigned int count;
	struct briefs_proc_entry e[];
};

/* Build a snapshot of the live per-sb list. Returns ERR_PTR(-ENOMEM) on
 * allocation failure. The array is later freed by briefs_proc_release(). */
static struct briefs_proc_snap *briefs_proc_build_snap(void)
{
	struct briefs_proc_snap *snap;
	struct briefs_sb_info *bsi;
	unsigned int n = 0, i = 0;

	/* First pass: count under the spinlock. */
	spin_lock(&briefs_sb_list_lock);
	list_for_each_entry(bsi, &briefs_sb_list, sb_list)
		n++;
	spin_unlock(&briefs_sb_list_lock);

	snap = kvmalloc_array(n + 1, sizeof(snap->e[0]), GFP_KERNEL);
	if (!snap)
		return ERR_PTR(-ENOMEM);
	snap->count = 0;

	/* Second pass: copy scalars under the spinlock (no sleeping here). */
	spin_lock(&briefs_sb_list_lock);
	list_for_each_entry(bsi, &briefs_sb_list, sb_list) {
		struct super_block *sb;

		if (i >= n)
			break;	/* list grew then shrank? just stop */
		sb = bsi->alloc.sb;
		strscpy(snap->e[i].s_id, sb ? sb->s_id : "?", sizeof(snap->e[i].s_id));
		snap->e[i].total_blocks = bsi->alloc.block_count;
		snap->e[i].free_blocks = bsi->free_data_blocks;
		snap->e[i].total_inodes = bsi->inode_alloc.block_count;
		snap->e[i].free_inodes = bsi->free_inodes;
		snap->e[i].mount_time = bsi->mount_jiffies / HZ;
		snap->e[i].debug = (bsi->mount_flags & BRIEFS_MF_DEBUG) ? 1 : 0;
		i++;
	}
	spin_unlock(&briefs_sb_list_lock);
	snap->count = i;
	return snap;
}

static void *briefs_proc_start(struct seq_file *m, loff_t *pos)
{
	struct briefs_proc_snap *snap = m->private;

	if (!snap) {
		snap = briefs_proc_build_snap();
		if (IS_ERR(snap))
			return snap;
		m->private = snap;
	}
	if (*pos >= snap->count)
		return NULL;
	return &snap->e[*pos];
}

static void *briefs_proc_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct briefs_proc_snap *snap = m->private;

	++*pos;
	if (*pos >= snap->count)
		return NULL;
	return &snap->e[*pos];
}

static int briefs_proc_show(struct seq_file *m, void *v)
{
	struct briefs_proc_entry *e = v;

	seq_printf(m,
		   "%s\ttotal_blocks=%llu free_blocks=%llu total_inodes=%llu free_inodes=%llu debug=%d mount_time=%llu\n",
		   e->s_id, e->total_blocks, e->free_blocks, e->total_inodes,
		   e->free_inodes, e->debug, e->mount_time);
	return 0;
}

static void briefs_proc_stop(struct seq_file *m, void *v)
{
	/* Nothing: the snapshot lives until release() so multi-page reads can
	 * resume into the same array. */
}

static const struct seq_operations briefs_proc_seq_ops = {
	.start	= briefs_proc_start,
	.next	= briefs_proc_next,
	.show	= briefs_proc_show,
	.stop	= briefs_proc_stop,
};

static int briefs_proc_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &briefs_proc_seq_ops);
}

static int briefs_proc_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	kvfree(m->private);
	m->private = NULL;
	return seq_release(inode, file);
}

static const struct proc_ops briefs_proc_pops = {
	.proc_open	= briefs_proc_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= briefs_proc_release,
};

/* ---- module-level init/exit ------------------------------------------- */

static struct proc_dir_entry *briefs_proc_dir;

int briefs_proc_init(void)
{
	/* proc_mkdir("fs/briefs", NULL) creates the nested /proc/fs/briefs/. */
	briefs_proc_dir = proc_mkdir("fs/briefs", NULL);
	if (!briefs_proc_dir)
		return -ENOMEM;

	if (!proc_create("mounts", 0444, briefs_proc_dir, &briefs_proc_pops))
		return -ENOMEM;
	return 0;
}

void briefs_proc_exit(void)
{
	if (briefs_proc_dir) {
		/* proc_remove drops the dir and everything under it (mounts),
		 * by pde, so we don't depend on slash-path lookup. */
		proc_remove(briefs_proc_dir);
		briefs_proc_dir = NULL;
	}
}