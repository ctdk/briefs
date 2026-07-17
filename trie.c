/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * BrieFS directory trie.
 *
 * Starting with BrieFS 0.7.0, trie nodes are packed into 4 KiB pages.
 * The directory inode stores a 64-bit "node reference" in dir_trie_root:
 *   ref = (absolute_block << TRIE_SLOT_BITS) | slot_index
 * with 0 meaning null.  Node links (first_child / next_sibling) are also
 * node references.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>
#include "briefs.h"
#include "briefs_alloc.h"

#define TRIE_ANCESTRY_LIMIT 256

/* Forward decl: briefs_trie_seed_pool() (below) uses the iterator stack before
 * trie_iter_push() is defined later in this file.
 */
static int trie_iter_push(struct trie_iter *iter, u64 ref, u8 emitted);

/*
 * Name pointer for a node slot.  The 2-byte length prefix lives at (base - 2);
 * the name bytes follow it.
 */
static inline char *trie_node_name(void *page_base, struct briefs_trie_node *node)
{
	return (char *)TRIE_NODE_NAME_BASE(page_base, node);
}

/*
 * Read a node by reference.  Returns buffer head, or ERR_PTR on error.
 * Caller must brelse(*bh).
 */
static int trie_read_node(struct super_block *sb, u64 ref,
                          struct buffer_head **bh,
                          struct briefs_trie_page **page,
                          struct briefs_trie_node **node)
{
	*bh = briefs_trie_read_page(sb, ref, page, node);
	if (IS_ERR(*bh)) {
		int err = PTR_ERR(*bh);
		*bh = NULL;
		return err;
	}
	return 0;
}

/*
 * Get a writable node by reference.
 */
static int trie_get_node(struct super_block *sb, u64 ref,
                         struct buffer_head **bh,
                         struct briefs_trie_page **page,
                         struct briefs_trie_node **node)
{
	*bh = briefs_trie_get_page(sb, ref, page, node);
	if (IS_ERR(*bh)) {
		int err = PTR_ERR(*bh);
		*bh = NULL;
		return err;
	}
	return 0;
}

/*
 * Find a child node by byte value.  On success, fills *child_ref with the
 * matching child reference and *prev_ref with the previous sibling (0 if the
 * child is the first child).  Returns 0 on success, -ENOENT if not found.
 */
static int trie_find_child_with_prev(struct super_block *sb, u64 parent_ref,
                                      u8 byte_val, u64 *child_ref,
                                      u64 *prev_ref)
{
	struct buffer_head *pbh, *cbh;
	struct briefs_trie_page *ppage, *cpage;
	struct briefs_trie_node *pnode, *cnode;
	u64 prev = 0;
	u64 child;

	if (trie_read_node(sb, parent_ref, &pbh, &ppage, &pnode) != 0)
		return -EIO;

	child = trie_node_first_child(pnode);
	while (!TRIE_REF_IS_NULL(child)) {
		cbh = briefs_trie_read_page(sb, child, &cpage, &cnode);
		if (IS_ERR(cbh))
			break;
		if (cnode->byte_val == byte_val) {
			*child_ref = child;
			*prev_ref = prev;
			brelse(cbh);
			brelse(pbh);
			return 0;
		}
		prev = child;
		child = trie_node_next_sibling(cnode);
		brelse(cbh);
	}

	brelse(pbh);
	return -ENOENT;
}

/*
 * Find a child node by byte value.  Returns the child node reference, or 0
 * if not found.
 */
static u64 briefs_trie_find_child(struct super_block *sb, u64 parent_ref,
                                   u8 byte_val)
{
	u64 child, prev;
	if (trie_find_child_with_prev(sb, parent_ref, byte_val, &child, &prev) == 0)
		return child;
	return 0;
}

/*
 * Link a child node into the parent's sibling chain.
 */
static int trie_link_child(struct super_block *sb, u64 parent_ref, u64 child_ref)
{
	struct buffer_head *pbh, *lbh;
	struct briefs_trie_page *ppage, *lpage;
	struct briefs_trie_node *pnode, *last_node;
	u64 last;

	if (trie_get_node(sb, parent_ref, &pbh, &ppage, &pnode) != 0)
		return -EIO;

	if (TRIE_REF_IS_NULL(trie_node_first_child(pnode))) {
		trie_node_set_first_child(pnode, child_ref);
		trie_node_set_child_count(pnode, trie_node_child_count(pnode) + 1);
		mark_buffer_dirty(pbh);
		brelse(pbh);
		return 0;
	}

	/* Walk to the last sibling. */
	last = trie_node_first_child(pnode);
	lbh = briefs_trie_get_page(sb, last, &lpage, &last_node);
	if (IS_ERR(lbh)) {
		brelse(pbh);
		return -EIO;
	}

	while (!TRIE_REF_IS_NULL(trie_node_next_sibling(last_node))) {
		u64 next = trie_node_next_sibling(last_node);
		brelse(lbh);
		lbh = briefs_trie_get_page(sb, next, &lpage, &last_node);
		if (IS_ERR(lbh)) {
			brelse(pbh);
			return -EIO;
		}
		last = next;
	}

	trie_node_set_next_sibling(last_node, child_ref);
	mark_buffer_dirty(lbh);
	brelse(lbh);

	trie_node_set_child_count(pnode, trie_node_child_count(pnode) + 1);
	mark_buffer_dirty(pbh);
	brelse(pbh);
	return 0;
}

/*
 * Create a child node of the given parent.  Returns child ref, or 0 on failure.
 */
static u64 briefs_trie_create_child(struct super_block *sb, u64 parent_ref,
                                     u8 depth, u8 byte_val, u8 node_type,
                                     size_t name_len)
{
	u64 child_ref;
	struct buffer_head *cbh;
	struct briefs_trie_page *cpage;
	struct briefs_trie_node *cnode;

	child_ref = briefs_trie_alloc_node(sb, name_len);
	if (TRIE_REF_IS_NULL(child_ref))
		return 0;

	cbh = briefs_trie_get_page(sb, child_ref, &cpage, &cnode);
	if (IS_ERR(cbh)) {
		briefs_trie_free_node(sb, child_ref);
		return 0;
	}
	cnode->depth = depth;
	cnode->byte_val = byte_val;
	cnode->node_type = node_type;
	mark_buffer_dirty(cbh);
	brelse(cbh);

	if (trie_link_child(sb, parent_ref, child_ref) != 0) {
		briefs_trie_free_node(sb, child_ref);
		return 0;
	}

	return child_ref;
}

/*
 * Find or create a child by byte_val.
 */
static u64 briefs_trie_find_or_create_child(struct super_block *sb, u64 parent_ref,
                                             u8 depth, u8 byte_val, u8 node_type)
{
	u64 child;

	child = briefs_trie_find_child(sb, parent_ref, byte_val);
	if (!TRIE_REF_IS_NULL(child))
		return child;
	return briefs_trie_create_child(sb, parent_ref, depth, byte_val, node_type, 0);
}

/*
 * Store a name into a node's slot, allocating name-heap space if needed.
 */
static int trie_store_name(struct super_block *sb, u64 ref,
                           const char *name, int name_len)
{
	return briefs_trie_node_store_name(sb, ref, name, name_len);
}

/*
 * briefs_trie_create_root - create the root node of a directory trie.
 */
int briefs_trie_create_root(struct super_block *sb, struct briefs_inode *di)
{
	u64 root_ref;
	int ret;

	ret = briefs_trie_page_init(sb, 0, 0, NODE_TYPE_INTERM, &root_ref);
	if (ret)
		return ret;

	/* Add the root page to the partial list immediately so future
	 * allocations can reuse its free slots.
	 */
	briefs_trie_page_add_partial(sb, TRIE_REF_BLOCK(root_ref));
	di->dir_trie_root = root_ref;
	return 0;
}

/*
 * briefs_trie_seed_pool - populate the partial-page pool from the on-disk trie.
 *
 * During journal replay, replay_dir_update() re-derives a directory trie by
 * re-running briefs_trie_insert() on a freshly briefs_iget()'d parent.  The
 * per-superblock partial-page pool is normally built lazily by live trie
 * mutations (create_root / fresh page_init / free), but at replay it starts
 * EMPTY: briefs_iget() copies dir_trie_root from disk but never scans the trie
 * pages for free slots.  With an empty pool, replay's first
 * briefs_trie_alloc_node() per insert takes the fresh-alloc branch (page_init
 * -> briefs_alloc_block) where the live path reused a free slot on an existing
 * page, so replay over-allocates trie pages and -ENOSPC's on a full fs
 * (generic/475).
 *
 * Walk every page reachable from @root_ref and add each page that still has
 * room for another node to the partial pool, reproducing the pool state live
 * would have had at this trie.  briefs_trie_page_add_partial() dedups by block,
 * so visiting multiple nodes of the same page is harmless.  Called once per
 * directory (gated by binfo->trie_pool_seeded) and only while the journal's
 * in_replay flag is set, so the live post-mount path is unaffected.
 */
int briefs_trie_seed_pool(struct super_block *sb, u64 root_ref)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct trie_iter *iter;
	int ret = 0;
	u64 visited = 0, added = 0;
	/*
	 * A valid trie has at most block_count trie pages (one per data block),
	 * each holding TRIE_SLOTS_PER_BLOCK nodes, so it can never contain more
	 * than block_count * TRIE_SLOTS_PER_BLOCK reachable nodes.  Use that as a
	 * hard visit cap (plus a small margin): it never truncates a legitimate
	 * trie, but it guarantees termination if the on-disk trie is corrupt with a
	 * cycle in the first_child/next_sibling graph (dm-error partial sync can
	 * leave stale pointers forming a back-edge), which would otherwise spin
	 * here forever and wedge the mount.  Hitting the cap aborts seeding; re-
	 * derivation then falls back to briefs_alloc_block() (today's behaviour),
	 * which may -ENOSPC but never hangs.
	 */
	u64 cap = bsi->alloc.block_count * TRIE_SLOTS_PER_BLOCK + 1024;

	if (TRIE_REF_IS_NULL(root_ref))
		return 0;

	iter = briefs_trie_iter_alloc();
	if (!iter)
		return -ENOMEM;

	ret = trie_iter_push(iter, root_ref, 0);
	if (ret)
		goto out;

	while (iter->sp > 0) {
		struct buffer_head *bh;
		struct briefs_trie_page *page;
		struct briefs_trie_node *node;
		u64 ref, child, sib;

		if (visited >= cap) {
			pr_warn("briefs: replay pool seed: visited cap %llu hit (corrupt trie? root=%llu); aborting seed\n",
				cap, (unsigned long long)root_ref);
			break;
		}

		ref = iter->stack[--iter->sp];

		if (trie_read_node(sb, ref, &bh, &page, &node) != 0) {
			/* Unreadable page during replay: skip it, keep seeding the
			 * rest.  A later re-derivation step will surface the error.
			 */
			continue;
		}
		visited++;

		if (briefs_trie_page_has_room(page)) {
			briefs_trie_page_add_partial(sb, TRIE_REF_BLOCK(ref));
			added++;
		}

		child = trie_node_first_child(node);
		sib = trie_node_next_sibling(node);
		brelse(bh);

		if (!TRIE_REF_IS_NULL(child)) {
			ret = trie_iter_push(iter, child, 0);
			if (ret)
				goto out;
		}
		if (!TRIE_REF_IS_NULL(sib)) {
			ret = trie_iter_push(iter, sib, 0);
			if (ret)
				goto out;
		}

		if ((visited & 4095) == 0)
			cond_resched();
	}

	pr_debug("briefs: replay seed_pool root=%llu: visited=%llu added=%llu\n",
		 (unsigned long long)root_ref, visited, added);

out:
	briefs_trie_iter_free(iter);
	return ret;
}

/*
 * briefs_trie_lookup - find an entry by name in the directory trie.
 */
int briefs_trie_lookup(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len,
                        u64 *found_ino, u8 *found_type)
{
	struct buffer_head *cbh;
	struct briefs_trie_page *cpage;
	struct briefs_trie_node *cnode;
	u64 cur, child;
	int pos;

	if (TRIE_REF_IS_NULL(di->dir_trie_root))
		return -ENOENT;

	cur = di->dir_trie_root;

	for (pos = 0; pos < name_len; pos++) {
		u8 bval = (u8)name[pos];

		child = briefs_trie_find_child(sb, cur, bval);
		if (TRIE_REF_IS_NULL(child))
			return -ENOENT;

		if (trie_read_node(sb, child, &cbh, &cpage, &cnode) != 0)
			return -EIO;

		if (pos == name_len - 1) {
			if (TRIE_IS_LEAF(cnode)) {
				char *ename = trie_node_name(cbh->b_data, cnode);
				int elen = trie_node_name_len(cnode) - 2;

				if (elen == name_len && memcmp(ename, name, name_len) == 0) {
					*found_ino = trie_node_inode(cnode);
					*found_type = TRIE_NODE_FTYPE(cnode);
					brelse(cbh);
					return 0;
				}
			}
			brelse(cbh);
			return -ENOENT;
		}

		if (!(cnode->node_type & NODE_TYPE_INTERM)) {
			/* Pure leaf where we need an INTERM.  Check full name. */
			char *ename = trie_node_name(cbh->b_data, cnode);
			int elen = trie_node_name_len(cnode) - 2;

			brelse(cbh);
			if (elen == name_len && memcmp(ename, name, name_len) == 0) {
				*found_ino = trie_node_inode(cnode);
				*found_type = TRIE_NODE_FTYPE(cnode);
				return 0;
			}
			return -ENOENT;
		}

		brelse(cbh);
		cur = child;
	}

	return -ENOENT;
}

/*
 * Unlink a child from its parent's sibling chain.  child_prev == 0 means the
 * child is the first child.
 */
static void trie_unlink_child(struct super_block *sb, u64 parent_ref,
                              u64 child_prev, u64 child_ref)
{
	struct buffer_head *pbh, *chbh;
	struct briefs_trie_page *ppage, *chpage;
	struct briefs_trie_node *pnode, *chnode;
	u64 next;

	if (trie_get_node(sb, parent_ref, &pbh, &ppage, &pnode) != 0)
		return;

	if (trie_read_node(sb, child_ref, &chbh, &chpage, &chnode) != 0) {
		brelse(pbh);
		return;
	}
	next = trie_node_next_sibling(chnode);
	brelse(chbh);

	if (TRIE_REF_IS_NULL(child_prev)) {
		trie_node_set_first_child(pnode, next);
	} else {
		struct buffer_head *prev_bh;
		struct briefs_trie_page *prev_page;
		struct briefs_trie_node *prev_node;

		if (trie_get_node(sb, child_prev, &prev_bh, &prev_page, &prev_node) == 0) {
			trie_node_set_next_sibling(prev_node, next);
			mark_buffer_dirty(prev_bh);
			brelse(prev_bh);
		}
	}

	trie_node_set_child_count(pnode, trie_node_child_count(pnode) - 1);
	mark_buffer_dirty(pbh);
	brelse(pbh);
}

/*
 * Split a pure leaf at the given position.  The leaf currently hangs off
 * parent `cur` at byte value `bval` (the new name's byte at `pos`).  On
 * return, *cur_out holds the node reference to continue descending from.
 */
static int trie_split_leaf(struct super_block *sb, u64 cur, u64 child,
                           int pos, u8 bval, const char *name, int name_len,
                           u64 ino, u8 type, u64 *cur_out)
{
	struct buffer_head *lbh, *ibh, *gbh;
	struct briefs_trie_page *lpage, *ipage, *gpage;
	struct briefs_trie_node *lnode, *inode, *gnode;
	int old_name_len;
	u64 internal, old_sibling;

	if (trie_read_node(sb, child, &lbh, &lpage, &lnode) != 0)
		return -EIO;
	old_name_len = trie_node_name_len(lnode) - 2;

	if (old_name_len == pos + 1) {
		/* Old leaf is a prefix of the new name. */
		lnode->node_type = NODE_TYPE_INTERM | NODE_STATUS_LEAF;
		lnode->depth = pos + 1;
		mark_buffer_dirty(lbh);
		brelse(lbh);
		*cur_out = child;
		return 0;
	}

	old_sibling = trie_node_next_sibling(lnode);
	brelse(lbh);

	internal = briefs_trie_create_child(sb, cur, pos + 1, bval,
	                                     NODE_TYPE_INTERM, 0);
	if (TRIE_REF_IS_NULL(internal))
		return -ENOSPC;

	/* Reparent: find link from cur to child, redirect to internal. */
	if (trie_get_node(sb, cur, &gbh, &gpage, &gnode) != 0)
		return -EIO;
	if (trie_node_first_child(gnode) == child) {
		trie_node_set_first_child(gnode, internal);
	} else {
		u64 prev = trie_node_first_child(gnode);
		while (!TRIE_REF_IS_NULL(prev)) {
			struct buffer_head *tbh;
			struct briefs_trie_page *tpage;
			struct briefs_trie_node *tmp;
			if (trie_get_node(sb, prev, &tbh, &tpage, &tmp) != 0)
				break;
			if (trie_node_next_sibling(tmp) == child) {
				trie_node_set_next_sibling(tmp, internal);
				mark_buffer_dirty(tbh);
				brelse(tbh);
				break;
			}
			prev = trie_node_next_sibling(tmp);
			brelse(tbh);
		}
	}
	mark_buffer_dirty(gbh);
	brelse(gbh);

	/* Link old leaf as child of internal. */
	if (trie_get_node(sb, internal, &ibh, &ipage, &inode) == 0) {
		trie_node_set_first_child(inode, child);
		trie_node_set_next_sibling(inode, old_sibling);
		trie_node_set_child_count(inode, 1);
		mark_buffer_dirty(ibh);
		brelse(ibh);
	}

	/* If split at last byte, store new name on internal. */
	if (pos == name_len - 1) {
		if (trie_get_node(sb, internal, &ibh, &ipage, &inode) == 0) {
			inode->node_type |= NODE_STATUS_LEAF;
			TRIE_SET_FTYPE(inode, type);
			trie_node_set_inode(inode, ino);
			trie_store_name(sb, internal, name, name_len);
			mark_buffer_dirty(ibh);
			brelse(ibh);
		}
	}

	*cur_out = internal;
	return 0;
}

/*
 * briefs_trie_insert - insert a name/inode entry into the directory trie.
 */
int briefs_trie_insert(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len,
                        u64 ino, u8 type)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_page *page, *cpage;
	struct briefs_trie_node *node, *cnode;
	u64 cur, child, new_leaf, existing;
	int pos, ret;

	if (TRIE_REF_IS_NULL(di->dir_trie_root)) {
		ret = briefs_trie_create_root(sb, di);
		if (ret)
			return ret;
	}

	cur = di->dir_trie_root;

	if (name_len > TRIE_MAX_NAME_LEN)
		return -ENAMETOOLONG;

	for (pos = 0; pos < name_len; pos++) {
		u8 bval = (u8)name[pos];

		if (pos == name_len - 1) {
			existing = briefs_trie_find_child(sb, cur, bval);

			if (!TRIE_REF_IS_NULL(existing)) {
				if (trie_get_node(sb, existing, &cbh, &cpage, &cnode) != 0)
					return -EIO;

				if (cnode->node_type & NODE_TYPE_INTERM) {
					if (cnode->node_type & NODE_STATUS_LEAF) {
						char *ename = trie_node_name(cbh->b_data, cnode);
						int elen = trie_node_name_len(cnode) - 2;
						if (elen == name_len &&
						    memcmp(ename, name, name_len) == 0) {
							brelse(cbh);
							return -EEXIST;
						}
					}

					cnode->node_type |= NODE_STATUS_LEAF;
					TRIE_SET_FTYPE(cnode, type);
					trie_node_set_inode(cnode, ino);
					trie_store_name(sb, existing, name, name_len);
					mark_buffer_dirty(cbh);
					brelse(cbh);
					return 0;
				}

				/* Existing pure leaf.  Check duplicate then split. */
				{
					char *ename = trie_node_name(cbh->b_data, cnode);
					int elen = trie_node_name_len(cnode) - 2;
					if (elen == name_len &&
					    memcmp(ename, name, name_len) == 0) {
						brelse(cbh);
						return -EEXIST;
					}
				}
				brelse(cbh);

				ret = trie_split_leaf(sb, cur, existing, pos, bval,
				                     name, name_len, ino, type, &cur);
				if (ret != 0)
					return ret;
				continue;
			}

			/* No existing child: create a new pure leaf. */
			new_leaf = briefs_trie_create_child(sb, cur, pos, bval, 0,
			                                   name_len);
			if (TRIE_REF_IS_NULL(new_leaf))
				return -ENOSPC;

			if (trie_get_node(sb, new_leaf, &bh, &page, &node) != 0) {
				briefs_trie_free_node(sb, new_leaf);
				return -EIO;
			}
			node->node_type = 0;
			TRIE_SET_FTYPE(node, type);
			trie_node_set_inode(node, ino);
			trie_store_name(sb, new_leaf, name, name_len);
			mark_buffer_dirty(bh);
			brelse(bh);
			return 0;
		}

		/* Middle byte: find or create an INTERM child. */
		child = briefs_trie_find_or_create_child(sb, cur, pos + 1, bval,
		                                         NODE_TYPE_INTERM);
		if (TRIE_REF_IS_NULL(child))
			return -ENOSPC;

		if (trie_read_node(sb, child, &cbh, &cpage, &cnode) != 0)
			return -EIO;
		if (!(cnode->node_type & NODE_TYPE_INTERM)) {
			brelse(cbh);
			ret = trie_split_leaf(sb, cur, child, pos, bval,
			                     name, name_len, ino, type, &cur);
			if (ret != 0)
				return ret;
			continue;
		}
		brelse(cbh);
		cur = child;
	}

	return -EINVAL;
}

/*
 * briefs_trie_update_entry - update the inode and file type of an existing
 * directory entry in the trie.  Used when a create/symlink/mknod operation
 * replaces an existing name without first removing it.
 *
 * Returns 0 on success, -ENOENT if the name is not in the trie.
 */
int briefs_trie_update_entry(struct super_block *sb, struct briefs_inode *di,
                             const char *name, size_t name_len,
                             u64 new_ino, u8 new_type)
{
	struct buffer_head *cbh;
	struct briefs_trie_page *cpage;
	struct briefs_trie_node *cnode;
	u64 cur, child;
	int pos;

	if (TRIE_REF_IS_NULL(di->dir_trie_root))
		return -ENOENT;

	cur = di->dir_trie_root;

	for (pos = 0; pos < name_len; pos++) {
		u8 bval = (u8)name[pos];

		child = briefs_trie_find_child(sb, cur, bval);
		if (TRIE_REF_IS_NULL(child))
			return -ENOENT;

		if (trie_get_node(sb, child, &cbh, &cpage, &cnode) != 0)
			return -EIO;

		if (pos == name_len - 1) {
			if (TRIE_IS_LEAF(cnode)) {
				char *ename = trie_node_name(cbh->b_data, cnode);
				int elen = trie_node_name_len(cnode) - 2;

				if (elen == (int)name_len &&
				    memcmp(ename, name, name_len) == 0) {
					trie_node_set_inode(cnode, new_ino);
					TRIE_SET_FTYPE(cnode, new_type);
					mark_buffer_dirty(cbh);
					brelse(cbh);
					return 0;
				}
			}
			brelse(cbh);
			return -ENOENT;
		}

		if (!(cnode->node_type & NODE_TYPE_INTERM)) {
			/* Pure leaf where we need an INTERM. */
			char *ename = trie_node_name(cbh->b_data, cnode);
			int elen = trie_node_name_len(cnode) - 2;

			brelse(cbh);
			if (elen == (int)name_len &&
			    memcmp(ename, name, name_len) == 0) {
				/*
				 * A pure leaf occupies the exact full name.  To
				 * replace it with a new entry, the caller must split
				 * or remove the old one first; this update path only
				 * handles cases where the trie already has internal
				 * nodes for each byte.  Report ENOENT so the caller
				 * falls back to remove+insert.
				 */
				return -ENOENT;
			}
			return -ENOENT;
		}

		brelse(cbh);
		cur = child;
	}

	return -ENOENT;
}

/*
 * briefs_trie_remove - remove an entry from the directory trie.
 */
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_page *page, *cpage;
	struct briefs_trie_node *node, *pnode;
	u64 cur, parent, child_prev, child;
	u64 *ancestry;
	int anc;
	int pos;
	int ret;

	ancestry = kcalloc(TRIE_ANCESTRY_LIMIT, sizeof(u64), GFP_NOFS);
	ret = 0;

	if (TRIE_REF_IS_NULL(di->dir_trie_root)) {
		ret = -ENOENT;
		goto out;
	}

	parent = 0;
	child_prev = 0;
	cur = di->dir_trie_root;
	anc = 0;
	ancestry[anc++] = cur;

	for (pos = 0; pos < name_len; pos++) {
		ret = trie_find_child_with_prev(sb, cur, (u8)name[pos], &child, &child_prev);
		if (ret != 0) {
			ret = (ret == -ENOENT) ? -ENOENT : -EIO;
			goto out;
		}

		if (trie_read_node(sb, cur, &bh, &page, &pnode) != 0) {
			ret = -EIO;
			goto out;
		}

		if (pos == name_len - 1) {
			if (trie_read_node(sb, child, &cbh, &cpage, &node) != 0) {
				brelse(bh);
				ret = -EIO;
				goto out;
			}

			if ((node->node_type & NODE_TYPE_INTERM) &&
			    !(node->node_type & NODE_STATUS_LEAF)) {
				/* The name is a *prefix* of longer entries but is not itself
				 * an entry (LEAF not set), so it does not exist in the trie.
				 * This is -ENOENT, not -ENOTEMPTY: -ENOTEMPTY is reserved for
				 * removing a directory that still has children, which is a
				 * caller-level concern (briefs_dir.c:459 does the real rmdir
				 * emptiness check before calling us).  Returning -ENOTEMPTY
				 * here made cumulative journal replay (replay_dir_update's
				 * delete path) fail: it tolerates -ENOENT but treats
				 * -ENOTEMPTY as fatal, surfacing as replay -EIO /
				 * "can't read superblock" on remount (generic/003). */
				brelse(cbh);
				brelse(bh);
				ret = -ENOENT;
				goto out;
			}

			if (node->node_type & NODE_TYPE_INTERM) {
				bool has_children = !TRIE_REF_IS_NULL(trie_node_first_child(node)) ||
				                  trie_node_child_count(node) != 0;

				node->node_type &= ~NODE_STATUS_LEAF;
				if (has_children) {
					mark_buffer_dirty(cbh);
					brelse(cbh);
					brelse(bh);
					ret = 0;
					goto out;
				}
				mark_buffer_dirty(cbh);
				brelse(cbh);

				trie_unlink_child(sb, cur, child_prev, child);
				briefs_trie_free_node(sb, child);
				brelse(bh);
				goto collapse;
			}

			/* Pure leaf: unlink and free */
			brelse(cbh);
			trie_unlink_child(sb, cur, child_prev, child);
			briefs_trie_free_node(sb, child);
			brelse(bh);
			goto collapse;
		}

		parent = cur;
		child_prev = 0;
		cur = child;
		if (anc < TRIE_ANCESTRY_LIMIT)
			ancestry[anc++] = cur;
		brelse(bh);
	}

	ret = -ENOENT;
	goto out;

collapse:
	{
		int i;
		/* Collapse empty intermediate nodes including the leaf's immediate
			 * parent.  The ancestry array holds the path from root to leaf; the
			 * leaf itself was already removed before this label, so start at the
			 * parent of the removed leaf (anc - 1).
			 */
			for (i = anc - 1; i >= 1; i--) {
			u64 check = ancestry[i];
			struct buffer_head *cbh2, *pbh2;
			struct briefs_trie_page *cpage2, *ppage2;
			struct briefs_trie_node *cn2, *pn2;
			u64 pblk = ancestry[i - 1];

			if (trie_read_node(sb, check, &cbh2, &cpage2, &cn2) != 0)
				break;

			if (!(cn2->node_type & NODE_TYPE_INTERM) ||
			    (cn2->node_type & NODE_STATUS_LEAF) ||
			    trie_node_child_count(cn2) != 0 ||
			    !TRIE_REF_IS_NULL(trie_node_first_child(cn2))) {
				brelse(cbh2);
				break;
			}

			if (trie_get_node(sb, pblk, &pbh2, &ppage2, &pn2) != 0) {
				brelse(cbh2);
				break;
			}

			if (trie_node_first_child(pn2) == check) {
				trie_node_set_first_child(pn2, trie_node_next_sibling(cn2));
			} else {
				u64 w = trie_node_first_child(pn2);
				while (!TRIE_REF_IS_NULL(w)) {
					struct buffer_head *wbh;
					struct briefs_trie_page *wpage;
					struct briefs_trie_node *wn;
					if (trie_get_node(sb, w, &wbh, &wpage, &wn) != 0)
						break;
					if (trie_node_next_sibling(wn) == check) {
						trie_node_set_next_sibling(wn, trie_node_next_sibling(cn2));
						mark_buffer_dirty(wbh);
						brelse(wbh);
						break;
					}
					w = trie_node_next_sibling(wn);
					brelse(wbh);
				}
			}
			trie_node_set_child_count(pn2, trie_node_child_count(pn2) - 1);
			mark_buffer_dirty(pbh2);
			brelse(pbh2);

			briefs_trie_free_node(sb, check);
			brelse(cbh2);
		}

		/*
		 * If the collapse left the root as an empty intermediate node
		 * (no children), free it too and clear the directory trie root.
		 * Otherwise an unlinked sole entry leaves the directory pointing
		 * at an empty trie page that is never referenced again.
		 */
		if (!TRIE_REF_IS_NULL(di->dir_trie_root)) {
			struct buffer_head *rbh;
			struct briefs_trie_page *rpage;
			struct briefs_trie_node *rnode;

			if (trie_read_node(sb, di->dir_trie_root, &rbh, &rpage, &rnode) == 0) {
				if ((rnode->node_type & NODE_TYPE_INTERM) &&
				    !(rnode->node_type & NODE_STATUS_LEAF) &&
				    trie_node_child_count(rnode) == 0 &&
				    TRIE_REF_IS_NULL(trie_node_first_child(rnode))) {
					briefs_trie_free_node(sb, di->dir_trie_root);
					di->dir_trie_root = 0;
				}
				brelse(rbh);
			}
		}
	}

out:
	kfree(ancestry);
	return ret;
}

/*
 * briefs_trie_free_all - free all nodes in a directory trie recursively.
 */
void briefs_trie_free_all(struct super_block *sb, struct briefs_inode *di)
{
	struct trie_stack_entry {
		u64 ref;
		int state;
	} *stack;
	int sp, stack_cap;

	if (TRIE_REF_IS_NULL(di->dir_trie_root))
		return;

	stack_cap = 512;
	stack = __vmalloc(stack_cap * sizeof(*stack), GFP_KERNEL);
	if (!stack) {
		pr_err("briefs: failed to allocate trie free stack\n");
		return;
	}

	sp = 0;
	stack[sp].ref = di->dir_trie_root;
	stack[sp].state = 0;
	sp++;

	while (sp > 0) {
		u64 ref;
		int state;
		struct buffer_head *bh;
		struct briefs_trie_page *page;
		struct briefs_trie_node *node;

		ref = stack[sp - 1].ref;
		state = stack[sp - 1].state;

		if (state == 1) {
			briefs_trie_free_node(sb, ref);
			sp--;
			continue;
		}

		if (trie_read_node(sb, ref, &bh, &page, &node) != 0) {
			sp--;
			continue;
		}

		stack[sp - 1].state = 1;

		{
			u64 child = trie_node_first_child(node);
			while (!TRIE_REF_IS_NULL(child)) {
				struct buffer_head *cbh;
				struct briefs_trie_page *cpage;
				struct briefs_trie_node *cn;
				u64 next;

				if (trie_read_node(sb, child, &cbh, &cpage, &cn) != 0)
					break;
				next = trie_node_next_sibling(cn);
				brelse(cbh);

				if (sp < 512) {
					stack[sp].ref = child;
					stack[sp].state = 0;
					sp++;
				}
				child = next;
			}
		}

		brelse(bh);
	}

	vfree(stack);
	di->dir_trie_root = 0;
}

/*
 * briefs_trie_iter_alloc - allocate a trie iterator with a dynamically
 * growing stack.  The returned iterator must be freed with
 * briefs_trie_iter_free().
 */
struct trie_iter *briefs_trie_iter_alloc(void)
{
	struct trie_iter *iter;

	iter = kmalloc(sizeof(*iter), GFP_KERNEL);
	if (!iter)
		return NULL;

	iter->cap = 256;
	iter->stack = kmalloc_array(iter->cap, sizeof(u64), GFP_KERNEL);
	iter->leaf_emitted = kmalloc_array(iter->cap, sizeof(u8), GFP_KERNEL);
	if (!iter->stack || !iter->leaf_emitted) {
		kfree(iter->stack);
		kfree(iter->leaf_emitted);
		kfree(iter);
		return NULL;
	}

	iter->sp = 0;
	iter->pending = false;
	iter->gen = 0;
	iter->emit_idx = 0;
	return iter;
}

/*
 * briefs_trie_iter_free - free a trie iterator and its dynamic stack.
 */
void briefs_trie_iter_free(struct trie_iter *iter)
{
	if (!iter)
		return;
	kfree(iter->stack);
	kfree(iter->leaf_emitted);
	kfree(iter);
}

/*
 * Grow the iterator stack to fit at least need more entries.
 * Returns 0 on success, -ENOMEM on failure.
 */
static int trie_iter_grow(struct trie_iter *iter, int need)
{
	int new_cap = iter->cap;

	while (new_cap < iter->sp + need)
		new_cap *= 2;

	if (new_cap == iter->cap)
		return 0;

	/*
	 * Grow both arrays atomically: allocate the two replacements first, copy,
	 * then free the olds and swap.  krealloc() cannot do this safely in a pair
	 * -- on failure it returns NULL but leaves the old block intact, while on
	 * success it may free the old block and move.  Calling krealloc() twice and
	 * freeing on partial failure therefore double-frees whichever succeeded
	 * (its old block was already freed by the move) and leaves the iterator
	 * holding a dangling pointer that briefs_trie_iter_free() then kfree()s
	 * again -- a cross-slab double-free / use-after-free.  The all-or-nothing
	 * kmalloc_array + copy + free-old sequence below never touches the live
	 * arrays until both replacements exist, so on -ENOMEM the iterator is left
	 * exactly as the caller found it (no dangling pointer, no double-free).
	 */
	{
		u64 *new_stack;
		u8 *new_emitted;

		new_stack = kmalloc_array(new_cap, sizeof(u64), GFP_KERNEL);
		if (!new_stack)
			return -ENOMEM;
		new_emitted = kmalloc_array(new_cap, sizeof(u8), GFP_KERNEL);
		if (!new_emitted) {
			kfree(new_stack);
			return -ENOMEM;
		}
		memcpy(new_stack, iter->stack, iter->cap * sizeof(u64));
		memcpy(new_emitted, iter->leaf_emitted, iter->cap * sizeof(u8));
		kfree(iter->stack);
		kfree(iter->leaf_emitted);
		iter->stack = new_stack;
		iter->leaf_emitted = new_emitted;
		iter->cap = new_cap;
	}
	return 0;
}

/*
 * Push a node reference onto the iterator stack.
 * Returns 0 on success, -ENOMEM if the stack could not be grown.
 */
static int trie_iter_push(struct trie_iter *iter, u64 ref, u8 emitted)
{
	int ret;

	ret = trie_iter_grow(iter, 1);
	if (ret)
		return ret;

	iter->stack[iter->sp] = ref;
	iter->leaf_emitted[iter->sp] = emitted;
	iter->sp++;
	return 0;
}

/*
 * briefs_trie_iter_init - initialize a trie iteration starting from root.
 */
void briefs_trie_iter_init(struct trie_iter *iter, struct briefs_inode *di, u64 gen)
{
	iter->sp = 0;
	iter->pending = false;
	iter->gen = gen;
	iter->emit_idx = 0;
	if (!TRIE_REF_IS_NULL(di->dir_trie_root)) {
		if (iter->cap > 0) {
			iter->stack[0] = di->dir_trie_root;
			iter->sp = 1;
			iter->leaf_emitted[0] = 0;
		}
	}
}

/*
 * briefs_trie_iter_next - get the next leaf entry from the trie.
 */
int briefs_trie_iter_next(struct super_block *sb, struct trie_iter *iter,
                          u64 current_gen, u64 *ino, u8 *type, char *name_buf,
                          int *name_len)
{
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	int ret;

	if (iter->gen != current_gen)
		return -ESTALE;

	if (iter->pending) {
		iter->pending = false;
		if (ino) *ino = iter->pending_ino;
		if (type) *type = iter->pending_type;
		if (name_buf && name_len) {
			memcpy(name_buf, iter->pending_name_buf, iter->pending_name_len);
			*name_len = iter->pending_name_len;
		}
		return 0;
	}

	while (iter->sp > 0) {
		u64 ref;
		u8 emitted;

		ref = iter->stack[iter->sp - 1];
		emitted = iter->leaf_emitted[iter->sp - 1];
		iter->sp--;

		if (trie_read_node(sb, ref, &bh, &page, &node) != 0)
			continue;

		if (emitted) {
			goto push_children;
		}

		if (TRIE_IS_LEAF(node)) {
			char *src = trie_node_name(bh->b_data, node);
			int nlen = trie_node_name_len(node) - 2;

			if (nlen > 0 && nlen <= BRIEFS_NAME_LEN) {
				if (ino) *ino = trie_node_inode(node);
				if (type) *type = TRIE_NODE_FTYPE(node);
				if (name_buf && name_len) {
					memcpy(name_buf, src, nlen);
					*name_len = nlen;
				}

				if (!TRIE_REF_IS_NULL(trie_node_first_child(node))) {
					ret = trie_iter_push(iter, ref, 1);
					if (ret) {
						brelse(bh);
						return ret;
					}
				}

				brelse(bh);
				return 0;
			}
		}

	push_children:
		if (!TRIE_REF_IS_NULL(trie_node_first_child(node))) {
			u64 child = trie_node_first_child(node);
			int pushed = 0;

			while (!TRIE_REF_IS_NULL(child)) {
				ret = trie_iter_push(iter, child, 0);
				if (ret) {
					brelse(bh);
					return ret;
				}
				pushed++;

				{
					struct buffer_head *cbh;
					struct briefs_trie_page *cpage;
					struct briefs_trie_node *cn;
					if (trie_read_node(sb, child, &cbh, &cpage, &cn) != 0)
						break;
					child = trie_node_next_sibling(cn);
					brelse(cbh);
				}
			}

			if (pushed) {
				brelse(bh);
				continue;
			}
		}

		brelse(bh);
	}

	return -ENOENT;
}
