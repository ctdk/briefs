// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * BrieFS sysfs surface: a small set of stable, one-value-per-file, read-only
 * attributes per mounted superblock at /sys/fs/briefs/<s_id>/.
 *
 * Unlike the debugfs tree, sysfs is created for EVERY mount (not gated by
 * -o debug): these are the cheap, stable attributes a tool would want on a
 * normal mount (label, uuid, version, sizes, free counts). The richer
 * diagnostic dump lives in debugfs under -o debug.
 *
 * Layout: briefs_init creates /sys/fs/briefs/ (briefs_fs_kobj under fs_kobj).
 * Each fill_super adds a per-sb kobject <s_id> there (the embedded
 * bsi->s_kobj) and a group of attribute files under it. put_super removes the
 * group and the kobject before tearing bsi down.
 *
 * Lifecycle note (important): the per-sb kobject is EMBEDDED in bsi, and its
 * ktype release is a no-op. bsi is freed by the fill_super error path (and is,
 * pre-existing-ly, not freed on the unmount path at all); the kobject is only
 * the sysfs registration handle, NOT the owner of bsi's memory. Because bsi is
 * never kfree'd on unmount, the embedded kobject memory stays valid even if a
 * sysfs file descriptor lingers open past unmount — so the no-op release is
 * safe. put_super still does kobject_del first so that in-flight show ops drain
 * (kernfs_deactivate waits) before the existing teardown touches bsi.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include "briefs.h"

static struct kobject *briefs_fs_kobj;

/* ---- per-sb ktype ------------------------------------------------------ */

/*
 * No-op: bsi's lifetime is managed by fill_super/put_super, not this kobject
 * (see the file header). We define release only so the kernel does not print
 * the "does not have a release() function, it is broken" warning for a NULL
 * ktype->release.
 */
static void briefs_sb_kobj_release(struct kobject *kobj)
{
}

static struct kobj_type briefs_sb_ktype = {
	.release	= briefs_sb_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
};

/* ---- attribute show helpers ------------------------------------------- */

/* bsi->s_kobj is embedded in bsi; recover the containing bsi. */
static inline struct briefs_sb_info *briefs_kobj_to_bsi(struct kobject *kobj)
{
	return container_of(kobj, struct briefs_sb_info, s_kobj);
}

static ssize_t label_show(struct kobject *kobj, struct kobj_attribute *attr,
			  char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	struct briefs_superblock *s = bsi->sb;

	if (!s)
		return sprintf(buf, "\n");
	/* label is fixed-width 64 bytes, may be 0-terminated early */
	return sprintf(buf, "%.64s\n", s->label);
}

static ssize_t uuid_show(struct kobject *kobj, struct kobj_attribute *attr,
			 char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	struct briefs_superblock *s = bsi->sb;

	if (!s)
		return sprintf(buf, "\n");
	return sprintf(buf, "%pU\n", s->uuid);
}

static ssize_t version_show(struct kobject *kobj, struct kobj_attribute *attr,
			    char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	struct briefs_superblock *s = bsi->sb;

	if (!s)
		return sprintf(buf, "0.0.0\n");
	return sprintf(buf, "%llu.%llu.%llu\n", le64_to_cpu(s->major_version),
		       le64_to_cpu(s->minor_version), le64_to_cpu(s->patch_version));
}

static ssize_t block_size_show(struct kobject *kobj, struct kobj_attribute *attr,
			       char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	struct briefs_superblock *s = bsi->sb;

	if (!s)
		return sprintf(buf, "0\n");
	return sprintf(buf, "%llu\n", le64_to_cpu(s->block_size));
}

static ssize_t total_blocks_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	u64 total;

	mutex_lock(&bsi->alloc.lock);
	total = bsi->alloc.block_count;
	mutex_unlock(&bsi->alloc.lock);
	return sprintf(buf, "%llu\n", total);
}

static ssize_t free_blocks_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	u64 free;

	/* Authoritative live count from the allocator (mirror briefs_statfs),
	 * not the stale cached bsi->free_data_blocks. */
	mutex_lock(&bsi->alloc.lock);
	free = bsi->alloc.free_count;
	mutex_unlock(&bsi->alloc.lock);
	return sprintf(buf, "%llu\n", free);
}

static ssize_t total_inodes_show(struct kobject *kobj,
				 struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	u64 total;

	mutex_lock(&bsi->inode_alloc.lock);
	total = bsi->inode_alloc.block_count;
	mutex_unlock(&bsi->inode_alloc.lock);
	return sprintf(buf, "%llu\n", total);
}

static ssize_t free_inodes_show(struct kobject *kobj,
				struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	u64 free;

	mutex_lock(&bsi->inode_alloc.lock);
	free = bsi->inode_alloc.free_count;
	mutex_unlock(&bsi->inode_alloc.lock);
	return sprintf(buf, "%llu\n", free);
}

static ssize_t feature_incompat_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);
	struct briefs_superblock *s = bsi->sb;

	if (!s)
		return sprintf(buf, "0x0\n");
	return sprintf(buf, "0x%016llx\n", le64_to_cpu(s->feature_incompat));
}

static ssize_t mount_time_show(struct kobject *kobj,
			       struct kobj_attribute *attr, char *buf)
{
	struct briefs_sb_info *bsi = briefs_kobj_to_bsi(kobj);

	/* Seconds since boot at mount, same basis as debugfs mount_info. */
	return sprintf(buf, "%llu\n", bsi->mount_jiffies / HZ);
}

static struct kobj_attribute label_attr = __ATTR_RO(label);
static struct kobj_attribute uuid_attr = __ATTR_RO(uuid);
static struct kobj_attribute version_attr = __ATTR_RO(version);
static struct kobj_attribute block_size_attr = __ATTR_RO(block_size);
static struct kobj_attribute total_blocks_attr = __ATTR_RO(total_blocks);
static struct kobj_attribute free_blocks_attr = __ATTR_RO(free_blocks);
static struct kobj_attribute total_inodes_attr = __ATTR_RO(total_inodes);
static struct kobj_attribute free_inodes_attr = __ATTR_RO(free_inodes);
static struct kobj_attribute feature_incompat_attr = __ATTR_RO(feature_incompat);
static struct kobj_attribute mount_time_attr = __ATTR_RO(mount_time);

static struct attribute *briefs_sb_attrs[] = {
	&label_attr.attr,
	&uuid_attr.attr,
	&version_attr.attr,
	&block_size_attr.attr,
	&total_blocks_attr.attr,
	&free_blocks_attr.attr,
	&total_inodes_attr.attr,
	&free_inodes_attr.attr,
	&feature_incompat_attr.attr,
	&mount_time_attr.attr,
	NULL,
};

static const struct attribute_group briefs_sb_attr_group = {
	.attrs = briefs_sb_attrs,
};

/* ---- module-level init/exit ------------------------------------------- */

int briefs_sysfs_init(void)
{
	/* fs_kobj is /sys/fs (EXPORT_SYMBOL_GPL); creates /sys/fs/briefs/. */
	briefs_fs_kobj = kobject_create_and_add("briefs", fs_kobj);
	if (!briefs_fs_kobj)
		return -ENOMEM;
	return 0;
}

void briefs_sysfs_exit(void)
{
	kobject_put(briefs_fs_kobj);
	briefs_fs_kobj = NULL;
}

/* ---- per-sb add/remove ------------------------------------------------- */

int briefs_sysfs_add_sb(struct super_block *sb)
{
	struct briefs_sb_info *bsi = briefs_sb(sb);
	int ret;

	/* Init the embedded kobject (refcount=1, ktype set) then register it
	 * under /sys/fs/briefs/ named after the VFS s_id (e.g. "loop0").
	 *
	 * Best-effort: sysfs is observability, not correctness, so an ENOMEM here
	 * must not fail the mount. On failure we drop the init ref (no-op release)
	 * and leave the kobject unregistered; briefs_sysfs_remove_sb guards on
	 * state_in_sysfs and becomes a no-op. */
	kobject_init(&bsi->s_kobj, &briefs_sb_ktype);
	ret = kobject_add(&bsi->s_kobj, briefs_fs_kobj, "%s", sb->s_id);
	if (ret) {
		pr_warn("briefs: sysfs kobject_add failed: %d (continuing)\n", ret);
		kobject_put(&bsi->s_kobj);
		return 0;
	}

	ret = sysfs_create_group(&bsi->s_kobj, &briefs_sb_attr_group);
	if (ret) {
		pr_warn("briefs: sysfs_create_group failed: %d (continuing)\n", ret);
		kobject_del(&bsi->s_kobj);
		kobject_put(&bsi->s_kobj);
		return 0;
	}
	return 0;
}

void briefs_sysfs_remove_sb(struct super_block *sb)
{
	struct briefs_sb_info *bsi = briefs_sb(sb);

	/* Only tear down if add actually registered the kobject. kobject_del
	 * drops the sysfs dir and waits for in-flight show ops (kernfs_deactivate)
	 * to drain, so once we return no sysfs reader can touch bsi. */
	if (!bsi->s_kobj.state_in_sysfs)
		return;

	sysfs_remove_group(&bsi->s_kobj, &briefs_sb_attr_group);
	kobject_del(&bsi->s_kobj);
	kobject_put(&bsi->s_kobj);
}