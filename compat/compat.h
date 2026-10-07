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
 * BRIEFS_HAS_CGROUPWB_FIX below is a bugfix-presence gate, not an
 * API-shape boundary: it tracks whether the kernel's inode_switch_wbs
 * machinery is safe enough for BrieFS to opt into cgroup writeback
 * (SB_I_CGROUPWB).  Since 2026-10-07 it is a plain >= 7.2 boundary:
 * the per-sb isw rework (31c1d19ead2c) is mainline-only from 7.2-rc1
 * onward, and the v1-only stable backports (6.12.96, 6.18.39, 7.1.4)
 * were proven insufficient by the 6.12.111 gate crash.  If Debian ever
 * cherry-picked the rework into a lower patchlevel, the flag would stay
 * 0 there -- conservative: generic/563 keeps failing but no crash
 * risk.  The build-matrix flag probe asserts the boundary per target.
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
 * Cgroup-writeback wb-switch bugfixes.  BrieFS must not set
 * SB_I_CGROUPWB unless the kernel's inode_switch_wbs machinery is
 * safe, because the cgwb-offline path can otherwise queue or run an
 * isw against freed BrieFS inode memory and wedge the whole VM
 * (every umount then hangs in cgroup_writeback_umount()'s
 * flush_workqueue).
 *
 * History (as of 2026-10-07): the wb-switch race fix generation that
 * reached the stable trees is only v1 -- cba38ec4cbd3 ("writeback: fix
 * race between cgroup_writeback_umount() and inode_switch_wbs()":
 * counter-before-grab ordering + rcu extensions + synchronize_rcu()
 * in cgroup_writeback_umount; backported 6.12.96 c923cc3cb5cd,
 * 6.18.39, 7.1.4, 6.6.145, 6.1.178, 5.10.y).  The follow-up per-sb
 * rework of the same series (31c1d19ead2c "writeback: use a per-sb
 * counter to drain inode wb switches at umount", plus its 1/3-2/3
 * siblings) was deliberately mainline-only: merged 7.2-rc1 via
 * vfs-7.2-rc1.writeback, never backported.  CVE-2026-31703
 * (6689f01d6740, stable 6.12.94 via 156cc63691c1) predates v1.
 *
 * The 2026-10-07 full-suite gate on stock Debian 6.12.111 proved v1
 * insufficient for the cgwb-offline (memcg teardown) path: an isw
 * referencing an already-freed BrieFS dir inode and its freed
 * super_block crashed inode_switch_wbs_work_fn (GPF in down_read on
 * garbage from the freed sb's s_bdi slot) even though v1 is fully
 * present in that kernel (verified by disassembly).  Forensics in
 * ~/src/briefs-notes/gate-oops-6.12.111-cgwb-isw-forensics.md.
 *
 * Therefore the gate is conservative: only kernels >= 7.2, which have
 * the complete per-sb rework (empirically validated by the clean
 * 2026-10-05 7.3 full-suite round with cgwb enabled), may set
 * SB_I_CGROUPWB.  generic/563 stays an accepted FAIL everywhere else.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 2, 0)
#define BRIEFS_HAS_CGROUPWB_FIX 1	/* per-sb isw drain present */
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