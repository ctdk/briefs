// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * BrieFS extended attributes.
 *
 * Each inode may own one external 4096-byte xattr block allocated from the
 * data block pool and referenced by the inode's xattr_offset (absolute block)
 * / xattr_size (bytes used).  The block format (briefs_xattr_header +
 * briefs_xattr_entry[] + name/value bytes + CRC32C at offset 4080) is defined
 * in briefs.h.  Names are stored with their namespace prefix.
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

/* Read and validate an inode's xattr block.  Returns the buffer_head (caller
 * brelse) or ERR_PTR(-ENODATA) if the inode has no xattr block / -EIO on a
 * bad read, magic, or CRC.  Caller holds xattr_sem (read or write).
 */
static struct buffer_head *briefs_xattr_block_read(struct inode *inode)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct buffer_head *bh;
	struct briefs_xattr_header *hdr;
	u64 block;
	__le32 magic;

	block = binfo->disk_inode.xattr_offset;
	if (block == 0)
		return ERR_PTR(-ENODATA);

	bh = sb_bread(inode->i_sb, block);
	if (!bh)
		return ERR_PTR(-EIO);

	hdr = (struct briefs_xattr_header *)bh->b_data;
	magic = le32_to_cpu(hdr->magic);
	if (magic != BRIEFS_XATTR_MAGIC) {
		pr_warn("briefs: xattr block %llu bad magic 0x%08x\n", block, magic);
		brelse(bh);
		return ERR_PTR(-EIO);
	}
	if (le32_to_cpu(hdr->used_size) > BRIEFS_XATTR_MAX_USED ||
	    briefs_verify_chain_checksum(bh->b_data, *xattr_crc_slot(bh->b_data)) != 0) {
		pr_warn("briefs: xattr block %llu bad CRC/size\n", block);
		brelse(bh);
		return ERR_PTR(-EIO);
	}
	return bh;
}

/* Locate an entry by full name.  Returns the entry or NULL. */
static struct briefs_xattr_entry *
briefs_xattr_find(struct buffer_head *bh, const char *name, size_t name_len)
{
	struct briefs_xattr_header *hdr = (struct briefs_xattr_header *)bh->b_data;
	struct briefs_xattr_entry *entries;
	u32 count, i;

	count = le32_to_cpu(hdr->entry_count);
	entries = (struct briefs_xattr_entry *)(bh->b_data + sizeof(*hdr));
	for (i = 0; i < count; i++) {
		struct briefs_xattr_entry *e = &entries[i];
		u32 enlen = le16_to_cpu(e->name_len);

		if (enlen == name_len &&
		    memcmp(bh->b_data + le16_to_cpu(e->name_offset), name, name_len) == 0)
			return e;
	}
	return NULL;
}

/* ---- get / list ------------------------------------------------------- */

int briefs_xattr_get(struct inode *inode, const char *name,
		     void *value, size_t size)
{
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct buffer_head *bh;
	struct briefs_xattr_entry *e;
	size_t name_len;
	int ret;

	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;

	down_read(&binfo->xattr_sem);
	bh = briefs_xattr_block_read(inode);
	if (IS_ERR(bh)) {
		ret = PTR_ERR(bh);
		if (ret == -EIO)
			ret = -EIO;
		else
			ret = -ENODATA;	/* no block */
		goto out;
	}
	e = briefs_xattr_find(bh, name, name_len);
	if (!e) {
		ret = -ENODATA;
		goto out_brelse;
	}
	ret = le16_to_cpu(e->value_len);
	if (value) {
		if (size < (size_t)ret)
			ret = -ERANGE;
		else if (ret > 0)
			memcpy(value, bh->b_data + le16_to_cpu(e->value_offset), ret);
	}
out_brelse:
	brelse(bh);
out:
	up_read(&binfo->xattr_sem);
	return ret;
}

ssize_t briefs_xattr_list(struct dentry *dentry, char *buffer, size_t size)
{
	struct inode *inode = d_inode(dentry);
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct buffer_head *bh;
	struct briefs_xattr_header *hdr;
	struct briefs_xattr_entry *entries;
	size_t rest = size;
	u32 count, i;
	ssize_t ret;

	down_read(&binfo->xattr_sem);
	bh = briefs_xattr_block_read(inode);
	if (IS_ERR(bh)) {
		ret = PTR_ERR(bh);
		ret = (ret == -EIO) ? -EIO : 0;	/* no block -> empty list */
		goto out;
	}
	hdr = (struct briefs_xattr_header *)bh->b_data;
	count = le32_to_cpu(hdr->entry_count);
	entries = (struct briefs_xattr_entry *)(bh->b_data + sizeof(*hdr));

	for (i = 0; i < count; i++) {
		struct briefs_xattr_entry *e = &entries[i];
		u32 nlen = le16_to_cpu(e->name_len);
		size_t need = nlen + 1;	/* name + NUL */

		if (buffer) {
			if (need > rest) {
				ret = -ERANGE;
				goto out_brelse;
			}
			memcpy(buffer, bh->b_data + le16_to_cpu(e->name_offset), nlen);
			buffer += nlen;
			*buffer++ = '\0';
		}
		rest -= need;
	}
	ret = size - rest;
out_brelse:
	brelse(bh);
out:
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

/*
 * Clear xattr_offset/size, persist + journal the cleared inode, then free the
 * block and journal the free.  Caller holds xattr_sem write and has already
 * updated i_ctime if appropriate.  No-op if there is no block.
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
	 * Publish the cleared pointer before freeing the block, so replay never
	 * sees an inode pointing at a freed block (mirrors the dir-trie free
	 * ordering in briefs_free_inode_data).
	 */
	binfo->disk_inode.xattr_offset = 0;
	binfo->disk_inode.xattr_size = 0;
	briefs_sync_inode_times(inode, &binfo->disk_inode);
	briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
	briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
				  &binfo->disk_inode, false);
	if (bsi->journal && !bsi->journal->in_replay)
		briefs_journal_inode_full(bsi->journal, inode->i_ino, &disk_di);

	briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, old_block));
	if (bsi->journal && !bsi->journal->in_replay)
		briefs_journal_extent_free(bsi->journal, inode->i_ino, 0,
					   old_block, 1);
}

int briefs_xattr_set(struct inode *inode, const char *name,
		     const void *value, size_t size, int flags)
{
	struct briefs_sb_info *bsi = inode->i_sb->s_fs_info;
	struct briefs_inode_info *binfo = briefs_i(inode);
	struct buffer_head *bh_old = NULL;
	struct briefs_xattr_header *hdr;
	struct briefs_xattr_entry *entries;
	struct xattr_kv *kvs = NULL;
	u32 n = 0, capacity = 0;
	size_t name_len;
	bool removing = !value;
	bool found = false;
	bool dirtied = false;
	int ret = 0, i;

	if (!name)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > XATTR_NAME_MAX)
		return -ERANGE;
	if (size > XATTR_SIZE_MAX)
		return -E2BIG;

	down_write(&binfo->xattr_sem);

	/* Collect the existing entries into owned memory. */
	if (binfo->disk_inode.xattr_offset != 0) {
		bh_old = briefs_xattr_block_read(inode);
		if (IS_ERR(bh_old)) {
			ret = PTR_ERR(bh_old);
			bh_old = NULL;
			if (ret != -ENODATA) {
				ret = -EIO;
				goto out;
			}
			/* stale pointer / no readable block: treat as empty */
		}
	}
	if (bh_old) {
		hdr = (struct briefs_xattr_header *)bh_old->b_data;
		n = le32_to_cpu(hdr->entry_count);
		capacity = n + 1;
		kvs = kmalloc_array(capacity, sizeof(*kvs), GFP_KERNEL);
		if (!kvs) {
			ret = -ENOMEM;
			goto out_free_kvs;
		}
		entries = (struct briefs_xattr_entry *)(bh_old->b_data + sizeof(*hdr));
		for (i = 0; i < (int)n; i++) {
			struct briefs_xattr_entry *e = &entries[i];
			u32 nl = le16_to_cpu(e->name_len);
			u32 vl = le16_to_cpu(e->value_len);

			kvs[i].name_len = nl;
			kvs[i].name = kmalloc(nl, GFP_KERNEL);
			kvs[i].value_len = vl;
			kvs[i].value = vl ? kmalloc(vl, GFP_KERNEL) : NULL;
			if (!kvs[i].name || (vl && !kvs[i].value)) {
				xattr_kv_free(kvs, i + 1);
				kvs = NULL;
				ret = -ENOMEM;
				goto out_free_kvs;
			}
			memcpy(kvs[i].name, bh_old->b_data + le16_to_cpu(e->name_offset), nl);
			if (vl)
				memcpy(kvs[i].value,
				       bh_old->b_data + le16_to_cpu(e->value_offset), vl);
		}
	}

	/* Locate the target entry. */
	for (i = 0; i < (int)n; i++) {
		if (kvs[i].name_len == name_len &&
		    memcmp(kvs[i].name, name, name_len) == 0) {
			found = true;
			break;
		}
	}

	if (removing) {
		if (!found) {
			ret = -ENODATA;	/* XATTR_REPLACE set by removexattr */
			goto out_free_kvs;
		}
		/* Drop the entry. */
		kfree(kvs[i].name);
		kfree(kvs[i].value);
		kvs[i] = kvs[n - 1];
		n--;
	} else {
		if (found) {
			void *nv;

			if (flags & XATTR_CREATE) {
				ret = -EEXIST;
				goto out_free_kvs;
			}
			nv = kmalloc(size, GFP_KERNEL);
			if (!nv) {
				ret = -ENOMEM;
				goto out_free_kvs;
			}
			memcpy(nv, value, size);
			kfree(kvs[i].value);
			kvs[i].value = nv;
			kvs[i].value_len = size;
		} else {
			if (flags & XATTR_REPLACE) {
				ret = -ENODATA;
				goto out_free_kvs;
			}
			if (n + 1 > capacity) {
				/* Should not happen (capacity == n+1), but be safe. */
				struct xattr_kv *grown =
					krealloc(kvs, (n + 1) * sizeof(*kvs),
						 GFP_KERNEL);
				if (!grown) {
					ret = -ENOMEM;
					goto out_free_kvs;
				}
				kvs = grown;
			}
			kvs[n].name_len = name_len;
			kvs[n].name = kmalloc(name_len, GFP_KERNEL);
			kvs[n].value_len = size;
			kvs[n].value = size ? kmalloc(size, GFP_KERNEL) : NULL;
			if (!kvs[n].name || (size && !kvs[n].value)) {
				kfree(kvs[n].name);
				kfree(kvs[n].value);
				ret = -ENOMEM;
				goto out_free_kvs;
			}
			memcpy(kvs[n].name, name, name_len);
			if (size)
				memcpy(kvs[n].value, value, size);
			n++;
		}
	}

	if (bh_old) {
		brelse(bh_old);
		bh_old = NULL;
	}

	if (n == 0) {
		/* Last entry removed: free the block. */
		inode_set_ctime_to_ts(inode, current_time(inode));
		briefs_xattr_free_block_locked(inode);
		dirtied = true;
		goto out_free_kvs;
	}

	/* Serialize the entry set into a fresh block buffer. */
	{
		u32 single_footprint = sizeof(struct briefs_xattr_header) +
			sizeof(struct briefs_xattr_entry) +
			ALIGN(name_len, 4) + ALIGN(size, 4);
		u32 off, used, j;
		struct buffer_head *bh;
		u64 block, rel;
		bool newly_allocated = false;
		struct briefs_disk_inode disk_di;

		/*
		 * A single entry that cannot fit alone is ERANGE; a set that
		 * does not fit is ENOSPC.
		 */
		used = sizeof(struct briefs_xattr_header) +
		       n * sizeof(struct briefs_xattr_entry);
		for (j = 0; j < n; j++)
			used += ALIGN(kvs[j].name_len, 4) + ALIGN(kvs[j].value_len, 4);
		if (!removing && single_footprint > BRIEFS_XATTR_MAX_USED) {
			ret = -ERANGE;
			goto out_free_kvs;
		}
		if (used > BRIEFS_XATTR_MAX_USED) {
			ret = -ENOSPC;
			goto out_free_kvs;
		}

		/* Get a writable buffer for the block. */
		if (binfo->disk_inode.xattr_offset == 0) {
			rel = briefs_alloc_block(&bsi->alloc);
			if (rel == 0) {
				ret = -ENOSPC;
				goto out_free_kvs;
			}
			block = data_to_abs(bsi->sb, rel);
			bh = briefs_get_zero_block(inode->i_sb, block);
			if (!bh) {
				briefs_free_block(&bsi->alloc, rel);
				ret = -EIO;
				goto out_free_kvs;
			}
			newly_allocated = true;
		} else {
			block = binfo->disk_inode.xattr_offset;
			bh = sb_bread(inode->i_sb, block);
			if (!bh) {
				ret = -EIO;
				goto out_free_kvs;
			}
		}

		/*
		 * Build the block: header + entry array + name/value bytes, then
		 * zero the tail and write the CRC at offset 4080.
		 */
		hdr = (struct briefs_xattr_header *)bh->b_data;
		hdr->magic = cpu_to_le32(BRIEFS_XATTR_MAGIC);
		hdr->version = cpu_to_le32(BRIEFS_XATTR_VERSION);
		hdr->used_size = cpu_to_le32(used);
		hdr->entry_count = cpu_to_le32(n);
		entries = (struct briefs_xattr_entry *)(bh->b_data + sizeof(*hdr));
		off = sizeof(struct briefs_xattr_header) +
		      n * sizeof(struct briefs_xattr_entry);
		for (j = 0; j < n; j++) {
			entries[j].name_len = cpu_to_le16(kvs[j].name_len);
			entries[j].value_len = cpu_to_le16(kvs[j].value_len);
			entries[j].name_offset = cpu_to_le16(off);
			memcpy(bh->b_data + off, kvs[j].name, kvs[j].name_len);
			off += ALIGN(kvs[j].name_len, 4);
			if (kvs[j].value_len) {
				entries[j].value_offset = cpu_to_le16(off);
				memcpy(bh->b_data + off, kvs[j].value,
				       kvs[j].value_len);
				off += ALIGN(kvs[j].value_len, 4);
			} else {
				entries[j].value_offset = 0;
			}
		}
		/* Zero [used, 4096) so the tail (incl. old CRC) is clean. */
		memset(bh->b_data + used, 0, BRIEFS_BLOCK_SIZE - used);
		*xattr_crc_slot(bh->b_data) =
			cpu_to_le64(briefs_chain_checksum(bh->b_data));
		mark_buffer_dirty(bh);

		/*
		 * Belt-and-suspenders: make the block content durable before
		 * publishing the pointer (mirrors briefs_trie_page_init).
		 */
		sync_dirty_buffer(bh);
		if (briefs_check_meta_write_error(bh)) {
			briefs_handle_meta_write_error(inode->i_sb, "xattr block sync");
			if (newly_allocated)
				briefs_free_block(&bsi->alloc, rel);
			brelse(bh);
			ret = -EIO;
			goto out_free_kvs;
		}

		/* Publish the new xattr_offset/size and bump ctime. */
		binfo->disk_inode.xattr_offset = block;
		binfo->disk_inode.xattr_size = used;
		inode_set_ctime_to_ts(inode, current_time(inode));
		briefs_sync_inode_times(inode, &binfo->disk_inode);

		/*
		 * Journal the block content BEFORE the inode pointer so replay
		 * restores the block before the inode references it.
		 */
		if (bsi->journal && !bsi->journal->in_replay)
			briefs_journal_xattr_data(bsi->journal, inode->i_ino,
						  block, used, bh->b_data);

		briefs_cpu_inode_to_disk(&binfo->disk_inode, &disk_di);
		briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
					  &binfo->disk_inode, false);
		if (bsi->journal && !bsi->journal->in_replay)
			briefs_journal_inode_full(bsi->journal, inode->i_ino,
						  &disk_di);

		brelse(bh);
		dirtied = true;
	}

out_free_kvs:
	if (kvs)
		xattr_kv_free(kvs, n);
	if (dirtied)
		mark_inode_dirty(inode);
out:
	if (bh_old)
		brelse(bh_old);
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
