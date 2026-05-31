/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_H
#define _BRIEFS_H

#include <linux/fs.h>

/* BrieFS magic number */
#define _BRIEFS_SUPER_MAGIC 0x50656c6963616e62 /* "Pelicanb" */

/* Semantic versioning, yo */
#define _BRIEFS_MAJOR_VER 0
#define _BRIEFS_MINOR_VER 0
#define _BRIEFS_PATCH_VER 0

/* TODO: Take endianness into account. Default to little endian, or include a
 * test for what endianness the fs was originally created on and flip if
 * necessary?
 */

struct briefs_superblock {
	_u64 magic;
	_u32 major_version;
	_u32 minor_version;
	_u32 patch_version;
	_u64 total_blocks;
	_u64 data_blocks;
	_u64 block_size;
	_u64 inode_size;
	_u64 blocks_per_group;
	_u64 inodes_per_group;
	_u64 fs_created;	
	_u64 fs_last_mounted;
	_u64 fs_last_checkpoint;
	_u64 free_data_blocks;
	_u64 free_inodes;
	_u64 root_inode_number;
	_u64 feature_compat;
	_u64 feature_ro_compat;
	_u64 feature_incompat;
	_u8 uuid[16]; /* 128-bit uuid for this volume */
	_u64 eat_offset;
	_u64 eat_blocks;
	
	u_char label[64]; /* utf8, null padded */
	u_char reserved[XXXXX];
} /* 4096 bytes */

struct briefs_inode {
	_u64 inode_number;
	_u64 magic;
	_u32 filemode;
	_u32 uid;
	_u32 gid;
	_u64 filesize;
	_u64 ctime_sec;
	_u64 ctime_nsec;
	_u64 atime_sec;
	_u64 atime_nsec;
	_u64 mtime_sec;
	_u64 mtime_nsec;
	_u32 nlinks;
	_u32 num_extents_inline;
	_u64 extent_inline_base;extent_inline_base
	_u64 num_extents_total; /* inline + overflow */
	_u8 reserved_extents[256]; /* Dummy out inline extents for the moment */
	_u64 xattr_offset;
	_u64 xattr_size;
	_u64 parent_inode;
	_u32 link_count; /* same as nlinks? */
	_u32 flags;
	_u8 reserved[XXX]; /* zero padded as needed to give 512 byte inodes */
}

/* Function headers and the like */
#ifdef __KERNEL__

#endif /* __KERNEL__ */

#endif /* _BRIEFS_H */extent_inline_base
