/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs module - filesystem registration and module metadata */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include "briefs.h"

/* Filesystem type structure */
static struct file_system_type briefs_fs_type = {
	.owner = THIS_MODULE,
	.name = "briefs",
	.fs_flags = FS_REQUIRES_DEV,
	.mount = briefs_mount,
};

/* Module init */
static int __init briefs_init(void) {
	int ret;

	pr_info("briefs: loading filesystem module\n");
	pr_info("briefs: magic=0x%016llx, block_size=%d, inode_size=%d\n",
		(unsigned long long)_BRIEFS_SUPER_MAGIC, BRIEFS_BLOCK_SIZE, BRIEFS_INODE_SIZE);

	ret = register_filesystem(&briefs_fs_type);
	if (ret) {
		pr_err("briefs: failed to register filesystem: %d\n", ret);
		return ret;
	}

	pr_info("briefs: filesystem registered successfully\n");
	return 0;
}

/* Module exit */
static void __exit briefs_exit(void) {
	pr_info("briefs: unregistering filesystem\n");
	unregister_filesystem(&briefs_fs_type);
	pr_info("briefs: unloading filesystem module\n");
}

module_init(briefs_init);
module_exit(briefs_exit);

MODULE_AUTHOR("Jeremy Bingham");
MODULE_DESCRIPTION("BrieFS - extend-only, trie-based filesystem.");
MODULE_LICENSE("Dual MIT/GPL");
