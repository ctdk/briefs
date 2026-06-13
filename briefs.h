/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#ifndef _BRIEFS_H
#define _BRIEFS_H

#include <linux/fs.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/blk_types.h>
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
#define BRIEFS_INODE_SIZE 512
#define BRIEFS_MAX_LINKS 65535
#define BRIEFS_NAME_LEN 255

/* Semantic versioning, yo */
#define _BRIEFS_MAJOR_VER 0
#define _BRIEFS_MINOR_VER 5
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
	JRN_BITMAP_UPDATE,
	JRN_TRIE_UPDATE,
	JRN_DIR_UPDATE,
	JRN_SBBLOCK_UPDATE,
	JRN_CHECKPOINT,
	JRN_END,
};

/* Journal record header (16 bytes) */
struct journal_record_hdr {
	__u32 type;           /* enum journal_record_type */
	__u32 flags;          /* JRN_FLAG_* */
	__u32 data_len;       /* bytes of data following header */
	__u32 checksum;       /* crc32c of type + flags + data_len + data */
};

/* Record data structures */

/* JRN_EXTENT_ALLOC */
struct jrn_extent_alloc {
	__u64 ino;            /* inode being modified */
	__u64 offset;         /* logical block offset */
	__u64 length;         /* number of blocks allocated */
	__u64 phys_start;     /* starting physical block */
	__u32 extent_index;   /* which extent slot this affects */
	__u8 reserved[44];    /* padding to 64 bytes */
};

/* JRN_EXTENT_FREE */
struct jrn_extent_free {
	__u64 ino;            /* inode being modified */
	__u64 offset;         /* logical block offset */
	__u64 phys_start;     /* starting physical block */
	__u64 length;         /* number of blocks freed */
	__u8 reserved[48];    /* padding to 64 bytes */
};

/* JRN_INODE_UPDATE */
struct jrn_inode_update {
	__u64 ino;            /* inode being modified */
	__u32 mode;
	__u32 nlink;
	__u32 uid;
	__u32 gid;
	__u64 size;
	__u64 atime_sec;
	__u64 atime_nsec;
	__u64 mtime_sec;
	__u64 mtime_nsec;
	__u64 ctime_sec;
	__u64 ctime_nsec;
	__u32 flags;
	__u32 reserved;
};

/* JRN_INODE_ALLOC */
struct jrn_inode_alloc {
	__u64 ino;            /* inode number being allocated */
	__u32 mode;
	__u32 nlink;
	__u32 uid;
	__u32 gid;
	__u32 reserved1;
	__u64 reserved2;
};

/* JRN_INODE_FREE */
struct jrn_inode_free {
	__u64 ino;            /* inode being freed */
	__u32 reserved[3];
	__u64 reserved2;
};

/* JRN_BITMAP_UPDATE */
struct jrn_bitmap_update {
	__u32 bitmap_type;    /* 0 = free data, 1 = free inode */
	__u32 reserved1;
	__u64 offset;         /* bit offset in bitmap */
	__u64 count;          /* number of consecutive bits */
	__u8 flip_to;         /* 0 = mark free, 1 = mark allocated */
	__u8 reserved2[39];   /* padding to 64 bytes */
};

/* JRN_TRIE_UPDATE */
struct jrn_trie_update {
	__u64 node_offset;    /* block offset of trie node */
	__u32 range_start;
	__u32 range_len;
	__u32 free_count;
	__u64 left_child;
	__u64 right_child;
	__u32 flags;
	__u32 reserved;
};

/* JRN_DIR_UPDATE */
struct jrn_dir_update {
	__u64 parent_ino;
	__u64 child_ino;
	__u32 name_len;
	__u8 name[251];
	__u8 op;              /* 0 = add, 1 = delete */
	__u8 reserved[6];
};

/* JRN_CHECKPOINT */
struct jrn_checkpoint {
	__u64 checkpoint_seq;
	__u32 record_count;
	__u32 reserved1;
	__u64 log_sequence_end;
	__u64 trie_root_node;
	__u64 free_data_count;
	__u64 free_inode_count;
	__u64 reserved2;
};

/* Journal record sizes */
#define JRN_EXTENT_ALLOC_SIZE 64
#define JRN_EXTENT_FREE_SIZE 32
#define JRN_INODE_UPDATE_SIZE 80
#define JRN_INODE_ALLOC_SIZE 40
#define JRN_INODE_FREE_SIZE 24
#define JRN_BITMAP_UPDATE_SIZE 64
#define JRN_TRIE_UPDATE_SIZE 64
#define JRN_DIR_UPDATE_SIZE 320
#define JRN_CHECKPOINT_SIZE 80

/* Journal block header (16 bytes) */
struct journal_block_header {
	__u32 magic;          /* JOURNAL_MAGIC */
	__u32 block_seq;      /* monotonically increasing */
	__u32 record_count;   /* number of records in this block */
	__u32 reserved;
};

/* Journal block (one block on disk, includes header + records) */
struct journal_block {
	struct journal_block_header header;
	unsigned char records[];
};

/* Superblock - block 0, 4096 bytes */
struct briefs_superblock {
	__u64 magic;

	/* 8 byte version numbers is a bit ridiculous, but it makes memory
	 * alignment a lot easier. At least there's tons of extra space in the
	 * superblock for it.
	 */
	__u64 major_version;
	__u64 minor_version;
	__u64 patch_version;

	__u64 total_blocks;
	__u64 data_blocks;
	__u64 block_size;
	__u64 inode_size;
	__u64 blocks_per_group;
	__u64 inodes_per_group;
	__u64 fs_created;
	__u64 fs_last_mounted;
	__u64 fs_last_checkpoint;
	/* tracked by trie root free_count */
	__u64 free_data_blocks;
	__u64 free_inodes;
	__u64 root_inode_number;
	__u64 feature_compat;
	__u64 feature_ro_compat;
	__u64 feature_incompat;
	
	/* 128-bit uuid for this volume */
	__u8 uuid[16];

	/* The below is particularly subject to change */
	__u64 eat_offset;
	__u64 eat_blocks;
	__u64 trie_root_block;
	__u64 trie_blocks_used;
	__u64 trie_node_pool_start;
	__u64 trie_node_pool_size;
	__u64 inode_bitmap_offset;
	__u64 inode_bitmap_blocks;
	__u64 inode_table_offset;
	__u64 journal_offset;
	__u64 journal_blocks;
	__u64 checkpoint_seq;
	__u64 journal_log_start;
	__u64 journal_log_end;
	__u64 reserved_journal[4];

	/* utf8, null padded */
	unsigned char label[64];

	/* Superblock padding */
	unsigned char reserved[_BRIEFS_SUPER_RESERVED];
}; /* 1024 bytes */

/* Extent entry - 32 bytes */
struct briefs_extent {
	__u64 offset;
	__u64 phys;
	__u64 len;
	__u64 flags;
};

/* Inode - 512 bytes */
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
	struct briefs_extent inline_extents[8];
	__u64 xattr_offset;
	__u64 xattr_size;
	__u64 parent_inode;
	__u32 unused; 		   /* used to duplicate nlinks, renamed */
	__u32 flags;
	__u64 dir_trie_root;       /* block number of directory trie root (dirs only) */
	__u64 rdev;                /* device number (block/char special files) */
	__u8 reserved[80]; /* zero padded to 512 bytes */
};

/* Extent chain for overflow - 256 extents + header = 272 bytes */
struct briefs_extent_chain {
	__u64 next_overflow_block;
	__u32 num_extents_in_block;
	__u32 pad;
	struct briefs_extent extents[256];
	__u64 checksum;
};

/* Function headers and the like */

/* Trie root block - first block in trie node pool */

/* Trie node magic "TRN " - 0x54524E20 */
#define BRIEFS_TRIE_MAGIC 0x54524E20

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
 * The file type (ftype) for leaf entries is stored in the reserved[0]
 * byte of the trie node struct, not in node_type.  node_type is used
 * purely for trie structure (INTERM, STATUS_LEAF).
 */
#define NODE_STATUS_LEAF    0x08

#define NODE_FLAG_DELETED   0x00000004
#define NODE_FLAG_ROOT      0x00000008

/* Trie node - 32 bytes, one per block.
 * Internal nodes dispatch children by a single byte of the filename.
 * Leaf nodes hold the actual entry (name + inode).
 * Names longer than briefs_name_len are not supported.
 * The name itself is stored inline in the node block after this struct header,
 * at BRIEFS_BLOCK_SIZE - name_offset.
 */
struct briefs_trie_node {
	__u32 magic;              /* "TRN " - 0x54524E20 */
	__u32 child_count;        /* number of children (0 for leaf) */
	__u64 first_child;        /* block number of first child node */
	__u64 next_sibling;       /* block number of next sibling at same depth */
	__u8  depth;              /* depth in trie (0 = root) */
	__u8  node_type;          /* NODE_TYPE_* */
	__u8  byte_val;           /* the byte value this node represents (internal nodes) */
	__u8  f_type; 		  /* file type, formerly reserved[0] */
	__u8  reserved[4];
	__u64 flags;              /* NODE_FLAG_* */

	/* Leaf entry data.  Valid when TRIE_IS_LEAF(node) is true —
	 * includes pure leaf nodes and NODE_TYPE_INTERM nodes with
	 * NODE_STATUS_LEAF set.  The file type (ftype) for leaf entries
	 * is stored in ftype (formerly reserved[0]). */
	__u64 inode;              /* inode number */
	__u16 name_len;           /* full name length (not just remaining bytes) */
	__u16 name_offset;        /* offset from block end to name bytes */
};

/*
 * Helper to get/set the file type stored in a trie node's reserved[0].
 * The node_type field is used for trie structure only (INTERM, STATUS_LEAF);
 * the actual file type (ftype from S_IFMT >> 12) lives here.
 */
#define TRIE_NODE_FTYPE(node) ((node)->f_type)
#define TRIE_SET_FTYPE(node, ftype) ((node)->f_type = (ftype))

/*
 * Trie block header - immediately follows trie nodes within a block
 * (currently each node occupies a full block, so this is a placeholder
 *  for future packed-multiple-nodes-per-block optimization).
 *
 * For now, each struct briefs_trie_node fills one 4096-byte block.
 * The name is stored in the trailing bytes of that block.
 */
#define TRIE_NODE_NAME_BASE(node) ((void *)(node) + BRIEFS_BLOCK_SIZE - (node)->name_offset)

/* Maximum name length the trie can store inline (name fits in unused tail of block) */
#define TRIE_MAX_NAME_LEN (BRIEFS_BLOCK_SIZE - sizeof(struct briefs_trie_node))

/* Trie operations - directory trie node allocation (uses data block allocator) */
int briefs_trie_create_root(struct super_block *sb, struct briefs_inode *di);
int briefs_trie_lookup(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len, u64 *found_ino, u8 *found_type);
int briefs_trie_insert(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len, u64 ino, u8 type);
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                       const char *name, int name_len);
void briefs_trie_free_all(struct super_block *sb, struct briefs_inode *di);

/* Trie iterator for readdir - depth-first walk yielding leaves */
struct trie_iter {
	u64 stack[256];
	int sp;
	/* Per-entry flag: 1 if this stack entry had its leaf emitted.
	 * Used so that when an INTERM+NODE_STATUS_LEAF node is re-pushed
	 * after emitting its own leaf, we can skip re-emission on re-visit.
	 * Only valid for entries where stack[i] != 0 (sp entries). */
	u8 leaf_emitted[256];
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

void briefs_trie_iter_init(struct trie_iter *iter, struct briefs_inode *di, u64 gen);
int briefs_trie_iter_next(struct super_block *sb, struct trie_iter *iter, u64 current_gen, u64 *ino, u8 *type, char *name_buf, int *name_len);

/* Compute CRC32 checksum for journal record */
__u32 briefs_crc32c(__u32 crc, const void *data, size_t len);

/* Directory block magic */
#define BRIEFS_DIR_MAGIC 0x44525952  /* "DRYR" */

/*
 * Directory entry — compact, variable-length name.
 * Names are packed into the trailing name region of the directory block
 * (growing downward from block end). Each DirEntry has a name_len and
 * name_off that points into the name region.
 *
 * A name_off of 0 means "no name" (used for empty/deleted slots).
 * name_off is the byte offset FROM THE END of the block:
 *   name_ptr = block + block_size - name_off
 */
struct briefs_dir_entry {
	__u64 inode;              /* inode number */
	__u8  type;               /* file type (S_IFMT bits) */
	__u8  flags;              /* flags */
	__u8  reserved[2];        /* padding to 16-byte alignment */
	__u16 name_len;           /* name length (1..BRIEFS_NAME_LEN) */
	__u16 name_off;           /* offset from block end into name region */
};                            /* 16 bytes total */

/*
 * Directory block header (16 bytes). Followed by a variable-length
 * array of DirEntry structs, then a packed name region that grows
 * downward from block_size.
 *
 *          +----------------+  block_start
 *          | DirBlockHeader |  16 bytes
 *          +----------------+
 *          | DirEntry[0]    |  16 bytes
 *          | DirEntry[1]    |  16 bytes
 *          | ...            |
 *          +----------------+  data_end = sizeof(header) + num_entries * sizeof(DirEntry)
 *          |    unused      |
 *          +----------------+
 *          | name_N  ...    |  names_size bytes
 *          | name_1  ...    |
 *          | name_0  ...    |
 *          +----------------+  block_start + block_size
 *
 * Names are stored with a 2-byte length prefix for forward scanning:
 *   [len:2][name bytes...]
 * The region grows downward, so the newest name is closest to block end.
 * name_off = offset from block end to the start of the name entry
 *            (including the 2-byte length prefix).
 */
struct briefs_dir_block {
	__u32 magic;              /* BRIEFS_DIR_MAGIC */
	__u32 num_entries;        /* number of entries in this block */
	__u32 data_size;          /* bytes used by entries (header + entries) */
	__u32 names_size;         /* bytes used by packed name region */
};

/* Max entries per directory block (rough estimate for safety) */
#define BRIEFS_DIR_MAX_ENTRIES 300

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
		u64 trie_gen;            /* generation counter for trie modifications */
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
	return sb->trie_node_pool_start + sb->trie_blocks_used + rel_block;
}

/* Convert absolute block number back to data-relative */
static inline u64 abs_to_data(struct briefs_superblock *sb, u64 abs_block)
{
	return abs_block - (sb->trie_node_pool_start + sb->trie_blocks_used);
}

/* Convenience: first block of the inode table on disk */
static inline u64 briefs_inode_table_start(struct briefs_superblock *sb)
{
	return sb->inode_table_offset;
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
u64 briefs_compute_i_blocks(struct briefs_inode *di);
int briefs_read_extent(struct super_block *sb, struct briefs_inode *di, int index, struct briefs_extent *ext);
int briefs_append_extent(struct super_block *sb, struct briefs_inode *di, struct briefs_extent *ext);
int briefs_write_inode(struct inode *inode, struct writeback_control *wbc);

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
ssize_t briefs_read_iter(struct kiocb *iocb, struct iov_iter *to);
ssize_t briefs_write_iter(struct kiocb *iocb, struct iov_iter *from);
int briefs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
                   struct iattr *attr);
int briefs_getattr(struct mnt_idmap *idmap, const struct path *path,
                   struct kstat *stat, u32 request_mask, unsigned int query_flags);
int briefs_fsync(struct file *file, loff_t start, loff_t end, int datasync);
int briefs_open(struct inode *inode, struct file *file);
int briefs_release(struct inode *inode, struct file *file);

/* Directory operations */
int briefs_readdir(struct file *file, struct dir_context *ctx);

/* Superblock operations */
int briefs_statfs(struct dentry *dentry, struct kstatfs *buf);
void briefs_put_super(struct super_block *sb);
void briefs_kill_sb(struct super_block *sb);

#endif /* __KERNEL__ */

#endif /* _BRIEFS_H */
