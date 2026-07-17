/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs module - filesystem registration and module metadata */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include "briefs.h"

/* Slab cache for inode allocations (larger than struct inode) */
struct kmem_cache *briefs_inode_cachep;

/* Filesystem type structure */
static struct file_system_type briefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "briefs",
	.fs_flags = FS_REQUIRES_DEV,
	.init_fs_context = briefs_init_fs_context,
	.kill_sb = briefs_kill_sb,
	.parameters = briefs_param_spec,
};

/* Inode slab constructor - called for each new slab allocation */
static void briefs_init_once(void *foo) {
	struct briefs_inode_info *binfo = (struct briefs_inode_info *)foo;

	inode_init_once(&binfo->vfs_inode);
}

/* Module init */
static int __init briefs_init(void) {
	int ret;

	pr_info("briefs: loading filesystem module\n");
	pr_debug("briefs: magic=0x%016llx, block_size=%d, inode_size=%d\n",
		 (unsigned long long)_BRIEFS_SUPER_MAGIC, BRIEFS_BLOCK_SIZE, BRIEFS_INODE_SIZE);

	/* Catch on-disk structure size drift vs. Go briefs-utils layout */
	briefs_build_bug_on_sizes();

	/* Create slab cache for inodes */
	briefs_inode_cachep = kmem_cache_create("briefs_inode_cache",
		sizeof(struct briefs_inode_info),
		0, (SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT),
		briefs_init_once);
	if (!briefs_inode_cachep) {
		pr_err("briefs: failed to create inode slab cache\n");
		return -ENOMEM;
	}

	ret = register_filesystem(&briefs_fs_type);
	if (ret) {
		pr_err("briefs: failed to register filesystem: %d\n", ret);
		kmem_cache_destroy(briefs_inode_cachep);
		return ret;
	}

	/* Observability surfaces. All best-effort and non-fatal: a failure here
	 * must not prevent the module from loading — the filesystem still works
	 * without them. Order: debugfs root, /sys/fs/briefs, /proc/fs/briefs. */
	ret = briefs_debugfs_init();
	if (ret)
		pr_warn("briefs: debugfs_init failed: %d (continuing)\n", ret);
	ret = briefs_sysfs_init();
	if (ret)
		pr_warn("briefs: sysfs_init failed: %d (continuing)\n", ret);
	ret = briefs_proc_init();
	if (ret)
		pr_warn("briefs: proc_init failed: %d (continuing)\n", ret);

	pr_debug("briefs: filesystem registered successfully\n");
	return 0;
}

/* Module exit */
static void __exit briefs_exit(void) {
	pr_debug("briefs: unregistering filesystem\n");
	unregister_filesystem(&briefs_fs_type);

	/* Tear down the module-level surfaces in reverse init order. Per-sb
	 * entries were removed in briefs_put_super as each mount went away. */
	briefs_proc_exit();
	briefs_sysfs_exit();
	briefs_debugfs_exit();

	kmem_cache_destroy(briefs_inode_cachep);
	pr_debug("briefs: unloading filesystem module\n");
}

module_init(briefs_init);
module_exit(briefs_exit);

MODULE_AUTHOR("Jeremy Bingham");
MODULE_DESCRIPTION("BrieFS - extent-only, trie-based filesystem.");
MODULE_LICENSE("Dual MIT/GPL");
#ifdef BRIEFS_BUILD_VERSION
MODULE_VERSION(BRIEFS_BUILD_VERSION);
#endif

/* Let `mount -t briefs` (and `modprobe fs-briefs`) auto-load this module via
 * depmod. Without this alias, the module must be insmod'd manually before the
 * first mount -- something xfstests and ordinary users won't do. depmod turns
 * MODULE_ALIAS_FS("briefs") into the "fs-briefs" module alias that the kernel's
 * request_module("fs-briefs") on mount looks up. */
MODULE_ALIAS_FS("briefs");
