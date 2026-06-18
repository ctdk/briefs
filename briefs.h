/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_H
#define _BRIEFS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/blk_types.h>
#include <linux/unaligned.h>
#include "briefs_alloc.h"

/* BrieFS magic number */
#define _BRIEFS_SUPER_MAGIC 0x504C434E /* "PLCN" */
#define _BRIEFS_INODE_MAGIC 0x494E4F44  /* "INOD" */

/* Define the number of reserved/padding bytes in the superblock */
#define _BRIEFS_SUPER_RESERVED 648

#define _BRIEFS_ROOT_INO 1
#define _BRIEFS_BAD_INO  2

/* Default values */
#define BRIEFS_BLOCK_SIZE 4096
#define BRIEFS_BLOCK_SHIFT 12
#define BRIEFS_INODE_SIZE 512
#define BRIEFS_INODE_INLINE_DATA_SIZE 256
#define BRIEFS_NAME_LEN 255
#define BRIEFS_CHAIN_EXTENTS 127

/* Semantic versioning, yo */
#define _BRIEFS_MAJOR_VER 0
#define _BRIEFS_MINOR_VER 8
#define _BRIEFS_PATCH_VER 0

/* Journal magic */
#define JOURNAL_MAGIC 0x4A4E4C5A  /* "JNLZ" */
#define CHECKPOINT_MAGIC 0x43485053  /* "CHPS" */

/* Journal record types */
enum journal_record_type {
	JRN_NONE = 0,
	JRN_EXTENT_ALLOC,
	JRN_EXTENT_FREE,
	JRN_INODE_UPDATE,
	JRN_INODE_ALLOC,
	JRN_INODE_FREE,
	JRN_TRIE_ALLOC,
	JRN_DIR_UPDATE,
	JRN_CHECKPOINT,
	JRN_INODE_FULL,
	JRN_SYMLINK_DATA,
	JRN_END,
};

/* Journal record header (16 bytes) */
struct journal_record_hdr {
	__le32 type;          /* enum journal_record_type */
	__le32 flags;         /* JRN_FLAG_* */
	__le32 data_len;      /* bytes of data following header */
	__le32 checksum;      /* crc32c of type + flags + data_len + data */
};

/* Record data structures (on-disk, little endian) */

/* JRN_EXTENT_ALLOC */
struct jrn_extent_alloc {
	__le64 ino;           /* inode being modified */
	__le64 offset;        /* logical block offset */
	__le64 length;        /* number of blocks allocated */
	__le64 phys_start;    /* starting physical block */
	__le32 extent_index;  /* which extent slot this affects */
	__u8 reserved[44];    /* padding to 64 bytes */
};

/* JRN_EXTENT_FREE */
struct jrn_extent_free {
	__le64 ino;           /* inode being modified */
	__le64 offset;        /* logical block offset */
	__le64 phys_start;    /* starting physical block */
	__le64 length;        /* number of blocks freed */
	__u8 reserved[48];    /* padding to 64 bytes */
};

/* JRN_INODE_UPDATE */
struct jrn_inode_update {
	__le64 ino;           /* inode being modified */
	__le32 mode;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le64 size;
	__le64 atime_sec;
	__le64 atime_nsec;
	__le64 mtime_sec;
	__le64 mtime_nsec;
	__le64 ctime_sec;
	__le64 ctime_nsec;
	__le32 flags;
	__le32 reserved;
};

/* JRN_INODE_ALLOC */
struct jrn_inode_alloc {
	__le64 ino;           /* inode number being allocated */
	__le32 mode;
	__le32 nlink;
	__le32 uid;
	__le32 gid;
	__le32 reserved1;
	__le64 reserved2;
};

/* JRN_INODE_FREE */
struct jrn_inode_free {
	__le64 ino;           /* inode being freed */
	__le32 reserved[3];
	__le64 reserved2;
};

/* JRN_TRIE_ALLOC - log allocation/free of a packed directory-trie page */
struct jrn_trie_alloc {
	__le64 block;      /* absolute data block used as a trie page */
	__le32 op;         /* 0 = allocated, 1 = freed */
	__le32 reserved;
};

/* JRN_DIR_UPDATE */
struct jrn_dir_update {
	__le64 parent_ino;
	__le64 child_ino;
	__le32 name_len;
	__u8 name[255];
	__u8 op;              /* 0 = add, 1 = delete */
	__u8 reserved[2];
};

/* JRN_INODE_FULL - complete 512-byte on-disk inode snapshot */
struct jrn_inode_full {
	__le64 ino;               /* inode number */
	__u8 inode_data[512];     /* raw little-endian disk inode */
	__u8 reserved[40];        /* padding to 560 bytes */
};

/* JRN_SYMLINK_DATA - inline symlink target bytes */
struct jrn_symlink_data {
	__le64 ino;           /* symlink inode */
	__le64 phys;          /* physical block holding the target */
	__le32 target_len;    /* number of target bytes that follow */
	__u8   target[];      /* symlink target bytes (no trailing NUL) */
};

/* JRN_CHECKPOINT */
struct jrn_checkpoint {
	__le64 checkpoint_seq;
	__le32 record_count;
	__le32 reserved1;
	__le64 log_sequence_end;
	__le64 trie_root_node;
	__le64 free_data_count;
	__le64 free_inode_count;
	__le64 reserved2;
};

/* Compute CRC32C checksum */
__u32 briefs_crc32c(__u32 crc, const void *data, size_t len);

/* Journal block header (16 bytes) */
struct journal_block_header {
	__le32 magic;         /* JOURNAL_MAGIC */
	__le32 block_seq;     /* monotonically increasing */
	__le32 record_count;  /* number of records in this block */
	__le32 reserved;
};

/* Journal block (one block on disk, includes header + records) */
struct journal_block {
	struct journal_block_header header;
	unsigned char records[];
};

/* Superblock - block 0, 4096 bytes */
struct briefs_superblock {
	__le64 magic;

	/* 8 byte version numbers is a bit ridiculous, but it makes memory
	 * alignment a lot easier. At least there's tons of extra space in the
	 * superblock for it.
	 */
	__le64 major_version;
	__le64 minor_version;
	__le64 patch_version;

	__le64 total_blocks;
	__le64 data_blocks;
	__le64 block_size;
	__le64 inode_size;
	__le64 blocks_per_group;
	__le64 inodes_per_group;
	__le64 fs_created;
	__le64 fs_last_mounted;
	__le64 fs_last_checkpoint;
	/* tracked by trie root free_count */
	__le64 free_data_blocks;
	__le64 free_inodes;
	__le64 root_inode_number;
	__le64 feature_compat;
	__le64 feature_ro_compat;
	__le64 feature_incompat;

	/* 128-bit uuid for this volume */
	__u8 uuid[16];

	/* The below is particularly subject to change */
	__le64 eat_offset;
	__le64 eat_blocks;
	__le64 trie_root_block;
	__le64 trie_blocks_used;
	__le64 trie_node_pool_start;
	__le64 trie_node_pool_size;
	__le64 inode_bitmap_offset;
	__le64 inode_bitmap_blocks;
	__le64 inode_table_offset;
	__le64 journal_offset;
	__le64 journal_blocks;
	__le64 checkpoint_seq;
	__le64 journal_log_start;
	__le64 journal_log_end;
	__le64 reserved_journal[4];

	/* utf8, null padded */
	unsigned char label[64];

	/* Superblock padding */
	unsigned char reserved[_BRIEFS_SUPER_RESERVED];
}; /* 1024 bytes */

/* Extent entry - 32 bytes (in-memory, CPU endian) */
struct briefs_extent {
	__u64 offset;
	__u64 phys;
	__u64 len;
	__u32 flags;
	__u32 pad;
};

/* On-disk extent entry - 32 bytes (little endian) */
struct briefs_disk_extent {
	__le64 offset;
	__le64 phys;
	__le64 len;
	__le32 flags;
	__le32 pad;
};

/* Inode flags */
#define InodeFlagReserved    0x00000001
#define InodeFlagCompressed  0x00000002
#define InodeFlagIndexed     0x00000004
#define InodeFlagInlineData  0x00000008

/* Inode - 512 bytes (in-memory, CPU endian) */
struct briefs_inode {
	__u64 inode_number;
	__u64 magic;
	__u32 filemode;
	__u32 uid;
	__u32 gid;
	__u32 _pad0;               /* explicit padding for u64 alignment */
	__u64 filesize;
	__u64 ctime_sec;
	__u64 ctime_nsec;
	__u64 atime_sec;
	__u64 atime_nsec;
	__u64 mtime_sec;
	__u64 mtime_nsec;
	__u64 creation_time_sec;
	__u64 creation_time_nsec;
	__u32 nlinks;
	__u32 num_extents_inline;
	__u64 extent_inline_base;
	__u64 num_extents_total; /* inline + overflow */
	union {
		struct briefs_extent inline_extents[8];
		__u8 inline_data[256];
	};
	__u64 xattr_offset;
	__u64 xattr_size;
	__u64 parent_inode;
	__u32 unused; 		   /* used to duplicate nlinks, renamed */
	__u32 flags;
	__u64 dir_trie_root;       /* packed trie node reference: (block << 6) | slot (dirs only) */
	__u64 rdev;                /* device number (block/char special files) */
	__u8 reserved[80]; /* zero padded to 512 bytes */
};

/* On-disk inode - 512 bytes (little endian) */
struct briefs_disk_inode {
	__le64 inode_number;
	__le64 magic;
	__le32 filemode;
	__le32 uid;
	__le32 gid;
	__le32 _pad0;
	__le64 filesize;
	__le64 ctime_sec;
	__le64 ctime_nsec;
	__le64 atime_sec;
	__le64 atime_nsec;
	__le64 mtime_sec;
	__le64 mtime_nsec;
	__le64 creation_time_sec;
	__le64 creation_time_nsec;
	__le32 nlinks;
	__le32 num_extents_inline;
	__le64 extent_inline_base;
	__le64 num_extents_total;
	union {
		struct briefs_disk_extent inline_extents[8];
		__u8 inline_data[256];
	};
	__le64 xattr_offset;
	__le64 xattr_size;
	__le64 parent_inode;
	__le32 unused;
	__le32 flags;
	__le64 dir_trie_root;
	__le64 rdev;
	__u8 reserved[80];
};

static inline void briefs_disk_extent_to_cpu(const struct briefs_disk_extent *src,
                                              struct briefs_extent *dst)
{
	dst->offset = le64_to_cpu(src->offset);
	dst->phys = le64_to_cpu(src->phys);
	dst->len = le64_to_cpu(src->len);
	dst->flags = le32_to_cpu(src->flags);
	dst->pad = le32_to_cpu(src->pad);
}

static inline void briefs_cpu_extent_to_disk(const struct briefs_extent *src,
                                              struct briefs_disk_extent *dst)
{
	dst->offset = cpu_to_le64(src->offset);
	dst->phys = cpu_to_le64(src->phys);
	dst->len = cpu_to_le64(src->len);
	dst->flags = cpu_to_le32(src->flags);
	dst->pad = cpu_to_le32(src->pad);
}

static inline void briefs_disk_inode_to_cpu(const struct briefs_disk_inode *src,
                                             struct briefs_inode *dst)
{
	int i;

	dst->inode_number = le64_to_cpu(src->inode_number);
	dst->magic = le64_to_cpu(src->magic);
	dst->filemode = le32_to_cpu(src->filemode);
	dst->uid = le32_to_cpu(src->uid);
	dst->gid = le32_to_cpu(src->gid);
	dst->_pad0 = le32_to_cpu(src->_pad0);
	dst->filesize = le64_to_cpu(src->filesize);
	dst->ctime_sec = le64_to_cpu(src->ctime_sec);
	dst->ctime_nsec = le64_to_cpu(src->ctime_nsec);
	dst->atime_sec = le64_to_cpu(src->atime_sec);
	dst->atime_nsec = le64_to_cpu(src->atime_nsec);
	dst->mtime_sec = le64_to_cpu(src->mtime_sec);
	dst->mtime_nsec = le64_to_cpu(src->mtime_nsec);
	dst->creation_time_sec = le64_to_cpu(src->creation_time_sec);
	dst->creation_time_nsec = le64_to_cpu(src->creation_time_nsec);
	dst->nlinks = le32_to_cpu(src->nlinks);
	dst->num_extents_inline = le32_to_cpu(src->num_extents_inline);
	dst->extent_inline_base = le64_to_cpu(src->extent_inline_base);
	dst->num_extents_total = le64_to_cpu(src->num_extents_total);
	if (le32_to_cpu(src->flags) & InodeFlagInlineData)
		memcpy(dst->inline_data, src->inline_data, sizeof(dst->inline_data));
	else
		for (i = 0; i < 8; i++)
			briefs_disk_extent_to_cpu(&src->inline_extents[i], &dst->inline_extents[i]);
	dst->xattr_offset = le64_to_cpu(src->xattr_offset);
	dst->xattr_size = le64_to_cpu(src->xattr_size);
	dst->parent_inode = le64_to_cpu(src->parent_inode);
	dst->unused = le32_to_cpu(src->unused);
	dst->flags = le32_to_cpu(src->flags);
	dst->dir_trie_root = le64_to_cpu(src->dir_trie_root);
	dst->rdev = le64_to_cpu(src->rdev);
	memcpy(dst->reserved, src->reserved, sizeof(dst->reserved));
}

static inline void briefs_cpu_inode_to_disk(const struct briefs_inode *src,
                                             struct briefs_disk_inode *dst)
{
	int i;

	dst->inode_number = cpu_to_le64(src->inode_number);
	dst->magic = cpu_to_le64(src->magic);
	dst->filemode = cpu_to_le32(src->filemode);
	dst->uid = cpu_to_le32(src->uid);
	dst->gid = cpu_to_le32(src->gid);
	dst->_pad0 = cpu_to_le32(src->_pad0);
	dst->filesize = cpu_to_le64(src->filesize);
	dst->ctime_sec = cpu_to_le64(src->ctime_sec);
	dst->ctime_nsec = cpu_to_le64(src->ctime_nsec);
	dst->atime_sec = cpu_to_le64(src->atime_sec);
	dst->atime_nsec = cpu_to_le64(src->atime_nsec);
	dst->mtime_sec = cpu_to_le64(src->mtime_sec);
	dst->mtime_nsec = cpu_to_le64(src->mtime_nsec);
	dst->creation_time_sec = cpu_to_le64(src->creation_time_sec);
	dst->creation_time_nsec = cpu_to_le64(src->creation_time_nsec);
	dst->nlinks = cpu_to_le32(src->nlinks);
	dst->num_extents_inline = cpu_to_le32(src->num_extents_inline);
	dst->extent_inline_base = cpu_to_le64(src->extent_inline_base);
	dst->num_extents_total = cpu_to_le64(src->num_extents_total);
	if (src->flags & InodeFlagInlineData)
		memcpy(dst->inline_data, src->inline_data, sizeof(dst->inline_data));
	else
		for (i = 0; i < 8; i++)
			briefs_cpu_extent_to_disk(&src->inline_extents[i], &dst->inline_extents[i]);
	dst->xattr_offset = cpu_to_le64(src->xattr_offset);
	dst->xattr_size = cpu_to_le64(src->xattr_size);
	dst->parent_inode = cpu_to_le64(src->parent_inode);
	dst->unused = cpu_to_le32(src->unused);
	dst->flags = cpu_to_le32(src->flags);
	dst->dir_trie_root = cpu_to_le64(src->dir_trie_root);
	dst->rdev = cpu_to_le64(src->rdev);
	memcpy(dst->reserved, src->reserved, sizeof(dst->reserved));
}

/* Extent chain for overflow - one block on disk */
struct briefs_extent_chain {
	__le64 next_overflow_block;
	__le32 num_extents_in_block;
	__le32 pad;
	struct briefs_disk_extent extents[127];
	__le64 checksum;
};

/*
 * Extent chain block checksum helpers.
 * The checksum covers bytes [0, block_size-16), i.e. the header and all
 * 127 extents but not the 8-byte checksum field itself (nor the 8 bytes of
 * trailing padding). A stored value of zero is treated as legacy (no
 * checksum) and always verifies.
 */
static inline __u64 briefs_chain_checksum(const void *data)
{
	return (__u64)briefs_crc32c(0, data, BRIEFS_BLOCK_SIZE - 2 * sizeof(__u64));
}

static inline int briefs_verify_chain_checksum(const void *data, __le64 stored)
{
	__u64 cpu_stored = le64_to_cpu(stored);

	if (cpu_stored == 0)
		return 0; /* legacy block with no checksum */
	return (briefs_chain_checksum(data) == cpu_stored) ? 0 : -EIO;
}

/* Function headers and the like */

/* Trie root block - first block in trie node pool */

/* Trie node/page magic values */
#define BRIEFS_TRIE_MAGIC      0x54524E20  /* "TRN " - legacy single-node blocks */
#define BRIEFS_TRIE_PAGE_MAGIC 0x54524E50  /* "TRNP" - packed multi-node pages */

/* Bitwise trie macros for directory trie */
#define TRIE_IS_LEAF(node) (((node)->node_type & NODE_TYPE_INTERM) == 0 || \
			((node)->node_type & NODE_STATUS_LEAF))

/* Trie node flags */
#define TRIE_FLAG_ROOT    0x00000001
#define TRIE_FLAG_NEW     0x00000002
#define TRIE_FLAG_DIRTY   0x00000004

/* Trie node types */
#define NODE_TYPE_FILE      0x01
#define NODE_TYPE_DIR       0x02
#define NODE_TYPE_INTERM    0x04
/*
 * NODE_STATUS_LEAF - this INTERM node also stores a leaf entry.
 * Set on NODE_TYPE_INTERM nodes whose name is a prefix of longer
 * names branching from this node (e.g. "file_1" when "file_10"
 * and "file_100" also exist).
 *
 * The file type (ftype) for leaf entries is stored in f_type, not in
 * node_type.  node_type is used purely for trie structure (INTERM, STATUS_LEAF).
 */
#define NODE_STATUS_LEAF    0x08

#define NODE_FLAG_DELETED   0x00000004
#define NODE_FLAG_ROOT      0x00000008

/*
 * Packed trie page layout (BrieFS >= 0.7.0).
 *
 * Directory trie nodes are packed into 4 KiB "pages" allocated from the
 * data-block allocator.  Each page holds up to TRIE_SLOTS_PER_BLOCK node
 * slots and a shared variable-length name heap.  Child/sibling links are
 * stored as 64-bit "node references" rather than block numbers.
 *
 * A node reference is encoded as:
 *   ref = (absolute_block_number << TRIE_SLOT_BITS) | slot_index
 * with ref == 0 meaning "null".  Slots are numbered 0..TRIE_SLOTS_PER_BLOCK-1
 * inside each page; slot 0 is the root node for that page.
 */
#define TRIE_SLOTS_PER_BLOCK 64
#define TRIE_SLOT_BITS 6
#define TRIE_SLOT_MASK ((1ULL << TRIE_SLOT_BITS) - 1)

#define TRIE_MAKE_REF(block, slot) (((u64)(block) << TRIE_SLOT_BITS) | ((slot) & TRIE_SLOT_MASK))
#define TRIE_REF_BLOCK(ref) ((ref) >> TRIE_SLOT_BITS)
#define TRIE_REF_SLOT(ref) ((ref) & TRIE_SLOT_MASK)
#define TRIE_REF_IS_NULL(ref) ((ref) == 0)

/*
 * Trie page header (20 bytes packed), followed immediately by the slot array.
 * Name data is stored in a heap that grows upward from the end of the block
 * toward the slot array.
 */
struct briefs_trie_page {
	__le32 magic;             /* BRIEFS_TRIE_PAGE_MAGIC */
	__le32 version;           /* 1 */
	__le16 live_count;        /* number of allocated slots */
	__le16 free_name_off;     /* bytes of name heap used, measured from
				   * the end of the block */
	__le64 free_slots;        /* bitmap of free slots (bit i == 1 -> free) */
} __attribute__((packed));

/*
 * Trie node slot (36 bytes packed).  One slot per directory trie node.
 * first_child and next_sibling are node references, not block numbers.
 */
struct briefs_trie_node {
	__le64 first_child;       /* node reference of first child */
	__le64 next_sibling;      /* node reference of next sibling */
	__le64 inode;             /* inode number (leaf entries) */
	__le16 name_len;          /* full name length + 2-byte prefix, or 0 if free */
	__le16 name_offset;       /* offset from block end to name bytes, or 0 if free */
	__u8  depth;              /* depth in trie (0 = root) */
	__u8  node_type;          /* NODE_TYPE_* */
	__u8  byte_val;           /* the byte value this node represents */
	__u8  f_type;             /* file type for leaf entries (S_IFMT >> 12) */
	__le16 flags;             /* NODE_FLAG_* */
	__le16 child_count;       /* number of children */
} __attribute__((packed));

/* Endian-aware accessors for packed trie page fields.
 * get_unaligned_leXX() already returns CPU-endian values and
 * put_unaligned_leXX() expects a CPU-endian value, so do not wrap
 * with extra leXX_to_cpu()/cpu_to_leXX() conversions.
 */
#define trie_page_magic(p)        get_unaligned_le32(&(p)->magic)
#define trie_page_version(p)      get_unaligned_le32(&(p)->version)
#define trie_page_live_count(p)   get_unaligned_le16(&(p)->live_count)
#define trie_page_free_name_off(p) get_unaligned_le16(&(p)->free_name_off)
#define trie_page_free_slots(p)   get_unaligned_le64(&(p)->free_slots)
#define trie_page_set_magic(p, v)       put_unaligned_le32((u32)(v), &(p)->magic)
#define trie_page_set_version(p, v)     put_unaligned_le32((u32)(v), &(p)->version)
#define trie_page_set_live_count(p, v)  put_unaligned_le16((u16)(v), &(p)->live_count)
#define trie_page_set_free_name_off(p, v) put_unaligned_le16((u16)(v), &(p)->free_name_off)
#define trie_page_set_free_slots(p, v)  put_unaligned_le64((u64)(v), &(p)->free_slots)

/* Endian-aware accessors for packed trie node fields */
#define trie_node_first_child(n)    get_unaligned_le64(&(n)->first_child)
#define trie_node_next_sibling(n)   get_unaligned_le64(&(n)->next_sibling)
#define trie_node_inode(n)          get_unaligned_le64(&(n)->inode)
#define trie_node_name_len(n)       get_unaligned_le16(&(n)->name_len)
#define trie_node_name_offset(n)    get_unaligned_le16(&(n)->name_offset)
#define trie_node_flags(n)          get_unaligned_le16(&(n)->flags)
#define trie_node_child_count(n)    get_unaligned_le16(&(n)->child_count)
#define trie_node_set_first_child(n, v)   put_unaligned_le64((u64)(v), &(n)->first_child)
#define trie_node_set_next_sibling(n, v)  put_unaligned_le64((u64)(v), &(n)->next_sibling)
#define trie_node_set_inode(n, v)         put_unaligned_le64((u64)(v), &(n)->inode)
#define trie_node_set_name_len(n, v)      put_unaligned_le16((u16)(v), &(n)->name_len)
#define trie_node_set_name_offset(n, v)   put_unaligned_le16((u16)(v), &(n)->name_offset)
#define trie_node_set_flags(n, v)         put_unaligned_le16((u16)(v), &(n)->flags)
#define trie_node_set_child_count(n, v)   put_unaligned_le16((u16)(v), &(n)->child_count)

/*
 * Helper to get/set the file type stored in f_type.
 * node_type is used for trie structure only (INTERM, STATUS_LEAF);
 * the actual file type (ftype from S_IFMT >> 12) lives here.
 */
#define TRIE_NODE_FTYPE(node) ((node)->f_type)
#define TRIE_SET_FTYPE(node, ftype) ((node)->f_type = (ftype))

/*
 * Pointer to the name bytes for a node slot.  The 2-byte little-endian
 * length prefix lives at (base - 2); the name bytes follow it.
 */
#define TRIE_NODE_NAME_BASE(page_base, node) \
	((void *)(page_base) + BRIEFS_BLOCK_SIZE - trie_node_name_offset(node) + 2)

/* Maximum inline name length a packed trie slot can hold */
#define TRIE_MAX_NAME_LEN BRIEFS_NAME_LEN

/*
 * Legacy single-node-per-block layout is no longer supported as of BrieFS 0.7.0.
 * The old struct is preserved below for reference/debugging of legacy images.
 * Fields are marked __le* so any future legacy reader is architecture-safe.
 */
struct briefs_trie_node_legacy {
	__le32 magic;
	__le32 child_count;
	__le64 first_child;
	__le64 next_sibling;
	__u8  depth;
	__u8  node_type;
	__u8  byte_val;
	__u8  f_type;
	__u8  reserved[4];
	__le64 flags;
	__le64 inode;
	__le16 name_len;
	__le16 name_offset;
};

/* Trie operations - directory trie node allocation (uses data block allocator) */
int briefs_trie_create_root(struct super_block *sb, struct briefs_inode *di);
int briefs_trie_lookup(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len, u64 *found_ino, u8 *found_type);
int briefs_trie_insert(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len, u64 ino, u8 type);
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len);
void briefs_trie_free_all(struct super_block *sb, struct briefs_inode *di);

/* Packed trie page helpers (briefs_trie_page.c) */
struct buffer_head *briefs_trie_read_page(struct super_block *sb, u64 node_ref,
                                          struct briefs_trie_page **page,
                                          struct briefs_trie_node **node);
struct buffer_head *briefs_trie_get_page(struct super_block *sb, u64 node_ref,
                                          struct briefs_trie_page **page,
                                          struct briefs_trie_node **node);
u64 briefs_trie_alloc_node(struct super_block *sb, size_t name_len);
void briefs_trie_free_node(struct super_block *sb, u64 node_ref);
int briefs_trie_node_store_name(struct super_block *sb, u64 node_ref,
                                const char *name, size_t name_len);
int briefs_trie_page_init(struct super_block *sb, u8 depth, u8 byte_val,
                          u8 node_type, u64 *out_ref);
void briefs_trie_cleanup_state(struct super_block *sb);
void briefs_trie_page_add_partial(struct super_block *sb, u64 block);
int briefs_trie_update_entry(struct super_block *sb, struct briefs_inode *di,
                            const char *name, size_t name_len,
                            u64 new_ino, u8 new_type);

/* Trie iterator for readdir - depth-first walk yielding leaves */
struct trie_iter {
	u64 *stack;
	u8 *leaf_emitted;
	int sp;
	int cap;
	/* Per-entry flag: 1 if this stack entry had its leaf emitted.
	 * Used so that when an INTERM+NODE_STATUS_LEAF node is re-pushed
	 * after emitting its own leaf, we can skip re-emission on re-visit.
	 * Only valid for entries where stack[i] != 0 (sp entries). */
	/* One-entry re-emit buffer: when dir_emit fails in briefs_readdir,
	 * the entry already consumed from the trie is saved here so it
	 * can be returned on the next briefs_trie_iter_next call. */
	bool pending;
	u64 pending_ino;
	u8 pending_type;
	u8 pending_name_buf[BRIEFS_NAME_LEN + 1];
	int pending_name_len;
	u64 gen;               /* generation of the trie when iterator was created */
};

struct trie_iter *briefs_trie_iter_alloc(void);
void briefs_trie_iter_free(struct trie_iter *iter);
void briefs_trie_iter_init(struct trie_iter *iter, struct briefs_inode *di, u64 gen);
int briefs_trie_iter_next(struct super_block *sb, struct trie_iter *iter, u64 current_gen, u64 *ino, u8 *type, char *name_buf, int *name_len);

/*
 * Packed directory-trie entries store only a 2-byte little-endian length
 * prefix followed by the name bytes.  The constants below describe the
 * accounting used for directory size updates.
 */
#define BRIEFS_DIR_ENTRY_PREFIX_LEN 2

/* VFS inode type macros */
#define BRIEFS_S_IFMT   0170000  /* type of file */
#define BRIEFS_S_IFSOCK 0140000  /* socket */
#define BRIEFS_S_IFLNK  0120000  /* symbolic link */
#define BRIEFS_S_IFREG  0100000  /* regular file */
#define BRIEFS_S_IFBLK  0060000  /* block device */
#define BRIEFS_S_IFDIR  0040000  /* directory */
#define BRIEFS_S_IFCHR  0020000  /* character device */
#define BRIEFS_S_IFIFO  0010000  /* FIFO */

#define BRIEFS_S_ISOCK(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFSOCK)
#define BRIEFS_S_ISLNK(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFLNK)
#define BRIEFS_S_ISREG(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFREG)
#define BRIEFS_S_ISBLK(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFBLK)
#define BRIEFS_S_ISDIR(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFDIR)
#define BRIEFS_S_ISCHR(m)  (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFCHR)
#define BRIEFS_S_ISFIFO(m) (((m) & BRIEFS_S_IFMT) == BRIEFS_S_IFIFO)

/* VFS structures */
struct briefs_sb_info {
	__u64 data_blocks;
	__u64 free_data_blocks;
	__u64 num_inodes;
	__u64 free_inodes;
	struct briefs_superblock *sb;
	struct buffer_head *sb_bh;       /* superblock buffer_head, released on unmount */
	struct block_device *bdev;
	struct briefs_alloc alloc;          /* data block allocator (3-level bitmap pyramid) */
	struct briefs_alloc inode_alloc;    /* inode allocator (3-level bitmap pyramid) */
	struct briefs_journal *journal;  /* transaction journal (dynamically allocated) */
};

/* briefs_inode_info - our inode info */
struct briefs_inode_info {
	struct inode vfs_inode;
	struct briefs_inode disk_inode;
	u64 inode_number;
	seqcount_t extent_seq;   /* protects disk_inode extent fields */
	struct mutex trie_lock;    /* protects directory trie structure */
	struct mutex extent_lock;  /* serializes extent list appends */
	u64 trie_gen;            /* generation counter for trie modifications */
	/*
	 * cached_max_end: running max of (extent.offset + extent.len) over all
	 * extents, in blocks. Protected by extent_seq (same as the disk_inode
	 * extent fields). 0 means "unknown/empty" -> briefs_get_block must NOT
	 * use the fast path. Overstatement is safe (fast path just fires less);
	 * understatement would make a mapped block look unmapped (read returns
	 * 0 = data loss), so it must be updated at every extent-growth site.
	 */
	u64 cached_max_end;
};

/* yanked from the xiafs module, which in turn was yanked from minix */
static inline struct briefs_sb_info *briefs_sb(struct super_block *sb) {
	return sb->s_fs_info;
}

static inline struct briefs_inode_info *briefs_i(struct inode *inode) {
	return container_of(inode, struct briefs_inode_info, vfs_inode);
}

/* Convert data-relative block to absolute block number */
static inline u64 data_to_abs(struct briefs_superblock *sb, u64 rel_block)
{
	return le64_to_cpu(sb->trie_node_pool_start) +
	       le64_to_cpu(sb->trie_blocks_used) + rel_block;
}

/* Convert absolute block number back to data-relative */
static inline u64 abs_to_data(struct briefs_superblock *sb, u64 abs_block)
{
	return abs_block - (le64_to_cpu(sb->trie_node_pool_start) +
	                  le64_to_cpu(sb->trie_blocks_used));
}

/* Convenience: first block of the inode table on disk */
static inline u64 briefs_inode_table_start(struct briefs_superblock *sb)
{
	return le64_to_cpu(sb->inode_table_offset);
}

#ifdef __KERNEL__

/* Inode operations */
extern const struct inode_operations briefs_dir_inode_ops;
extern const struct inode_operations briefs_file_inode_ops;
extern const struct inode_operations briefs_symlink_inode_ops;

/* File operations */
extern const struct file_operations briefs_dir_operations;
extern const struct file_operations briefs_file_operations;

/* address_space_operations */
extern const struct address_space_operations briefs_aops;

/* get_block callback - maps inode+iblock to buffer_head */
int briefs_get_block(struct inode *inode, sector_t iblock, struct buffer_head *bh_result, int create);

/* super_operations */
extern const struct super_operations briefs_super_ops;

/* Fill superblock - entry point for mount */
int briefs_fill_super(struct super_block *sb, void *data, int flags);
struct dentry *briefs_mount(struct file_system_type *fs_type, int flags,
                          const char *dev_name, void *data);

/* Inode operation prototypes */
struct inode *briefs_iget(struct super_block *sb, u64 ino);
struct inode *briefs_alloc_vfs_inode(struct super_block *sb);
void briefs_free_inode(struct inode *inode);
u64 briefs_alloc_inode(struct super_block *sb);
void briefs_free_inode_num(struct super_block *sb, u64 ino);
void briefs_free_inode_data(struct inode *inode);
u64 briefs_compute_i_blocks(struct super_block *sb, struct briefs_inode *di);
int briefs_read_extent(struct super_block *sb, struct briefs_inode *di, int index, struct briefs_extent *ext);
int briefs_append_extent(struct super_block *sb, struct briefs_inode *di, struct briefs_extent *ext);
int briefs_append_extent_nojournal(struct super_block *sb, struct briefs_inode *di,
                                    struct briefs_extent *ext);
void briefs_free_blocks_range(struct briefs_sb_info *bsi, u64 phys_start, u64 len);
void briefs_free_chain_blocks(struct super_block *sb, u64 chain_block);
int briefs_write_inode(struct inode *inode, struct writeback_control *wbc);

/* Disk inode I/O helpers (briefs_inode.c) */
struct buffer_head *briefs_read_inode_block(struct super_block *sb, u64 ino,
                                             struct briefs_disk_inode **di);
int briefs_persist_disk_inode(struct super_block *sb, u64 ino,
                               const struct briefs_inode *src, bool sync);
struct buffer_head *briefs_get_zero_block(struct super_block *sb, u64 block);

/* Mirror VFS timestamps into a disk inode */
static inline void briefs_sync_inode_times(struct inode *inode,
                                            struct briefs_inode *di)
{
	di->atime_sec = inode->i_atime_sec;
	di->atime_nsec = inode->i_atime_nsec;
	di->mtime_sec = inode->i_mtime_sec;
	di->mtime_nsec = inode->i_mtime_nsec;
	di->ctime_sec = inode->i_ctime_sec;
	di->ctime_nsec = inode->i_ctime_nsec;
}

/* Mirror VFS timestamps into a brand new disk inode. This sets
 * creation_time_sec and creation_time_nsec, unlike briefs_sync_inode_times. */
static inline void briefs_set_new_inode_times(struct inode *inode,
                                            struct briefs_inode *di)
{
	briefs_sync_inode_times(inode, di);
	di->creation_time_sec = inode->i_ctime_sec;
	di->creation_time_nsec = inode->i_ctime_nsec;
}

/* Update a parent directory after adding/removing an entry */
int briefs_update_parent_dir(struct inode *dir, struct briefs_sb_info *bsi,
                              ssize_t size_delta, int link_delta);

/* New inode creation helpers (briefs_inode.c) */
struct inode *briefs_new_inode(struct inode *dir, struct dentry *dentry,
                                umode_t mode, dev_t rdev);
int briefs_finish_create(struct inode *dir, struct dentry *dentry,
                          struct inode *inode, int link_delta);
void briefs_create_abort(struct super_block *sb, struct inode *dir,
                        struct inode *inode, const struct qstr *name,
                        bool dir_add_logged);

/* Inode slab cache (defined in briefs.c) */
extern struct kmem_cache *briefs_inode_cachep;

/* Add a directory entry to a parent directory */
int briefs_add_dir_entry(struct inode *dir, const char *name, size_t name_len, u64 child_ino, u8 type);

/* Remove a directory entry from a parent directory */
int briefs_remove_dir_entry(struct inode *dir, const char *name, size_t name_len);
void briefs_evict_inode(struct inode *inode);
void briefs_umount_begin(struct super_block *sb);
int briefs_sync_fs(struct super_block *sb, int wait);
int briefs_dir_open(struct inode *inode, struct file *file);
int briefs_dir_release(struct inode *inode, struct file *file);

/* Inode operations */
int briefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl);
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags);
int briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode);
int briefs_unlink(struct inode *dir, struct dentry *dentry);
int briefs_rmdir(struct inode *dir, struct dentry *dentry);
int briefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
                  struct inode *new_dir, struct dentry *new_dentry, unsigned int flags);
int briefs_link(struct dentry *old_dentry, struct inode *dir, struct dentry *new_dentry);
int briefs_symlink(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, const char *symname);
int briefs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, dev_t rdev);
const char *briefs_get_link(struct dentry *dentry, struct inode *inode, struct delayed_call *done);

/* File operations */
long briefs_fallocate(struct file *file, int mode, loff_t offset, loff_t len);

/* File operations */
ssize_t briefs_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t briefs_write_iter(struct kiocb *iocb, struct iov_iter *from);
int briefs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct iattr *attr);
int briefs_getattr(struct mnt_idmap *idmap, const struct path *path,
                   struct kstat *stat, u32 request_mask, unsigned int query_flags);
int briefs_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
                  u64 start, u64 len);
int briefs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
int briefs_open(struct inode *inode, struct file *file);
int briefs_release(struct inode *inode, struct file *file);

/* Directory operations */
int briefs_readdir(struct file *file, struct dir_context *ctx);

/* Superblock operations */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf);
void briefs_put_super(struct super_block *sb);
void briefs_kill_sb(struct super_block *sb);

/* Compile-time assertions for on-disk structure sizes.
 * These sizes must match the Go briefs-utils on-disk layout exactly.
 */
static inline void briefs_build_bug_on_sizes(void)
{
	BUILD_BUG_ON(sizeof(struct briefs_superblock) != 1024);
	BUILD_BUG_ON(sizeof(struct briefs_inode) != 512);
	BUILD_BUG_ON(sizeof(struct briefs_disk_inode) != 512);
	BUILD_BUG_ON(sizeof(struct briefs_extent) != 32);
	BUILD_BUG_ON(sizeof(struct briefs_disk_extent) != 32);
	BUILD_BUG_ON(sizeof(struct briefs_extent_chain) != 4088);
	BUILD_BUG_ON(sizeof(struct alloc_pool_header) != 48);
	BUILD_BUG_ON(sizeof(struct briefs_trie_page) != 20);
	BUILD_BUG_ON(sizeof(struct briefs_trie_node) != 36);
	BUILD_BUG_ON(sizeof(struct journal_block_header) != 16);
	BUILD_BUG_ON(sizeof(struct journal_record_hdr) != 16);
	BUILD_BUG_ON(sizeof(struct jrn_checkpoint) != 56);
	BUILD_BUG_ON(sizeof(struct jrn_dir_update) != 280);
	BUILD_BUG_ON(sizeof(struct jrn_inode_update) != 88);
	BUILD_BUG_ON(sizeof(struct jrn_extent_alloc) != 80);
	BUILD_BUG_ON(sizeof(struct jrn_extent_free) != 80);
	BUILD_BUG_ON(sizeof(struct jrn_inode_alloc) != 40);
	BUILD_BUG_ON(sizeof(struct jrn_trie_alloc) != 16);
	BUILD_BUG_ON(sizeof(struct jrn_inode_free) != 32);
	BUILD_BUG_ON(sizeof(struct jrn_inode_full) != 560);
}

#endif /* __KERNEL__ */

#endif /* _BRIEFS_H */
