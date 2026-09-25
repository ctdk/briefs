/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_H
#define _BRIEFS_COMPAT_H

/*
 * compat/compat.h - consolidated kernel compatibility layer.
 *
 * This directory is the ONLY place in the tree where LINUX_VERSION_CODE
 * is consulted.  Upstream API changes between the 6.12 floor (Debian
 * trixie, the runtime-validated target) and current linux-master are
 * expressed here as named feature flags plus stable, version-agnostic
 * wrapper names (briefs_compat_*), so that no .c file needs its own
 * #ifdef: every call site, ops table and prototype outside compat/ is
 * written once, against the stable names.
 *
 * This umbrella header is force-included by every compilation unit
 * (Makefile: ccflags-y += -include $(src)/compat/compat.h), ahead of
 * each file's own #include list, so the flags and wrappers are visible
 * no matter what a .c file includes first.  briefs.h additionally
 * includes it first so it stays self-contained.  Topic headers below
 * must be self-sufficient (they include the kernel headers they use)
 * because this file precedes every other include.
 *
 * Version checks rather than compile-time feature probes are deliberate:
 * struct-shape and prototype changes cannot be preprocessor-probed at
 * all, Debian does not backport in-tree API-shape changes into older
 * series, and the cross-kernel build matrix (tests/build-matrix.sh) turns
 * a wrong boundary into a compile error, never a silent break.
 *
 * BRIEFS_HAS_CGROUPWB_FIX below is the one deliberate exception: it is
 * keyed to stable-bugfix backport patchlevels (6.12.96, 6.18.39, 7.1.4),
 * not an API-shape boundary.  Debian ships upstream stable point
 * releases, so LINUX_VERSION_CODE still answers the question exactly.
 * If Debian ever cherry-picked the fix into a lower patchlevel, the
 * flag would stay 0 there -- conservative: generic/563 keeps failing
 * but no crash risk.  The build-matrix flag probe asserts the boundary
 * per target.
 */

#include <linux/version.h>

/*
 * Feature flags.  Each marks an upstream API change BrieFS consumes;
 * the wrapper arms are keyed on these, never on raw version checks.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 15, 0)
#define BRIEFS_HAS_MKDIR_DENTRY 1	/* i_op->mkdir returns dentry * */
#else
#define BRIEFS_HAS_MKDIR_DENTRY 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 16, 0)
#define BRIEFS_HAS_IOMAP_PRIVATE 1	/* private threaded through zero/mkwrite */
#else
#define BRIEFS_HAS_IOMAP_PRIVATE 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define BRIEFS_HAS_IOMAP_WRITE_OPS 1	/* iomap buffered-write series */
#else
#define BRIEFS_HAS_IOMAP_WRITE_OPS 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define BRIEFS_HAS_IOMAP_WRITEBACK_RANGE 1	/* writeback ops rework */
#else
#define BRIEFS_HAS_IOMAP_WRITEBACK_RANGE 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 17, 0)
#define BRIEFS_HAS_FILE_KATTR 1		/* struct fileattr renamed */
#else
#define BRIEFS_HAS_FILE_KATTR 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
#define BRIEFS_HAS_IOMAP_BIO_READ 1	/* iomap read rework */
#else
#define BRIEFS_HAS_IOMAP_BIO_READ 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 19, 0)
#define BRIEFS_HAS_INODE_STATE_HELPERS 1	/* i_state behind typed accessors */
#else
#define BRIEFS_HAS_INODE_STATE_HELPERS 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define BRIEFS_HAS_ACL_TO_XATTR_ALLOC 1	/* posix_acl_to_xattr allocs */
#else
#define BRIEFS_HAS_ACL_TO_XATTR_ALLOC 0
#endif

/*
 * 7.0 made f_op->setlease mandatory: kernel_setlease() lost its
 * generic fallback (2b10994be716, "filelock: default to returning
 * -EINVAL when ->setlease operation is NULL"), so a filesystem
 * without the member gets -EINVAL from fcntl(F_SETLEASE).  Before
 * 7.0 the fallback called generic_setlease() itself, so the gated
 * .setlease initializers in ops.c are a no-op change for older
 * kernels.  ext4 and xfs declare the member on their file and
 * directory tables alike.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define BRIEFS_HAS_MANDATORY_SETLEASE 1	/* f_op->setlease mandatory */
#else
#define BRIEFS_HAS_MANDATORY_SETLEASE 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
#define BRIEFS_HAS_BH_SUBMIT 1		/* buffer_head -> bio conversion */
#else
#define BRIEFS_HAS_BH_SUBMIT 0
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 3, 0)
#define BRIEFS_HAS_CREATE_NO_EXCL 1	/* i_op->create dropped bool excl */
#else
#define BRIEFS_HAS_CREATE_NO_EXCL 0
#endif

/*
 * Cgroup-writeback wb-switch bugfixes.  Both upstream bugs below must
 * be fixed in a kernel before BrieFS sets SB_I_CGROUPWB (see the
 * consumer in super.c, which follows the iomap.c writeback-arm
 * precedent for a #if BRIEFS_HAS_* region outside compat/):
 *
 *   CVE-2026-31703: wb use-after-free in inode_switch_wbs_work_fn()
 *     (upstream 6689f01d6740, mainline 7.1-rc1; stable 6.12.94 via
 *     156cc63691c1, 6.18.25, 7.0.2, 6.1.178, 6.6.147)
 *   CVE-2026-64378: cgroup_writeback_umount() vs inode_switch_wbs()
 *     umount race (upstream cba38ec4cbd3 + follow-ups, mainline
 *     7.2-rc1; stable 6.12.96 via c923cc3cb5cd, 6.18.39, 7.1.4,
 *     6.6.145, 6.1.178)
 *
 * Both-fixed: >= 7.2, 7.1.y >= 7.1.4, 6.18.y >= 6.18.39,
 * 6.12.y >= 6.12.96.  7.0.y is excluded entirely: it received only
 * CVE-2026-31703 (7.0.2) before the tree EOL'd -- no CVE-2026-64378
 * backport ever existed.  6.16/6.17 predate the CVE-2026-31703
 * introducing commit (e1b849cfa6b6, v6.18) but never got the
 * umount-race backport either (EOL first); 6.19.y likewise EOL'd
 * unfixed.  6.1.y/6.6.y carry both fixes but sit below BrieFS's 6.12
 * floor.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 1, 4) ||			\
	(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 18, 39) &&		\
	 LINUX_VERSION_CODE <  KERNEL_VERSION(6, 19, 0)) ||		\
	(LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 96) &&		\
	 LINUX_VERSION_CODE <  KERNEL_VERSION(6, 13, 0))
#define BRIEFS_HAS_CGROUPWB_FIX 1	/* both wb-switch bugs fixed */
#else
#define BRIEFS_HAS_CGROUPWB_FIX 0
#endif

#include "compat-fileattr.h"
#include "compat-iomap.h"
#include "compat-buffer.h"
#include "compat-inode.h"
#include "compat-acl.h"
#include "compat-vfs-ops.h"

#endif /* _BRIEFS_COMPAT_H */