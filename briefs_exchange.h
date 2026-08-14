/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_EXCHANGE_H
#define _BRIEFS_EXCHANGE_H

#include <linux/types.h>
#include <linux/ioctl.h>

/*
 * File-range exchange UAPI.
 *
 * The XFS file-range exchange ioctls (XFS_IOC_EXCHANGE_RANGE,
 * XFS_IOC_START_COMMIT, XFS_IOC_COMMIT_RANGE, XFS_IOC_SWAPEXT) are not exposed
 * through a generic kernel UAPI header: the struct layouts live only in the
 * XFS-internal fs/xfs/libxfs/xfs_fs.h.  xfs_io(1) issues these ioctls verbatim,
 * so BrieFS must accept the identical on-the-wire layout.  The structs below
 * are byte-for-byte copies of the XFS UAPI definitions so the _IOW/_IOR ioctl
 * numbers (which encode sizeof) match what userspace sends.  They carry the
 * briefs_ prefix to avoid colliding with any future generic UAPI, but the
 * field order, types, and sizes are identical to xfs_fs.h.
 */

/* xfs_bstime_t (8 + 4, padded to 16) */
struct briefs_bstime {
	__kernel_long_t	tv_sec;
	__s32		tv_nsec;
};

/* struct xfs_bstat -- 136 bytes */
struct briefs_bstat {
	__u64		bs_ino;
	__u16		bs_mode;
	__u16		bs_nlink;
	__u32		bs_uid;
	__u32		bs_gid;
	__u32		bs_rdev;
	__s32		bs_blksize;
	__s64		bs_size;
	struct briefs_bstime bs_atime;
	struct briefs_bstime bs_mtime;
	struct briefs_bstime bs_ctime;
	int64_t		bs_blocks;
	__u32		bs_xflags;
	__s32		bs_extsize;
	__s32		bs_extents;
	__u32		bs_gen;
	__u16		bs_projid_lo;
	__u16		bs_forkoff;
	__u16		bs_projid_hi;
	__u16		bs_sick;
	__u16		bs_checked;
	unsigned char	bs_pad[2];
	__u32		bs_cowextsize;
	__u32		bs_dmevmask;
	__u16		bs_dmstate;
	__u16		bs_aextents;
};

/* struct xfs_swapext -- 192 bytes (XFS_IOC_SWAPEXT arg) */
struct briefs_swapext {
	int64_t		sx_version;	/* 0 */
	int64_t		sx_fdtarget;	/* fd of the open (target) file */
	int64_t		sx_fdtmp;	/* fd of the donor (tmp) file */
	int64_t		sx_offset;	/* offset into file (unused by BrieFS) */
	int64_t		sx_length;	/* length from offset (unused by BrieFS) */
	char		sx_pad[16];
	struct briefs_bstat sx_stat;	/* stat of target before copy (out) */
};

/* struct xfs_exchange_range -- 40 bytes (XFS_IOC_EXCHANGE_RANGE arg) */
struct briefs_exchange_range {
	__s32		file1_fd;	/* donor file descriptor */
	__u32		pad;		/* must be zeroes */
	__u64		file1_offset;	/* donor (file1) offset, bytes */
	__u64		file2_offset;	/* open (file2) offset, bytes */
	__u64		length;		/* bytes to exchange */
	__u64		flags;		/* see BRIEFS_EXCHANGE_RANGE_* below */
};

/* struct xfs_commit_range -- 88 bytes (START_COMMIT / COMMIT_RANGE arg) */
struct briefs_commit_range {
	__s32		file1_fd;
	__u32		pad;		/* must be zeroes */
	__u64		file1_offset;
	__u64		file2_offset;
	__u64		length;
	__u64		flags;
	__u64		file2_freshness[6];	/* opaque freshness blob */
};

/*
 * Exchange file data all the way to the ends of both files, then exchange the
 * file sizes.  @length is ignored.
 */
#define BRIEFS_EXCHANGE_RANGE_TO_EOF		(1ULL << 0)

/* Flush all file data and metadata to disk before returning. */
#define BRIEFS_EXCHANGE_RANGE_DSYNC		(1ULL << 1)

/* Dry run: do all parameter verification but change nothing. */
#define BRIEFS_EXCHANGE_RANGE_DRY_RUN		(1ULL << 2)

/* Exchange only file1's written ranges (skip unwritten/hole). */
#define BRIEFS_EXCHANGE_RANGE_FILE1_WRITTEN	(1ULL << 3)

#define BRIEFS_EXCHANGE_RANGE_ALL_FLAGS		(BRIEFS_EXCHANGE_RANGE_TO_EOF | \
						 BRIEFS_EXCHANGE_RANGE_DSYNC | \
						 BRIEFS_EXCHANGE_RANGE_DRY_RUN | \
						 BRIEFS_EXCHANGE_RANGE_FILE1_WRITTEN)

/*
 * Opaque freshness blob sampled by START_COMMIT and verified by COMMIT_RANGE.
 * Layout matches struct xfs_commit_range_fresh (48 bytes == file2_freshness[6]).
 * BrieFS does not use the fsid field (cross-mount is rejected by f_path.mnt
 * before the freshness check); it samples ino/gen/mtime/ctime of file2 and
 * verifies them at commit time, returning -EBUSY on any mismatch.
 */
struct briefs_commit_range_fresh {
	__s32		fsid_val[2];		/* opaque (unused by BrieFS); xfs_fsid_t */
	__u64		file2_ino;
	__s64		file2_mtime;
	__s64		file2_ctime;
	__s32		file2_mtime_nsec;
	__s32		file2_ctime_nsec;
	__u32		file2_gen;
	__u32		magic;
};

#define BRIEFS_XCR_FRESH_MAGIC	0x444F524B	/* "DORK" */

/*
 * Ioctl numbers.  Computed from the briefs_ structs above, whose sizes match
 * the XFS UAPI structs, so the encoded numbers equal XFS_IOC_* and match the
 * values xfs_io issues.
 */
#define BRIEFS_IOC_EXCHANGE_RANGE	_IOW('X', 129, struct briefs_exchange_range)
#define BRIEFS_IOC_START_COMMIT		_IOR('X', 130, struct briefs_commit_range)
#define BRIEFS_IOC_COMMIT_RANGE		_IOW('X', 131, struct briefs_commit_range)
#define BRIEFS_IOC_SWAPEXT		_IOWR('X', 109, struct briefs_swapext)

#endif /* _BRIEFS_EXCHANGE_H */
