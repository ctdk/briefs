// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * BrieFS extended attributes.
 *
 * Each inode may own one or more chained external 4096-byte xattr blocks
 * allocated from the data block pool.  The first block is referenced by the
 * inode's xattr_offset (absolute block) / xattr_size (bytes used in the first
 * block).  Large values are split across continuation blocks linked by
 * briefs_xattr_header.next_block.  The block format is defined in briefs.h.
 * Names are stored with their namespace prefix.
 *
 * The three handlers (user / trusted / security) are published via
 * sb->s_xattr; the VFS resolves get/set by prefix and auto-sets IOP_XATTR on
 * every inode.  listxattr(2) uses briefs_xattr_list (.listxattr inode op),
 * which reads the on-disk block -- generic_listxattr would list nothing
 * because it only emits .name-set (exact-match) handlers, not prefix ones.
 *
 * Durability: an xattr block cannot be re-derived on journal replay (it is
 * user data, unlike directory tries), so each block write logs its content
 * via JRN_XATTR_DATA before the JRN_INODE_FULL that publishes xattr_offset.
 * xattr_sem (an rwsem) serializes block read/modify/free against get/list,
 * which the VFS does not protect with i_rwsem.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/xattr.h>
#include <linux/security.h>
#include <linux/capability.h>
#include <linux/buffer_head.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"
#include "briefs_xattr.h"

/* ---- on-disk block helpers -------------------------------------------- */

static inline __le64 *xattr_crc_slot(void *block_data)
{
	return (__le64 *)((char *)block_data + BRIEFS_BLOCK_SIZE - 2 * sizeof(__u64));
}

/* Load header fields, normalising both v1 (16-byte) and v2 (32-byte) headers.
 * Output pointers may be NULL for fields the caller does not need.
 */
static void xattr_header_load(const void *block_data, u32 *version,
			      u32 *used_size, u32 *entry_count, u64 *next_block,
			      u32 *flags)
{
	const struct briefs_xattr_header *hdr = block_data;
	u32 ver;

	ver = le32_to_cpu(hdr->version);
	if (version)
		*version = ver;
	if (used_size)
		*used_size = le32_to_cpu(hdr->used_size);
	if (entry_count)
		*entry_count = le32_to_cpu(hdr->entry_count);
	if (ver == 1) {
		if (next_block)
			*next_block = 0;
		if (flags)
			*flags = 0;
	} else {
		if (next_block)
			*next_block = le64_to_cpu(hdr->next_block);
		if (flags)
			*flags = le32_to_cpu(hdr->flags);
	}
}

/* Read and validate a single xattr block by absolute block number.  Returns
 * the buffer_head (caller brelse) or ERR_PTR(-EIO) on I/O or corruption.
 */
static struct buffer_head *briefs_xattr_block_read_abs(struct super_block *sb,
						       u64 block)
{
	struct buffer_head *bh;
	struct briefs_xattr_header *hdr;
	u32 magic, version, used_size;

	bh = sb_bread(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);

	hdr = (struct briefs_xattr_header *)bh->b_data;
	magic = le32_to_cpu(hdr->magic);
	if (magic != BRIEFS_XATTR_MAGIC) {
		pr_warn("briefs: xattr block %llu bad magic 0x%08x\n", block, magic);
		brelse(bh);
		return ERR_PTR(-EIO);
	}

	version = le32_to_cpu(hdr->version);
	if (version != 1 && version != 2) {
		pr_warn("briefs: xattr block %llu unsupported version %u\n", block, version);
		brelse(bh);
		return ERR_PTR(-EIO);
	}

	used_size = le32_to_cpu(hdr->used_size);
	if (used_size == 0 || used_size > BRIEFS_XATTR_MAX_USED ||
	    used_size < briefs_xattr_hdr_size(version) ||
	    briefs_verify_chain_checksum(bh->b_data, *xattr_crc_slot(bh->b_data)) != 0) {
		pr_warn("briefs: xattr block %llu bad CRC/size\n", block);
		brelse(bh);
		return ERR_PTR(-EIO);
	}
	return bh;
}

/* Read the head of an inode's xattr chain.  Returns ERR_PTR(-ENODATA) if the
 * inode has no xattr block, ERR_PTR(-EIO) on corruption.
 */
static struct buffer_head *briefs_xattr_block_read(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	u64 block;

	block = binfo->disk_inode.xattr_offset;
	if (block == 0)
		return ERR_PTR(-ENODATA);
	return briefs_xattr_block_read_abs(inode->i_sb, block);
}

/**
 * briefs_xattr_chain_next - return the next block in the chain, releasing @bh.
 * Returns NULL at end of chain, ERR_PTR(-EIO) on error.
 */
static struct buffer_head *briefs_xattr_chain_next(struct super_block *sb,
						    struct buffer_head *bh)
{
	u64 next;

	xattr_header_load(bh->b_data, NULL, NULL, NULL, &next, NULL);
	brelse(bh);
	if (next == 0)
		return NULL;
	return briefs_xattr_block_read_abs(sb, next);
}

/* Locate an entry by full name within one block.  Returns the entry or NULL. */
static struct briefs_xattr_entry *
briefs_xattr_find_in_block(struct buffer_head *bh, const char *name,
			   size_t name_len)
{
	struct briefs_xattr_header *hdr = (struct briefs_xattr_header *)bh->b_data;
	struct briefs_xattr_entry *entries;
	u32 count, i, hdr_size;

	hdr_size = briefs_xattr_hdr_size(le32_to_cpu(hdr->version));
	count = le32_to_cpu(hdr->entry_count);
	entries = (struct briefs_xattr_entry *)(bh->b_data + hdr_size);
	for (i = 0; i < count; i++) {
		struct briefs_xattr_entry *e = &entries[i];
		u32 enlen = le16_to_cpu(e->name_len);

		if (enlen == name_len &&
		    memcmp(bh->b_data + le16_to_cpu(e->name_offset), name, name_len) == 0)
			return e;
	}
	return NULL;
}

/* True if the block is a continuation block (raw value bytes, no entries). */
static bool xattr_block_is_cont(const void *block_data)
{
	u32 flags;

	xattr_header_load(block_data, NULL, NULL, NULL, NULL, &flags);
	return (flags & BRIEFS_XATTR_FLAG_CONT) != 0;
}

/* Find an entry across the whole chain.  Returns the entry and its block in
 * @out_bh on success, or NULL if not found.  Returns ERR_PTR(-EIO) on chain
 * corruption; the caller must brelse @out_bh on success.
 */
static struct briefs_xattr_entry *
briefs_xattr_find(struct inode *inode, const char *name, size_t name_len,
		  struct buffer_head **out_bh)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;

	bh = briefs_xattr_block_read(inode);
	if (IS_ERR(bh)) {
		*out_bh = bh;
		return NULL;
	}

	while (bh) {
		struct briefs_xattr_entry *e;

		if (!xattr_block_is_cont(bh->b_data))
			e = briefs_xattr_find_in_block(bh, name, name_len);
		else
			e = NULL;

		if (e) {
			*out_bh = bh;
			return e;
		}

		bh = briefs_xattr_chain_next(sb, bh);
		if (IS_ERR(bh)) {
			*out_bh = bh;
			return NULL;
		}
	}

	*out_bh = NULL;
	return NULL;
}

/* Copy a value that may span continuation blocks.  @first_bh is the entry
 * block containing @e.  Releases every block it reads.  Returns 0 or -EIO.
 */
static int xattr_copy_value(struct super_block *sb, struct buffer_head *first_bh,
			    const struct briefs_xattr_entry *e,
			    void *value, u32 value_len)
{
	struct buffer_head *bh = first_bh;
	u32 copied = 0;
	u32 frag_off = le16_to_cpu(e->value_offset);
	int ret = 0;

	while (copied < value_len) {
		u32 version, used, hdr_size, flags;
		u32 payload_start, avail, take;
		struct buffer_head *next;

		/* value_offset == 0 with a non-empty value means the whole value is
		 * stored in continuation block(s); skip the entry block.
		 */
		if (bh == first_bh && frag_off == 0 && value_len > 0) {
			next = briefs_xattr_chain_next(sb, bh);
			if (IS_ERR(next)) {
				ret = PTR_ERR(next);
				bh = NULL;
				break;
			}
			if (!next) {
				ret = -EIO;
				bh = NULL;
				break;
			}
			bh = next;
			continue;
		}

		xattr_header_load(bh->b_data, &version, &used, NULL, NULL, &flags);
		hdr_size = briefs_xattr_hdr_size(version);

		if (bh == first_bh) {
			payload_start = frag_off;
		} else if (flags & BRIEFS_XATTR_FLAG_CONT) {
			payload_start = hdr_size;
		} else {
			/* A non-continuation block in the middle of a value is corrupt. */
			ret = -EIO;
			break;
		}

		if (payload_start > used) {
			ret = -EIO;
			break;
		}
		avail = used - payload_start;
		take = min(avail, value_len - copied);
		if (take == 0) {
			ret = -EIO;
			break;
		}
		memcpy(value + copied, bh->b_data + payload_start, take);
		copied += take;

		if (copied >= value_len)
			break;

		{
			struct buffer_head *next = briefs_xattr_chain_next(sb, bh);

			if (IS_ERR(next)) {
				ret = PTR_ERR(next);
				bh = NULL;
				break;
			}
			if (!next) {
				ret = -EIO;
				bh = NULL;
				break;
			}
			bh = next;
		}
	}

	if (bh)
		brelse(bh);
	return ret;
}

/* ---- get / list ------------------------------------------------------- */

int briefs_xattr_get(struct inode *inode, const char *name,
		     void *value, size_t size)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct buffer_head *bh = NULL;
	struct briefs_xattr_entry *e;
	size_t name_len;
	int ret;

	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;

	down_read(&binfo->xattr_sem);
	e = briefs_xattr_find(inode, name, name_len, &bh);
	if (IS_ERR(bh)) {
		ret = PTR_ERR(bh);
		bh = NULL;
		goto out;
	}
	if (!e) {
		ret = -ENODATA;
		goto out;
	}

	ret = le16_to_cpu(e->value_len);
	if (value) {
		if (size < (size_t)ret) {
			ret = -ERANGE;
		} else if (ret > 0) {
			ret = xattr_copy_value(inode->i_sb, bh, e, value, ret);
			if (ret == 0)
				ret = le16_to_cpu(e->value_len);
		}
	}

	brelse(bh);
out:
	up_read(&binfo->xattr_sem);
	return ret;
}

ssize_t briefs_xattr_list(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	ssize_t ret = 0;
	size_t rest = size;

	down_read(&binfo->xattr_sem);
	bh = briefs_xattr_block_read(inode);
	if (IS_ERR(bh)) {
		ret = PTR_ERR(bh);
		if (ret == -EIO)
			goto out;
		/* No xattr block: empty list.  Do not brelse the ERR_PTR. */
		bh = NULL;
		ret = 0;
		goto out;
	}

	while (bh) {
		struct briefs_xattr_header *hdr;
		struct briefs_xattr_entry *entries;
		u32 count, i, hdr_size;

		if (xattr_block_is_cont(bh->b_data)) {
			struct buffer_head *next;

			next = briefs_xattr_chain_next(sb, bh);
			if (IS_ERR(next)) {
				ret = PTR_ERR(next);
				bh = NULL;
				goto out;
			}
			bh = next;
			continue;
		}

		hdr = (struct briefs_xattr_header *)bh->b_data;
		hdr_size = briefs_xattr_hdr_size(le32_to_cpu(hdr->version));
		count = le32_to_cpu(hdr->entry_count);
		entries = (struct briefs_xattr_entry *)(bh->b_data + hdr_size);

		for (i = 0; i < count; i++) {
			struct briefs_xattr_entry *e = &entries[i];
			u32 nlen = le16_to_cpu(e->name_len);
			size_t need = nlen + 1;	/* name + NUL */

			if (buffer) {
				if (need > rest) {
					ret = -ERANGE;
					brelse(bh);
					bh = NULL;
					goto out;
				}
				memcpy(buffer, bh->b_data + le16_to_cpu(e->name_offset), nlen);
				buffer += nlen;
				*buffer++ = '\0';
			}
			rest -= need;
		}

		bh = briefs_xattr_chain_next(sb, bh);
		if (IS_ERR(bh)) {
			ret = PTR_ERR(bh);
			bh = NULL;
			goto out;
		}
	}

	ret = size - rest;
out:
	if (bh && !IS_ERR(bh))
		brelse(bh);
	up_read(&binfo->xattr_sem);
	return ret;
}

/* ---- set / free ------------------------------------------------------- */

/* In-memory key/value collected from the existing block while rebuilding. */
struct xattr_kv {
	char *name;		/* owned, no trailing NUL kept; name_len bytes */
	u32 name_len;
	void *value;		/* owned, NULL when value_len == 0 */
	u32 value_len;
};

static void xattr_kv_free(struct xattr_kv *kvs, u32 n)
{
	u32 i;

	for (i = 0; i < n; i++) {
		kfree(kvs[i].name);
		kfree(kvs[i].value);
	}
	kfree(kvs);
}

/* Maximum number of 8-byte entry records that fit in one block's payload. */
#define XATTR_BLOCK_MAX_ENTRIES 501

/* In-memory descriptor for one block of the new xattr chain. */
struct xattr_block_desc {
	bool cont;
	u32 payload_used;
	union {
		struct {
			u32 kv[XATTR_BLOCK_MAX_ENTRIES];
			u32 inline_len[XATTR_BLOCK_MAX_ENTRIES];
			u32 n;
		} e;
		struct {
			u32 kv_idx;
			u32 value_off;
			u32 frag_len;
		} c;
	};
};

static void xattr_chain_descs_free(struct xattr_block_desc **descs, u32 nblocks)
{
	u32 i;

	if (!descs)
		return;
	for (i = 0; i < nblocks; i++)
		kfree(descs[i]);
	kfree(descs);
}

static int xattr_add_entry_block(struct xattr_block_desc ***blocks, u32 *nblocks)
{
	struct xattr_block_desc *d;
	struct xattr_block_desc **grown;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	grown = krealloc(*blocks, (*nblocks + 1) * sizeof(**blocks), GFP_KERNEL);
	if (!grown) {
		kfree(d);
		return -ENOMEM;
	}
	*blocks = grown;
	(*blocks)[(*nblocks)++] = d;
	return 0;
}

static int xattr_add_cont_block(struct xattr_block_desc ***blocks, u32 *nblocks,
				u32 kv_idx, u32 value_off, u32 frag_len)
{
	struct xattr_block_desc *d;
	struct xattr_block_desc **grown;

	d = kzalloc(sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->cont = true;
	d->payload_used = frag_len;
	d->c.kv_idx = kv_idx;
	d->c.value_off = value_off;
	d->c.frag_len = frag_len;
	grown = krealloc(*blocks, (*nblocks + 1) * sizeof(**blocks), GFP_KERNEL);
	if (!grown) {
		kfree(d);
		return -ENOMEM;
	}
	*blocks = grown;
	(*blocks)[(*nblocks)++] = d;
	return 0;
}

/* A loaded xattr chain: buffer_heads for every block, in order. */
struct xattr_chain {
	struct buffer_head **bhs;
	u32 count;
};

static void xattr_chain_release(struct xattr_chain *chain)
{
	u32 i;

	if (!chain)
		return;
	for (i = 0; i < chain->count; i++)
		brelse(chain->bhs[i]);
	kfree(chain->bhs);
	chain->bhs = NULL;
	chain->count = 0;
}

/* Load the entire on-disk chain for @inode into @chain.  Returns 0, -ENODATA,
 * or -EIO.  Caller holds xattr_sem (read or write).
 */
static int xattr_chain_load(struct inode *inode, struct xattr_chain *chain)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct buffer_head **grown;

	chain->bhs = NULL;
	chain->count = 0;

	bh = briefs_xattr_block_read(inode);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	while (bh) {
		if (chain->count >= BRIEFS_XATTR_MAX_CHAIN) {
			pr_warn("briefs: xattr chain too long at ino=%lu\n", inode->i_ino);
			brelse(bh);
			xattr_chain_release(chain);
			return -EIO;
		}
		grown = krealloc(chain->bhs,
				 (chain->count + 1) * sizeof(*chain->bhs),
				 GFP_KERNEL);
		if (!grown) {
			brelse(bh);
			xattr_chain_release(chain);
			return -ENOMEM;
		}
		chain->bhs = grown;
		chain->bhs[chain->count++] = bh;

		bh = briefs_xattr_chain_next(sb, bh);
		if (IS_ERR(bh)) {
			xattr_chain_release(chain);
			return PTR_ERR(bh);
		}
	}
	return 0;
}

/* Copy one entry's full value from a loaded chain into newly allocated memory.
 * Returns 0 or -EIO/-ENOMEM.
 */
static int xattr_chain_load_value(struct xattr_chain *chain, u32 entry_block,
				  const struct briefs_xattr_entry *e,
				  void **out_value, u32 value_len)
{
	void *value;
	u32 copied = 0;
	u32 frag_off = le16_to_cpu(e->value_offset);
	u32 block_idx = entry_block;

	if (value_len == 0) {
		*out_value = NULL;
		return 0;
	}

	value = kmalloc(value_len, GFP_KERNEL);
	if (!value)
		return -ENOMEM;

	while (copied < value_len) {
		struct buffer_head *bh = chain->bhs[block_idx];
		u32 version, used, hdr_size, flags;
		u32 payload_start, avail, take;

		/* value_offset == 0 with a non-empty value means the value starts in
		 * the next continuation block.
		 */
		if (block_idx == entry_block && frag_off == 0 && value_len > 0) {
			if (++block_idx >= chain->count) {
				kfree(value);
				return -EIO;
			}
			continue;
		}

		xattr_header_load(bh->b_data, &version, &used, NULL, NULL, &flags);
		hdr_size = briefs_xattr_hdr_size(version);

		if (block_idx == entry_block) {
			payload_start = frag_off;
		} else if (flags & BRIEFS_XATTR_FLAG_CONT) {
			payload_start = hdr_size;
		} else {
			kfree(value);
			return -EIO;
		}

		if (payload_start > used) {
			kfree(value);
			return -EIO;
		}
		avail = used - payload_start;
		take = min(avail, value_len - copied);
		if (take == 0) {
			kfree(value);
			return -EIO;
		}
		memcpy(value + copied, bh->b_data + payload_start, take);
		copied += take;

		if (copied >= value_len)
			break;
		if (++block_idx >= chain->count) {
			kfree(value);
			return -EIO;
		}
	}

	*out_value = value;
	return 0;
}

/* Read all entries (with full values, including split values) from @inode's
 * xattr chain into an allocated @kvs array and set @out_n.  Returns 0,
 * -ENODATA, -EIO, or -ENOMEM.  Caller must xattr_kv_free(*out_kvs, *out_n).
 */
static int briefs_xattr_load_entries(struct inode *inode,
				     struct xattr_kv **out_kvs, u32 *out_n)
{
	struct xattr_chain chain = { NULL, 0 };
	struct xattr_kv *kvs = NULL;
	u32 n = 0;
	int ret;
	u32 i;

	ret = xattr_chain_load(inode, &chain);
	if (ret)
		return ret;

	for (i = 0; i < chain.count; i++) {
		struct buffer_head *bh = chain.bhs[i];
		struct briefs_xattr_header *hdr;
		u32 version, entry_count, j, hdr_size;
		struct briefs_xattr_entry *entries;

		if (xattr_block_is_cont(bh->b_data))
			continue;

		hdr = (struct briefs_xattr_header *)bh->b_data;
		version = le32_to_cpu(hdr->version);
		hdr_size = briefs_xattr_hdr_size(version);
		entry_count = le32_to_cpu(hdr->entry_count);
		entries = (struct briefs_xattr_entry *)(bh->b_data + hdr_size);

		for (j = 0; j < entry_count; j++) {
			struct briefs_xattr_entry *e = &entries[j];
			struct xattr_kv *grown;
			u32 nl = le16_to_cpu(e->name_len);
			u32 vl = le16_to_cpu(e->value_len);
			void *value = NULL;

			grown = krealloc(kvs, (n + 1) * sizeof(*kvs), GFP_KERNEL);
			if (!grown) {
				ret = -ENOMEM;
				goto out;
			}
			kvs = grown;

			kvs[n].name = kmalloc(nl, GFP_KERNEL);
			if (!kvs[n].name) {
				ret = -ENOMEM;
				goto out;
			}
			memcpy(kvs[n].name, bh->b_data + le16_to_cpu(e->name_offset), nl);
			kvs[n].name_len = nl;

			ret = xattr_chain_load_value(&chain, i, e, &value, vl);
			if (ret) {
				kfree(kvs[n].name);
				goto out;
			}
			kvs[n].value = value;
			kvs[n].value_len = vl;
			n++;
		}
	}

	*out_kvs = kvs;
	*out_n = n;
	ret = 0;
out:
	xattr_chain_release(&chain);
	if (ret && kvs)
		xattr_kv_free(kvs, n);
	return ret;
}

/*
 * Free every block in an on-disk xattr chain starting at @head.  Logs each
 * JRN_EXTENT_FREE.  Caller holds xattr_sem write.  The inode pointer must
 * already have been cleared/persisted so replay never sees a freed block as
 * live.
 */
static void briefs_xattr_free_chain_locked(struct inode *inode, u64 head)
{
	struct super_block *sb = inode->i_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	u64 block = head;
	u32 visited = 0;

	while (block != 0) {
		u64 next;

		if (++visited > BRIEFS_XATTR_MAX_CHAIN) {
			pr_warn("briefs: xattr free loop detected at ino=%lu\n",
				inode->i_ino);
			break;
		}

		bh = briefs_xattr_block_read_abs(sb, block);
		if (IS_ERR(bh)) {
			/* Leak the rest if the chain is unreadable. */
			break;
		}
		xattr_header_load(bh->b_data, NULL, NULL, NULL, &next, NULL);
		brelse(bh);

		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
		if (bsi->journal && !bsi->journal->in_replay)
			briefs_journal_extent_free(bsi->journal, inode->i_ino, 0,
						   block, 1);
		block = next;
	}
}

/*
 * Clear xattr_offset/size, persist + journal the cleared inode, then free the
 * whole xattr chain.  Caller holds xattr_sem write and has already updated
 * i_ctime if appropriate.  No-op if there is no block.
 */
static void briefs_xattr_free_block_locked(struct inode *inode)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct briefs_disk_inode disk_di;
	u64 old_block;

	old_block = binfo->disk_inode.xattr_offset;
	if (old_block == 0)
		return;

	/*
	 * Publish the cleared pointer before freeing the chain, so replay never
	 * sees an inode pointing at a freed block.
	 */
	binfo->disk_inode.xattr_offset = 0;
	binfo->disk_inode.xattr_size = 0;
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
				  &binfo->disk_inode, false);
	if (bsi->journal && !bsi->journal->in_replay)
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

	briefs_xattr_free_chain_locked(inode, old_block);
}

/* Pack @n entries into one or more xattr blocks.  Each entry block stores entry
 * records plus as much of each value as fits inline; any remainder is placed in
 * continuation block(s) chained via briefs_xattr_header.next_block.  Returns
 * the descriptor array and block count, or a negative errno.
 */
static int xattr_build_chain(struct xattr_kv *kvs, u32 n,
			     struct xattr_block_desc ***out_descs,
			     u32 *out_nblocks)
{
	struct xattr_block_desc **descs = NULL;
	u32 nblocks = 0;
	u32 cur_eb = 0;
	u32 hdr_size = sizeof(struct briefs_xattr_header);
	u32 i;
	int ret = 0;

	*out_descs = NULL;
	*out_nblocks = 0;

	if (n == 0)
		return 0;

	if (xattr_add_entry_block(&descs, &nblocks))
		return -ENOMEM;

	for (i = 0; i < n; i++) {
		u32 name_len = kvs[i].name_len;
		u32 value_len = kvs[i].value_len;
		u32 need_entry = sizeof(struct briefs_xattr_entry) +
				 ALIGN(name_len, 4);
		u32 inline_len, max_payload, remaining;

again:
		max_payload = BRIEFS_XATTR_MAX_USED - hdr_size -
			      descs[cur_eb]->e.n *
			      sizeof(struct briefs_xattr_entry);

		/*
		 * Not enough payload capacity for even this entry's name/record
		 * (or the entry array is full): close the current entry block and
		 * start a fresh one.
		 */
		if (need_entry > max_payload - descs[cur_eb]->payload_used ||
		    descs[cur_eb]->e.n >= XATTR_BLOCK_MAX_ENTRIES) {
			if (xattr_add_entry_block(&descs, &nblocks))
				goto fail;
			cur_eb = nblocks - 1;
			goto again;
		}

		remaining = max_payload - descs[cur_eb]->payload_used - need_entry;
		if (value_len == 0 || remaining < 4)
			inline_len = 0;
		else if (ALIGN(value_len, 4) <= remaining)
			inline_len = value_len;
		else
			inline_len = min(value_len, remaining & ~3u);

		descs[cur_eb]->e.kv[descs[cur_eb]->e.n] = i;
		descs[cur_eb]->e.inline_len[descs[cur_eb]->e.n] = inline_len;
		descs[cur_eb]->e.n++;
		descs[cur_eb]->payload_used += need_entry + ALIGN(inline_len, 4);

		if (value_len > inline_len) {
			u32 rem = value_len - inline_len;
			u32 off = inline_len;

			while (rem > 0) {
				u32 cap = BRIEFS_XATTR_MAX_USED - hdr_size;
				u32 frag = min(rem, cap);

				if (xattr_add_cont_block(&descs, &nblocks, i, off, frag))
					goto fail;
				rem -= frag;
				off += frag;
			}
		}
	}

	if (nblocks > BRIEFS_XATTR_MAX_CHAIN) {
		ret = -ENOSPC;
		goto fail;
	}

	*out_descs = descs;
	*out_nblocks = nblocks;
	return 0;

fail:
	xattr_chain_descs_free(descs, nblocks);
	return ret;
}

int briefs_xattr_set(struct inode *inode, const char *name,
		     const void *value, size_t size, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct xattr_kv *kvs = NULL;
	struct xattr_block_desc **descs = NULL;
	u64 *rel_blocks = NULL, *abs_blocks = NULL;
	struct buffer_head **bhs = NULL;
	struct briefs_disk_inode disk_di;
	u32 n = 0, nblocks = 0;
	size_t name_len;
	bool removing = !value;
	bool found = false;
	bool dirtied = false;
	u64 old_head;
	u32 i, j;
	int ret = 0;

	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;
	if (size > XATTR_SIZE_MAX)
		return -E2BIG;

	down_write(&binfo->xattr_sem);

	old_head = binfo->disk_inode.xattr_offset;

	/* Load the existing xattr chain into owned memory. */
	ret = briefs_xattr_load_entries(inode, &kvs, &n);
	if (ret == -ENODATA) {
		ret = 0;
		n = 0;
		kvs = NULL;
	} else if (ret) {
		goto out;
	}

	/* Locate the target entry. */
	for (i = 0; i < n; i++) {
		if (kvs[i].name_len == name_len &&
		    memcmp(kvs[i].name, name, name_len) == 0) {
			found = true;
			break;
		}
	}

	if (removing) {
		if (!found) {
			ret = -ENODATA;	/* XATTR_REPLACE set by removexattr */
			goto out;
		}
		/* Drop the entry. */
		kfree(kvs[i].name);
		kfree(kvs[i].value);
		kvs[i] = kvs[n - 1];
		n--;
	} else {
		void *nv = size ? kmalloc(size, GFP_KERNEL) : NULL;

		if (size && !nv) {
			ret = -ENOMEM;
			goto out;
		}
		if (size)
			memcpy(nv, value, size);

		if (found) {
			if (flags & XATTR_CREATE) {
				kfree(nv);
				ret = -EEXIST;
				goto out;
			}
			kfree(kvs[i].value);
			kvs[i].value = nv;
			kvs[i].value_len = size;
		} else {
			struct xattr_kv *grown;

			if (flags & XATTR_REPLACE) {
				kfree(nv);
				ret = -ENODATA;
				goto out;
			}
			grown = krealloc(kvs, (n + 1) * sizeof(*kvs), GFP_KERNEL);
			if (!grown) {
				kfree(nv);
				ret = -ENOMEM;
				goto out;
			}
			kvs = grown;
			kvs[n].name_len = name_len;
			kvs[n].name = kmalloc(name_len, GFP_KERNEL);
			if (!kvs[n].name) {
				kfree(nv);
				ret = -ENOMEM;
				goto out;
			}
			memcpy(kvs[n].name, name, name_len);
			kvs[n].value = nv;
			kvs[n].value_len = size;
			n++;
		}
	}

	if (n == 0) {
		/* Last entry removed: free the chain. */
		inode_set_ctime_to_ts(inode, current_time(inode));
		briefs_xattr_free_block_locked(inode);
		dirtied = true;
		goto out;
	}

	ret = xattr_build_chain(kvs, n, &descs, &nblocks);
	if (ret)
		goto out;

	rel_blocks = kmalloc_array(nblocks, sizeof(*rel_blocks), GFP_KERNEL);
	abs_blocks = kmalloc_array(nblocks, sizeof(*abs_blocks), GFP_KERNEL);
	bhs = kmalloc_array(nblocks, sizeof(*bhs), GFP_KERNEL);
	if (!rel_blocks || !abs_blocks || !bhs) {
		ret = -ENOMEM;
		goto out_chain;
	}

	/* Allocate a fresh on-disk block for every descriptor. */
	for (i = 0; i < nblocks; i++) {
		rel_blocks[i] = briefs_alloc_block(&bsi->alloc);
		if (rel_blocks[i] == 0) {
			ret = -ENOSPC;
			goto out_chain;
		}
		abs_blocks[i] = data_to_abs(bsi->sb, rel_blocks[i]);
		bhs[i] = briefs_get_zero_block(sb, abs_blocks[i]);
		if (!bhs[i]) {
			briefs_free_block(&bsi->alloc, rel_blocks[i]);
			rel_blocks[i] = 0;
			ret = -EIO;
			goto out_chain;
		}
	}

	/* Serialize each descriptor into its buffer and link the chain. */
	for (i = 0; i < nblocks; i++) {
		struct xattr_block_desc *d = descs[i];
		struct buffer_head *bh = bhs[i];
		struct briefs_xattr_header *hdr = (struct briefs_xattr_header *)bh->b_data;
		u32 used;

		hdr->magic = cpu_to_le32(BRIEFS_XATTR_MAGIC);
		hdr->version = cpu_to_le32(BRIEFS_XATTR_VERSION);
		hdr->next_block = cpu_to_le64(i + 1 < nblocks ? abs_blocks[i + 1] : 0);
		hdr->reserved = 0;

		if (d->cont) {
			u32 kv_idx = d->c.kv_idx;
			u32 off = d->c.value_off;
			u32 frag = d->c.frag_len;

			hdr->flags = cpu_to_le32(BRIEFS_XATTR_FLAG_CONT);
			hdr->entry_count = 0;
			used = sizeof(*hdr) + frag;
			if (frag)
				memcpy(bh->b_data + sizeof(*hdr),
				       kvs[kv_idx].value + off, frag);
		} else {
			struct briefs_xattr_entry *entries;
			u32 off;

			hdr->flags = 0;
			hdr->entry_count = cpu_to_le32(d->e.n);
			entries = (struct briefs_xattr_entry *)(bh->b_data + sizeof(*hdr));
			off = sizeof(*hdr) + d->e.n * sizeof(*entries);
			for (j = 0; j < d->e.n; j++) {
				u32 kv_idx = d->e.kv[j];
				u32 inline_len = d->e.inline_len[j];
				struct xattr_kv *kv = &kvs[kv_idx];

				entries[j].name_len = cpu_to_le16(kv->name_len);
				entries[j].value_len = cpu_to_le16(kv->value_len);
				entries[j].name_offset = cpu_to_le16(off);
				memcpy(bh->b_data + off, kv->name, kv->name_len);
				off += ALIGN(kv->name_len, 4);
				if (kv->value_len == 0) {
					entries[j].value_offset = 0;
				} else if (inline_len == 0) {
					/* Sentinel: the whole value lives in the
					 * continuation block(s).
					 */
					entries[j].value_offset = 0;
				} else {
					entries[j].value_offset = cpu_to_le16(off);
					memcpy(bh->b_data + off, kv->value, inline_len);
					off += ALIGN(inline_len, 4);
				}
			}
			used = off;
		}
		hdr->used_size = cpu_to_le32(used);
		memset(bh->b_data + used, 0, BRIEFS_BLOCK_SIZE - used);
		*xattr_crc_slot(bh->b_data) =
			cpu_to_le64(briefs_chain_checksum(bh->b_data));
		mark_buffer_dirty(bh);
	}

	/* Make the chain durable before publishing the inode pointer. */
	for (i = 0; i < nblocks; i++) {
		sync_dirty_buffer(bhs[i]);
		if (briefs_check_meta_write_error(bhs[i])) {
			briefs_handle_meta_write_error(sb, "xattr chain sync");
			ret = -EIO;
			goto out_chain;
		}
	}

	/* Log the content of every block before the inode pointer. */
	if (bsi->journal && !bsi->journal->in_replay) {
		for (i = 0; i < nblocks; i++) {
			u32 used = le32_to_cpu(
				((struct briefs_xattr_header *)bhs[i]->b_data)->used_size);

			ret = briefs_journal_xattr_data(bsi->journal, inode->i_ino,
							abs_blocks[i], used,
							bhs[i]->b_data);
			if (ret)
				goto out_chain;
		}
	}

	/* Publish the new xattr head and size, then persist + journal the inode. */
	binfo->disk_inode.xattr_offset = abs_blocks[0];
	binfo->disk_inode.xattr_size =
		le32_to_cpu(((struct briefs_xattr_header *)bhs[0]->b_data)->used_size);
	inode_set_ctime_to_ts(inode, current_time(inode));
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_persist_disk_inode(sb, inode->i_ino, &binfo->disk_inode, false);
	if (bsi->journal && !bsi->journal->in_replay)
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

	/* Drop buffer references now that the chain is published. */
	for (i = 0; i < nblocks; i++) {
		brelse(bhs[i]);
		bhs[i] = NULL;
	}

	/* Free the old chain (inode pointer already points at the new head). */
	if (old_head != 0)
		briefs_xattr_free_chain_locked(inode, old_head);

	dirtied = true;

out_chain:
	if (ret) {
		/* Failure path: free any newly allocated blocks and quench their
		 * dirty buffers so stale xattr content cannot be written back.
		 */
		for (i = 0; i < nblocks; i++) {
			if (rel_blocks[i] != 0)
				briefs_free_block(&bsi->alloc, rel_blocks[i]);
		}
		if (bhs) {
			for (i = 0; i < nblocks; i++) {
				if (bhs[i]) {
					clear_buffer_dirty(bhs[i]);
					brelse(bhs[i]);
				}
			}
		}
	} else {
		if (bhs) {
			for (i = 0; i < nblocks; i++)
				brelse(bhs[i]);
		}
	}
	kfree(rel_blocks);
	kfree(abs_blocks);
	kfree(bhs);
	xattr_chain_descs_free(descs, nblocks);
out:
	if (kvs)
		xattr_kv_free(kvs, n);
	if (dirtied)
		mark_inode_dirty(inode);
	up_write(&binfo->xattr_sem);
	return ret;
}

void briefs_xattr_free(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);

	down_write(&binfo->xattr_sem);
	briefs_xattr_free_block_locked(inode);
	up_write(&binfo->xattr_sem);
}

/* ---- handlers --------------------------------------------------------- */

static bool briefs_xattr_user_list(struct dentry *dentry)
{
	return true;
}

static int briefs_xattr_user_get(const struct xattr_handler *handler,
				 struct dentry *unused, struct inode *inode,
				 const char *name, void *buffer, size_t size)
{
	return briefs_xattr_get(inode, xattr_full_name(handler, name),
				buffer, size);
}

static int briefs_xattr_user_set(const struct xattr_handler *handler,
				 struct mnt_idmap *idmap,
				 struct dentry *unused, struct inode *inode,
				 const char *name, const void *value,
				 size_t size, int flags)
{
	return briefs_xattr_set(inode, xattr_full_name(handler, name),
				value, size, flags);
}

static bool briefs_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static int briefs_xattr_trusted_get(const struct xattr_handler *handler,
				    struct dentry *unused, struct inode *inode,
				    const char *name, void *buffer, size_t size)
{
	return briefs_xattr_get(inode, xattr_full_name(handler, name),
				buffer, size);
}

static int briefs_xattr_trusted_set(const struct xattr_handler *handler,
				    struct mnt_idmap *idmap,
				    struct dentry *unused, struct inode *inode,
				    const char *name, const void *value,
				    size_t size, int flags)
{
	return briefs_xattr_set(inode, xattr_full_name(handler, name),
				value, size, flags);
}

static bool briefs_xattr_security_list(struct dentry *dentry)
{
	return true;
}

static int briefs_xattr_security_get(const struct xattr_handler *handler,
				     struct dentry *unused, struct inode *inode,
				     const char *name, void *buffer, size_t size)
{
	return briefs_xattr_get(inode, xattr_full_name(handler, name),
				buffer, size);
}

static int briefs_xattr_security_set(const struct xattr_handler *handler,
				     struct mnt_idmap *idmap,
				     struct dentry *unused,
				     struct inode *inode, const char *name,
				     const void *value, size_t size, int flags)
{
	return briefs_xattr_set(inode, xattr_full_name(handler, name),
				value, size, flags);
}

const struct xattr_handler briefs_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.list	= briefs_xattr_user_list,
	.get	= briefs_xattr_user_get,
	.set	= briefs_xattr_user_set,
};

const struct xattr_handler briefs_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= briefs_xattr_trusted_list,
	.get	= briefs_xattr_trusted_get,
	.set	= briefs_xattr_trusted_set,
};

const struct xattr_handler briefs_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.list	= briefs_xattr_security_list,
	.get	= briefs_xattr_security_get,
	.set	= briefs_xattr_security_set,
};

const struct xattr_handler * const briefs_xattr_handlers[] = {
	&briefs_xattr_user_handler,
	&briefs_xattr_trusted_handler,
	&briefs_xattr_security_handler,
	NULL,
};
