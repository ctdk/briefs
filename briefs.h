/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_H
#define _BRIEFS_H

#include <linux/fs.h>

/* BrieFS magic number */
#define _BRIEFS_SUPER_MAGIC 0x50656c6963616e62 /* "Pelicanb" */

/* Define the number of reserved/padding bytes in the superblock */
#define _BRIEFS_SUPER_RESERVED 3784

/* Semantic versioning, yo */
#define _BRIEFS_MAJOR_VER 0
#define _BRIEFS_MINOR_VER 0
#define _BRIEFS_PATCH_VER 1

/* TODO: Take endianness into account. Default to little endian, or include a
 * test for what endianness the fs was originally created on and flip if
 * necessary?
 */

struct briefs_superblock {
	_u64 magic; 					/* 0 offset */

	/* 8 byte version numbers is a bit ridiculous, but it makes memory
	 * alignment a lot easier. At least there's tons of extra space in the
	 * superblock for it.
	 */
	_u64 major_version; 				/* 8 */
	_u64 minor_version; 				/* 16 */
	_u64 patch_version; 				/* 24 */

	_u64 total_blocks;  				/* 32 */
	_u64 data_blocks; 				/* 40 */
	_u64 block_size; 				/* 48 */
	_u64 inode_size; 				/* 56 */
	_u64 blocks_per_group; 				/* 64 */
	_u64 inodes_per_group; 				/* 72 */
	_u64 fs_created;				/* 80 */
	_u64 fs_last_mounted; 				/* 88 */
	_u64 fs_last_checkpoint; 			/* 96 */
	_u64 free_inodes; 				/* 104 */
	_u64 root_inode_number; 			/* 112 */
	_u64 feature_compat; 				/* 120 */
	_u64 feature_ro_compat; 			/* 128 */
	_u64 feature_incompat; 				/* 136 */
	
	/* 128-bit uuid for this volume */
	_u8 uuid[16]; 					/* 144 */

	/* The below is particularly subject to change */
	_u64 eat_offset; 				/* 168 */
	_u64 eat_blocks; 				/* 176 */
	_u64 trie_root_block; 				/* 184 */
	_u64 trie_blocks_used; 				/* 192 */
	_u64 trie_node_pool_start; 			/* 200 */
	_u64 trie_node_pool_size; 			/* 208 */
	_u64 inode_bitmap_offset; 			/* 216 */
	_u64 inode_bitmap_blocks; 			/* 224 */
	_u64 data_bitmap_offset; 			/* 232 */
	_u64 data_bitmap_blocks; 			/* 240 */

	/* utf8, null padded */
	u_char label[64]; 				/* 248 */

	/* Currently we start padding the superblock at block 312. With basic
	 * arithmetic, 4096 - 312 means _BRIEFS_SUPER_RESERVED should be
	 * 3784. */
	u_char reserved[_BRIEFS_SUPER_RESERVED];	/* 312 */

	/* TODO: consider putting a superblock checksum at the end. Having
	 * occasional backup superblocks on the FS might not be a terrible idea
	 * either? */

	/* Also to consider, since we have the space for it:
	 * - creator OS
	 * - last mounted location
	 * - mount count?
	 * - creation time?
	 * - more as they come up
	 */
}; /* 4096 bytes */

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
	_u64 extent_inline_base;
	_u64 num_extents_total; /* inline + overflow */
	briefs_extent inline_extents[8];
	_u64 xattr_offset;
	_u64 xattr_size;
	_u64 parent_inode;
	_u32 link_count; /* same as nlinks? */
	_u32 flags;
	_u8 reserved[XXX]; /* zero padded as needed to give 512 byte inodes */
};

struct briefs_extent {
	_u64 offset
	_u64 phys
	_u64 len
	_u64 flags
};

struct briefs_extent_chain {
	_u64 next_overflow_block;
	_u32 num_extents_in_block;
	_u32 pad;
	briefs_extent extents[256];
	_u64 checksum;
};

/* Bitwise trie node definition */
struct trie_node {
	_u64 range_start; /* starting block of this node's range */
	_u32 range_len;   /* number of blocks covered (power of 2) */
	_u32 free_count;  /* how many blocks in this range are free */

	/* Leaf (range_len == 1):
	 *   free_count == 0 → allocated
	 *   free_count == 1 → free

	 * Internal node:
	 *   left child covers range_start, range_len / 2
	 *   right child covers range_start + range_len/2, range_len / 2
	 */

	_u64 left_child;  /* block offset of left child node */
	_u64 right_child; /* block offset of right child node */
}; /* 32 bytes */

/* Function headers and the like */
#ifdef __KERNEL__

#endif /* __KERNEL__ */

#endif /* _BRIEFS_H */extent_inline_base
