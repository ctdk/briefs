/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/* Briefs directory operations */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/statfs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/seqlock.h>
#include <linux/pagemap.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

/* briefs_readdir - enumerate directory contents */
int briefs_readdir(struct file *file, struct dir_context *ctx) {
	struct inode *dir = file_inode(file);
	struct trie_iter *iter;

	pr_debug("briefs: readdir offset=%llu (trie)\n", ctx->pos);

	if (!S_ISDIR(dir->i_mode))
		return -ENOTDIR;

	/* Emit . and .. via dir_emit_dots */
	if (ctx->pos < 2) {
		if (!dir_emit_dots(file, ctx))
			return -EIO;
		ctx->pos = 2;
	}

	/*
	 * Get or create the persistent trie iterator.
	 * If ctx->pos is 2 (just past dots), this is the first call or
	 * a seek to 0 — (re)initialize the iterator.
	 * If the iterator doesn't exist (e.g. opened without briefs_dir_open),
	 * allocate one now.
	 */
	iter = file->private_data;
	if (!iter) {
		struct briefs_inode_info *binfo = briefs_i(dir);
		iter = briefs_trie_iter_alloc();
		if (!iter)
			return -ENOMEM;

		mutex_lock(&binfo->trie_lock);
		briefs_trie_iter_init(iter, &binfo->disk_inode, binfo->trie_gen);
		mutex_unlock(&binfo->trie_lock);

		file->private_data = iter;
	} else if (ctx->pos == 2) {
		/* Seek to 0: reinitialize the iterator.
		 * Always reinitialize when ctx->pos is 2 (just past . and ..)
		 * regardless of whether the iterator is exhausted.  The old
		 * check (iter->sp > 0) skipped re-initialization when the
		 * iterator was fully consumed, causing the directory to
		 * appear empty after a seekdir(0) + readdir cycle. */
		struct briefs_inode_info *binfo = briefs_i(dir);

		mutex_lock(&binfo->trie_lock);
		briefs_trie_iter_init(iter, &binfo->disk_inode, binfo->trie_gen);
		mutex_unlock(&binfo->trie_lock);
	}

	/* Iterate the trie from the current iterator position */
	while (1) {
		char entry_name_buf[BRIEFS_NAME_LEN + 1];
		int entry_name_len;
		u64 entry_ino;
		u8 entry_type;
		struct briefs_inode_info *binfo = briefs_i(dir);

		mutex_lock(&binfo->trie_lock);
		if (briefs_trie_iter_next(dir->i_sb, iter, binfo->trie_gen, &entry_ino, &entry_type,
		                         entry_name_buf, &entry_name_len) != 0) {
			mutex_unlock(&binfo->trie_lock);
			break;
		}
		mutex_unlock(&binfo->trie_lock);

		unsigned int file_type = (entry_type << 12) & S_IFMT;

		if (!dir_emit(ctx, entry_name_buf, entry_name_len,
		              entry_ino, fs_umode_to_dtype(file_type))) {
			/* Buffer full - save this entry so briefs_trie_iter_next
			 * returns it on the next call (VFS retries with same ctx->pos
			 * and the trie iterator has already advanced past the entry). */
			iter->pending = true;
			iter->pending_ino = entry_ino;
			iter->pending_type = entry_type;
			iter->pending_name_len = entry_name_len;
			memcpy(iter->pending_name_buf, entry_name_buf, entry_name_len);
			return 0;
		}

		ctx->pos++;
	}

	return 0;
}

/* Open directory — allocate and initialize a persistent trie iterator */
int briefs_dir_open(struct inode *inode, struct file *file) {
	struct briefs_inode_info *binfo;
	struct trie_iter *iter;

	pr_debug("briefs: dir_open inode %lu\n", inode->i_ino);

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	iter = briefs_trie_iter_alloc();
	if (!iter)
		return -ENOMEM;

	binfo = briefs_i(inode);
	mutex_lock(&binfo->trie_lock);
	briefs_trie_iter_init(iter, &binfo->disk_inode, binfo->trie_gen);
	mutex_unlock(&binfo->trie_lock);
	file->private_data = iter;

	return 0;
}

/* Release directory — free the persistent trie iterator */
int briefs_dir_release(struct inode *inode, struct file *file) {
	pr_debug("briefs: dir_release inode %lu\n", inode->i_ino);
	briefs_trie_iter_free(file->private_data);
	file->private_data = NULL;
	return 0;
}

/*
 * briefs_add_dir_entry - insert a directory entry into the directory trie.
 */
int briefs_add_dir_entry(struct inode *dir, const char *name, size_t name_len, u64 child_ino, u8 type) {
	struct briefs_inode_info *binfo;
	int ret;

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);
	mutex_lock(&binfo->trie_lock);

	if (binfo->disk_inode.dir_trie_root == 0) {
		ret = briefs_trie_create_root(dir->i_sb, &binfo->disk_inode);
		if (ret) {
			mutex_unlock(&binfo->trie_lock);
			return ret;
		}
	}

	ret = briefs_trie_insert(dir->i_sb, &binfo->disk_inode,
				  name, name_len, child_ino, type);
	if (ret == -EEXIST) {
		/*
		 * Name already exists in the trie.  Update it in place with the
		 * new inode and file type rather than leaving the old pointer in
		 * place.  This prevents the directory entry from pointing at a
		 * freed/reused inode after an overwrite.
		 */
		ret = briefs_trie_update_entry(dir->i_sb, &binfo->disk_inode,
					       name, name_len, child_ino, type);
		if (ret == -ENOENT) {
			/*
			 * Fallback: the existing entry is a pure leaf that the
			 * update helper can't replace.  Remove it and re-insert.
			 */
			ret = briefs_trie_remove(dir->i_sb, &binfo->disk_inode,
						 name, name_len);
			if (ret == 0) {
				ret = briefs_trie_insert(dir->i_sb, &binfo->disk_inode,
							 name, name_len, child_ino, type);
			}
		}
	}

	if (ret == 0)
		binfo->trie_gen++;
	mutex_unlock(&binfo->trie_lock);
	return ret;
}

/*
 * briefs_remove_dir_entry - remove a directory entry from the trie.
 */
int briefs_remove_dir_entry(struct inode *dir, const char *name, size_t name_len)
{
	struct briefs_inode_info *binfo;
	int ret;

	if (!dir || !name || name_len < 1 || name_len > BRIEFS_NAME_LEN)
		return -EINVAL;

	binfo = briefs_i(dir);
	mutex_lock(&binfo->trie_lock);

	if (binfo->disk_inode.dir_trie_root == 0) {
		mutex_unlock(&binfo->trie_lock);
		return -ENOENT;
	}

	ret = briefs_trie_remove(dir->i_sb, &binfo->disk_inode, name, name_len);
		if (ret == 0) binfo->trie_gen++;
	mutex_unlock(&binfo->trie_lock);
	return ret;
}

/* briefs_lookup - find inode by name in directory trie */
struct dentry *briefs_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags) {
	struct briefs_inode_info *binfo;
	const char *name;
	unsigned int name_len;
	u64 found_ino;
	u8 found_type;
	int ret;

	if (!dir || !S_ISDIR(dir->i_mode))
		return ERR_PTR(-ENOTDIR);

	name = dentry->d_name.name;
	name_len = dentry->d_name.len;

	pr_debug("briefs: trie lookup for %pd in dir inode %lu\n", dentry, dir->i_ino);

	/* Check for . and .. */
	if (name_len == 1 && name[0] == '.') {
		igrab(dir);
		return d_splice_alias(dir, dentry);
	}
	if (name_len == 2 && name[0] == '.' && name[1] == '.') {
		if (dir->i_ino == _BRIEFS_ROOT_INO) {
			igrab(dir);
			return d_splice_alias(dir, dentry);
		}
		binfo = briefs_i(dir);
		if (binfo->disk_inode.parent_inode == 0)
			return ERR_PTR(-EIO);
		{
			struct inode *parent = briefs_iget(dir->i_sb, binfo->disk_inode.parent_inode);
			if (IS_ERR(parent))
				return ERR_CAST(parent);
			return d_splice_alias(parent, dentry);
		}
	}

	/* Search the directory trie */
	binfo = briefs_i(dir);
	mutex_lock(&binfo->trie_lock);
	ret = briefs_trie_lookup(dir->i_sb, &binfo->disk_inode, name, name_len, &found_ino, &found_type);
	mutex_unlock(&binfo->trie_lock);
	if (ret == -ENOENT)
		return NULL;
	if (ret != 0)
		return ERR_PTR(ret);

	{
		struct inode *inode = briefs_iget(dir->i_sb, found_ino);
		if (IS_ERR(inode))
			return ERR_CAST(inode);
		return d_splice_alias(inode, dentry);
	}
}

/* briefs_create - create a new file or directory */
int briefs_create(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode, bool excl) {
	struct inode *inode;
	int ret;
	bool is_dir = S_ISDIR(mode);

	pr_debug("briefs: create %pd (mode=%o) in dir %lu\n", dentry, mode, dir->i_ino);

	inode = briefs_new_inode(dir, dentry, mode, 0);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	/* For directories: create the directory trie root */
	if (is_dir) {
		struct briefs_inode_info *binfo = briefs_i(inode);

		ret = briefs_trie_create_root(dir->i_sb, &binfo->disk_inode);
		if (ret) {
			pr_err("briefs: failed to create dir trie root for ino %lu\n", inode->i_ino);
			briefs_create_abort(dir->i_sb, dir, inode, &dentry->d_name, false);
			return ret;
		}

		pr_debug("briefs: created dir trie root block=%llu for inode %lu\n",
			binfo->disk_inode.dir_trie_root, inode->i_ino);
	}

	ret = briefs_finish_create(dir, dentry, inode, is_dir ? 1 : 0);
	if (ret)
		return ret;

	d_instantiate(dentry, inode);

	pr_debug("briefs: created inode %lu, added to dir\n", inode->i_ino);
	return 0;
}

/* briefs_mkdir - create a new directory */
int briefs_mkdir(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry, umode_t mode) {
	/* Directories at least aren't getting the proper mode set. Let's do
	 * so now, although we may want to split directories and non-directories
	 * into their own functions. */
	mode |= S_IFDIR;
	if (dir->i_mode & S_ISGID)
		mode |= S_ISGID;

	pr_debug("briefs: mkdir %pd (mode=%o)\n", dentry, mode);
	return briefs_create(idmap, dir, dentry, mode, false);
}

/*
 * briefs_link - create a hard link.
 *
 * Creates a new directory entry pointing at the same inode as old_dentry.
 * The source entry remains unchanged — the inode gains an additional link.
 */
int briefs_link(struct dentry *old_dentry, struct inode *dir,
			       struct dentry *new_dentry)
{
	struct inode *inode = d_inode(old_dentry);
	struct briefs_sb_info *bsi = dir->i_sb->s_fs_info;
	u8 ftype;
	int ret;

	pr_debug("briefs: link %pd -> %pd in dir %lu\n", old_dentry, new_dentry, dir->i_ino);

	if (!inode)
		return -ENOENT;

	/* Hard links to directories are not permitted */
	if (S_ISDIR(inode->i_mode))
		return -EPERM;

	/* Add directory entry pointing at the existing inode */
	ftype = (inode->i_mode & S_IFMT) >> 12;
	ret = briefs_add_dir_entry(dir, new_dentry->d_name.name,
					   new_dentry->d_name.len, inode->i_ino, ftype);
	if (ret) {
		pr_err("briefs: link failed to add dir entry: %d\n", ret);
		return ret;
	}

	/* Log new entry creation only after the entry is on disk. */
	ret = briefs_journal_dir_add(bsi->journal, dir->i_ino, inode->i_ino, new_dentry);
	if (ret) {
		pr_err("briefs: link failed to journal dir add: %d\n", ret);
		briefs_remove_dir_entry(dir, new_dentry->d_name.name,
					new_dentry->d_name.len);
		return ret;
	}

	/* Increment nlink on the inode */
	inc_nlink(inode);

	/* Journal the nlink change */
	briefs_journal_inode_update(bsi->journal, inode);

	/* Persist the target inode with updated nlink */
	{
		struct briefs_inode_info *binfo = briefs_i(inode);
		binfo->disk_inode.nlinks = inode->i_nlink;
		ret = briefs_persist_disk_inode(dir->i_sb, inode->i_ino,
								&binfo->disk_inode, false);
		if (ret) {
			pr_err("briefs: link failed to persist target inode: %d\n", ret);
			drop_nlink(inode);
			binfo->disk_inode.nlinks = inode->i_nlink;
			briefs_remove_dir_entry(dir, new_dentry->d_name.name,
							new_dentry->d_name.len);
			briefs_journal_dir_del(bsi->journal, dir->i_ino, new_dentry);
			briefs_persist_disk_inode(dir->i_sb, inode->i_ino,
								  &binfo->disk_inode, false);
			return ret;
		}
	}

	/* Update and persist parent directory */
	ret = briefs_update_parent_dir(dir, bsi,
						BRIEFS_DIR_ENTRY_PREFIX_LEN + new_dentry->d_name.len,
						0);
	if (ret) {
		struct briefs_inode_info *binfo = briefs_i(inode);
		pr_err("briefs: link failed to update parent dir: %d\n", ret);
		drop_nlink(inode);
		binfo->disk_inode.nlinks = inode->i_nlink;
		briefs_remove_dir_entry(dir, new_dentry->d_name.name,
						new_dentry->d_name.len);
		briefs_journal_dir_del(bsi->journal, dir->i_ino, new_dentry);
		briefs_persist_disk_inode(dir->i_sb, inode->i_ino,
							  &binfo->disk_inode, false);
		return ret;
	}

	ihold(inode);
	d_instantiate(new_dentry, inode);

	pr_debug("briefs: link inode %lu now has %u links\n", inode->i_ino, inode->i_nlink);
	return 0;
}


/* briefs_empty_dir - check if a directory is empty by scanning the trie. Since
 * briefs does not store . or .. in the trie (dir_emit_dots handles those), any
 * entry present means that the directory is not empty. */
static int briefs_empty_dir(struct inode *inode, int *ret){
	struct briefs_inode_info *binfo;
	int res;

	binfo = briefs_i(inode);
	mutex_lock(&binfo->trie_lock);
	if (binfo->disk_inode.dir_trie_root != 0) {
		struct trie_iter *iter;
		char *entry_name_buf;
		int entry_name_len;
		u64 entry_ino;
		u8 entry_type;

		iter = briefs_trie_iter_alloc();
		if (!iter) {
			*ret = -ENOMEM;
			mutex_unlock(&binfo->trie_lock);
			return 0;
		}
		entry_name_buf = kmalloc(BRIEFS_NAME_LEN + 1, GFP_KERNEL);
		if (!entry_name_buf) {
			kfree(iter);
			*ret = -ENOMEM;
			mutex_unlock(&binfo->trie_lock);
			return 0;
		}

		briefs_trie_iter_init(iter, &binfo->disk_inode, binfo->trie_gen);
		res = briefs_trie_iter_next(inode->i_sb, iter, binfo->trie_gen,
									   &entry_ino, &entry_type,
									   entry_name_buf, &entry_name_len);

		/* for the love of all that is good, remove this as soon as it
		 * isn't needed. */
		pr_debug("briefs: rmdir found entry '%s'\n", entry_name_buf);

		kfree(entry_name_buf);
		briefs_trie_iter_free(iter);
		if (res == 0) {
			/* Found an entry — directory not empty */
			pr_debug("briefs: rmdir not empty (entry found)\n");
			*ret = -ENOTEMPTY;
			mutex_unlock(&binfo->trie_lock);
			return 0;
		}
	}
	mutex_unlock(&binfo->trie_lock);
	return 1;
}

/* briefs_unlink_common - common unlink/rmdir logic */
static int briefs_unlink_common(struct inode *dir, struct dentry *dentry) {
	struct briefs_sb_info *bsi;
	struct inode *inode = d_inode(dentry);
	int ret;

	pr_debug("briefs: common unlink/rmdir logic %pd\n", dentry);

	bsi = dir->i_sb->s_fs_info;

	/* Log directory entry deletion */
	ret = briefs_journal_dir_del(bsi->journal, dir->i_ino, dentry);
	if (ret) {
		pr_debug("journaling %pd failed\n", dentry);
		return ret;
	}

	/* Remove the directory entry from the parent */
	ret = briefs_remove_dir_entry(dir, dentry->d_name.name, dentry->d_name.len);
	if (ret) {
		pr_debug("removing %pd failed\n", dentry);
		return ret;
	}

	/*
	 * Drop nlink and mark inode dirty. The VFS will eventually evict
	 * this inode (when cache pressure occurs or on unmount), at which
	 * point briefs_evict_inode will free the data blocks and inode number.
	 *
	 * Without this mark_inode_dirty, the VFS might not write back the
	 * nlink=0 to disk or evict the inode promptly.
	 */
	if (S_ISDIR(inode->i_mode)) {
		drop_nlink(dir);
		clear_nlink(inode);
	} else {
		drop_nlink(inode);
		mark_inode_dirty(inode);
	}

	/* TODO: double check that this is *actually* the right thing to do. I
	 * have a sneaking suspicion this was hallucinated or is wrong, but I
	 * could easily be wrong myself. */
	/* Journal the target inode nlink change */
	briefs_journal_inode_update(bsi->journal, inode);

	/* Update parent directory size and persist (nlink was already adjusted) */
	ret = briefs_update_parent_dir(dir, bsi,
					-(ssize_t)(2 + dentry->d_name.len),
					0);
	if (ret) {
		pr_err("briefs: failed to update parent dir after unlink: %d\n", ret);
		return ret;
	}

	pr_debug("briefs: leaving common unlink/rmdir logic %pd\n", dentry);
	return 0;
}

/* briefs_unlink - remove a directory entry */
int briefs_unlink(struct inode *dir, struct dentry *dentry) {
	struct inode *inode = d_inode(dentry);
	int ret;

	pr_debug("briefs: unlink %pd\n", dentry);

	if (!inode)
		return -ENOENT;

	/* POSIX: unlink on a directory must fail with EISDIR */
	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	ret = briefs_unlink_common(dir, dentry);

	pr_debug("briefs: unlinked %pd\n", dentry);

	return ret;
}

/* briefs_rmdir - remove a directory after checking that it's empty. */
int briefs_rmdir(struct inode *dir, struct dentry *dentry) {
	struct inode *inode = d_inode(dentry);
	int ret;

	pr_debug("briefs: rmdir %pd\n", dentry);

	if (!inode)
		return -ENOENT;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	/* Check if the director is empty */
	if (!briefs_empty_dir(inode, &ret)) {
		return ret;
	}

	ret = briefs_unlink_common(dir, dentry);

	pr_debug("briefs: rmdir'd %pd\n", dentry);

	return ret;
}

/* briefs_rename - rename a directory entry */
int briefs_rename(struct mnt_idmap *idmap, struct inode *old_dir, struct dentry *old_dentry,
	                  struct inode *new_dir, struct dentry *new_dentry, unsigned int flags) {
	struct briefs_sb_info *bsi;
	struct inode *inode = d_inode(old_dentry);
	int ret;

	pr_debug("briefs: rename %pd -> %pd\n", old_dentry, new_dentry);

	if (!inode)
		return -ENOENT;

	bsi = old_dir->i_sb->s_fs_info;

	/* Log old entry deletion */
	ret = briefs_journal_dir_del(bsi->journal, old_dir->i_ino, old_dentry);
	if (ret)
		return ret;

	/* Log new entry creation */
	ret = briefs_journal_dir_add(bsi->journal, new_dir->i_ino, inode->i_ino, new_dentry);
	if (ret)
		return ret;

	/* Remove old entry */
	ret = briefs_remove_dir_entry(old_dir, old_dentry->d_name.name, old_dentry->d_name.len);
	if (ret)
		return ret;

	/* Add new entry */
	u8 ftype = (inode->i_mode & S_IFMT) >> 12;
	ret = briefs_add_dir_entry(new_dir, new_dentry->d_name.name, new_dentry->d_name.len,
					   inode->i_ino, ftype);
	if (ret)
		return ret;

	/* Handle cross-directory rename: update nlink and parent_inode */
	if (old_dir != new_dir) {
		if (S_ISDIR(inode->i_mode)) {
			drop_nlink(old_dir);
			briefs_journal_inode_update(bsi->journal, old_dir);
			inc_nlink(new_dir);
			briefs_journal_inode_update(bsi->journal, new_dir);

			/* Update parent_inode in the moved directory's on-disk inode */
			struct briefs_inode_info *moved_binfo = briefs_i(inode);
			moved_binfo->disk_inode.parent_inode = new_dir->i_ino;
			briefs_journal_inode_update(bsi->journal, inode);
			briefs_persist_disk_inode(inode->i_sb, inode->i_ino,
						  &moved_binfo->disk_inode, false);
		}
	}

	/* Persist parent directory nlink changes */
	ret = briefs_update_parent_dir(old_dir, bsi, 0, 0);
	if (ret)
		return ret;

	if (old_dir != new_dir) {
		ret = briefs_update_parent_dir(new_dir, bsi, 0, 0);
		if (ret)
			return ret;
	}

	pr_debug("briefs: renamed %pd -> %pd\n", old_dentry, new_dentry);
	return 0;
}
