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

#include "compat-fileattr.h"
#include "compat-iomap.h"
#include "compat-buffer.h"
#include "compat-vfs-ops.h"

#endif /* _BRIEFS_COMPAT_H */