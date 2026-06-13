/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * BrieFS directory trie.
 *
 * Each directory has a trie structure stored in individual blocks.
 * The root block number is in inode->dir_trie_root.
 *
 * Trie layout:
 *   - Internal nodes (NODE_TYPE_INTERM) have child_count > 0 and dispatch
 *     by the byte value at the node's depth in the filename.  An INTERM
 *     node with NODE_STATUS_LEAF set also stores a leaf entry whose name
 *     is a prefix of longer names branching from this node.
 *   - Pure leaf nodes (child_count = 0) store the full filename + inode in
 *     the trailing bytes of the block.
 *   - Children at each depth level are linked via first_child / next_sibling.
 *   - The root node is special: depth=0, byte_val=0.  It is always an INTERM
 *     node and may or may not have NODE_STATUS_LEAF.
 *
 * Names are stored in the trailing bytes of each node's block.
 * name_offset is the number of bytes from the end of the block to the
 * start of the name data, which consists of a 2-byte length prefix followed
 * by the name bytes.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/vmalloc.h>
#include "briefs.h"
#include "briefs_alloc.h"

/* Number of bytes for the 2-byte name-length prefix stored in the block */
#define TRIE_NAME_PREFIX 2

#define TRIE_ANCESTRY_LIMIT 256

/*
 * briefs_trie_alloc_node - allocate a block for a new trie node.
 * Returns the absolute block number, or 0 on failure.
 */
static u64 briefs_trie_alloc_node(struct super_block *sb)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 rel;

	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return 0;
	return data_to_abs(bsi->sb, rel);
}

/*
 * briefs_trie_free_node - free a trie node block.
 */
static void briefs_trie_free_node(struct super_block *sb, u64 block)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 rel;

	rel = abs_to_data(bsi->sb, block);
	briefs_free_block(&bsi->alloc, rel);
}

/*
 * briefs_trie_init_block - clear and initialize a trie node block.
 */
static void briefs_trie_init_block(struct super_block *sb, u64 block,
                                    u8 depth, u8 byte_val, u8 node_type)
{
	struct buffer_head *bh;
	struct briefs_trie_node *node;

	bh = sb_getblk(sb, block);
	if (!bh)
		return;
	/* sb_getblk may return an unmapped buffer on loop devices */
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
	}
	memset(bh->b_data, 0, sb->s_blocksize);
	node = (struct briefs_trie_node *)bh->b_data;
	node->magic = BRIEFS_TRIE_MAGIC;
	node->depth = depth;
	node->byte_val = byte_val;
	node->node_type = node_type;
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

/*
 * briefs_trie_find_child - find a child node by byte value.
 */
static u64 briefs_trie_find_child(struct super_block *sb, u64 parent_block,
                                   u8 byte_val)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_node *node, *cn;
	u64 child;

	bh = sb_bread(sb, parent_block);
	if (!bh)
		return 0;

	node = (struct briefs_trie_node *)bh->b_data;
	child = node->first_child;

	while (child) {
		cbh = sb_bread(sb, child);
		if (!cbh)
			break;
		cn = (struct briefs_trie_node *)cbh->b_data;
		if (cn->magic != BRIEFS_TRIE_MAGIC) {
			brelse(cbh);
			break;
		}
		if (cn->byte_val == byte_val) {
			u64 result;

			result = child;
			brelse(cbh);
			brelse(bh);
			return result;
		}
		child = cn->next_sibling;
		brelse(cbh);
	}

	brelse(bh);
	return 0;
}

/*
 * briefs_trie_create_child - create a child node of the given parent.
 * Adds it to the sibling chain. Returns the block number, or 0 on failure.
 */
static u64 briefs_trie_create_child(struct super_block *sb, u64 parent_block,
                                     u8 depth, u8 byte_val, u8 node_type)
{
	struct buffer_head *pbh, *cbh;
	struct briefs_trie_node *pnode, *cnode;
	u64 child_block;

	child_block = briefs_trie_alloc_node(sb);
	if (child_block == 0)
		return 0;

	briefs_trie_init_block(sb, child_block, depth, byte_val, node_type);

	pbh = sb_bread(sb, parent_block);
	if (!pbh) {
		briefs_trie_free_node(sb, child_block);
		return 0;
	}

	pnode = (struct briefs_trie_node *)pbh->b_data;
	if (pnode->first_child == 0) {
		pnode->first_child = child_block;
	} else {
		u64 last;

		last = pnode->first_child;
		while (last) {
			cbh = sb_bread(sb, last);
			if (!cbh)
				break;
			cnode = (struct briefs_trie_node *)cbh->b_data;
			if (cnode->next_sibling == 0) {
				cnode->next_sibling = child_block;
				mark_buffer_dirty(cbh);
				sync_dirty_buffer(cbh);
				brelse(cbh);
				break;
			}
			last = cnode->next_sibling;
			brelse(cbh);
		}
	}
	pnode->child_count++;
	mark_buffer_dirty(pbh);
	sync_dirty_buffer(pbh);
	brelse(pbh);

	return child_block;
}

/*
 * briefs_trie_find_or_create_child - find or create a child by byte_val.
 */
static u64 briefs_trie_find_or_create_child(struct super_block *sb, u64 parent_block,
                                             u8 depth, u8 byte_val, u8 node_type)
{
	u64 child;

	child = briefs_trie_find_child(sb, parent_block, byte_val);
	if (child)
		return child;
	return briefs_trie_create_child(sb, parent_block, depth, byte_val, node_type);
}

/*
 * briefs_trie_lookup - find an entry by name in the directory trie.
 *
 * Returns 0 on success with *found_ino and *found_type set.
 * Returns -ENOENT if not found.
 */
int briefs_trie_lookup(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len,
                        u64 *found_ino, u8 *found_type)
{
	struct buffer_head *cbh;
	struct briefs_trie_node *cnode;
	u64 cur, child;
	int pos;

	if (!di->dir_trie_root)
		return -ENOENT;

	cur = di->dir_trie_root;

	for (pos = 0; pos < name_len; pos++) {
		u8 bval = (u8)name[pos];
		int found_leaf_at_byte;

		if (pos == name_len - 1) {
			/*
			 * Last byte.  Check if a leaf exists at this
			 * byte position.
			 */
			child = briefs_trie_find_child(sb, cur, bval);
			if (child == 0)
				return -ENOENT;

			cbh = sb_bread(sb, child);
			if (!cbh)
				return -EIO;

			cnode = (struct briefs_trie_node *)cbh->b_data;
			found_leaf_at_byte = 0;

			/* Check if the child has leaf data */
			if (TRIE_IS_LEAF(cnode)) {
				char *ename;
				int elen;

				ename = (char *)cbh->b_data + BRIEFS_BLOCK_SIZE - cnode->name_offset + 2;
				elen = cnode->name_len - 2;
				if (elen == name_len && memcmp(ename, name, name_len) == 0) {
					*found_ino = cnode->inode;
					*found_type = TRIE_NODE_FTYPE(cnode);
					found_leaf_at_byte = 1;
				}
			}

			/*
			 * Even if the exact match is found and returned, we
			 * still look at children.  If there are longer entries
			 * that share this prefix, they exist as children of
			 * this node.  But for lookup, we only care about the
			 * exact match.
			 */
			brelse(cbh);
			if (found_leaf_at_byte)
				return 0;

			/*
			 * No exact match.  If this is an INTERM node without
			 * the leaf flag, we know the name differs.
			 */
			return -ENOENT;
		}

		/*
		 * Middle byte: find an INTERM child.
		 */
		child = briefs_trie_find_child(sb, cur, bval);
		if (child == 0)
			return -ENOENT;

		cbh = sb_bread(sb, child);
		if (!cbh)
			return -EIO;
		cnode = (struct briefs_trie_node *)cbh->b_data;

		if (!(cnode->node_type & NODE_TYPE_INTERM)) {
			/*
			 * Found a pure leaf where we need an INTERM.
			 * Check if its full name matches the search name.
			 */
			char *ename;
			int elen;

			ename = (char *)cbh->b_data + BRIEFS_BLOCK_SIZE - cnode->name_offset + 2;
			elen = cnode->name_len - 2;
			brelse(cbh);

			if (elen == name_len && memcmp(ename, name, name_len) == 0) {
				*found_ino = cnode->inode;
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
 * briefs_trie_create_root - create the root node of a directory trie.
 */
int briefs_trie_create_root(struct super_block *sb, struct briefs_inode *di)
{
	u64 root_block;

	root_block = briefs_trie_alloc_node(sb);
	if (root_block == 0)
		return -ENOSPC;

	briefs_trie_init_block(sb, root_block, 0, 0, NODE_TYPE_INTERM);
	di->dir_trie_root = root_block;

	return 0;
}

/*
 * briefs_trie_insert helper: split an existing pure leaf node when a new name
 * shares its prefix through this position.  This creates an INTERM node and
 * reparents the old leaf under it, handling both cases: when the old leaf
 * is a prefix of the new name, and when the names differ at the next byte.
 * 'child' is the block of the existing pure leaf, 'cur' is the current parent.
 * 'pos' is the depth/position being split at.
 * 'name' is the full new name being inserted (for name_len + data).
 * On return, 'cur' is updated to the INTERM node for continued descent.
 */

/*
 * briefs_trie_insert - insert a name/inode entry into the directory trie.
 *
 * Returns 0 on success, -EEXIST if entry already exists, negative on error.
 */
int briefs_trie_insert(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len,
                        u64 ino, u8 type)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_node *node, *cnode;
	u64 cur, child, new_leaf;
	int pos, ret;

	if (!di->dir_trie_root) {
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
			/*
			 * Last byte of the name.  Check if a node already
			 * exists at this byte value.
			 */
			u64 existing = briefs_trie_find_child(sb, cur, bval);

			if (existing) {
				cbh = sb_bread(sb, existing);
				if (cbh) {
					cnode = (struct briefs_trie_node *)cbh->b_data;

					if (cnode->node_type & NODE_TYPE_INTERM) {
						/*
						 * Existing INTERM node.  Check for
						 * duplicate name first.
						 */
						if (cnode->node_type & NODE_STATUS_LEAF) {
							char *ename;
							int elen;

							ename = (char *)cbh->b_data + BRIEFS_BLOCK_SIZE - cnode->name_offset + 2;
							elen = cnode->name_len - 2;
							if (elen == name_len && memcmp(ename, name, name_len) == 0) {
								brelse(cbh);
								return -EEXIST;
							}
						}

						/*
						 * Set NODE_STATUS_LEAF on the existing
						 * INTERM node.  Its name is a prefix
						 * of longer names branching from it.
						 */
						cnode->node_type |= NODE_STATUS_LEAF;
						TRIE_SET_FTYPE(cnode, type);
						cnode->inode = ino;
						cnode->name_len = 2 + name_len;
						{
							u16 n_off = 2 + name_len;
							char *n_dest = (char *)cbh->b_data + BRIEFS_BLOCK_SIZE - n_off;
							*(__u16 *)n_dest = (__u16)name_len;
							memcpy(n_dest + 2, name, name_len);
							cnode->name_offset = n_off;
						}

						mark_buffer_dirty(cbh);
						sync_dirty_buffer(cbh);
						brelse(cbh);
						return 0;
					}

					/*
					 * Existing pure leaf.  Check for duplicate,
					 * then split.
					 */
					{
						char *ename = (char *)cbh->b_data + BRIEFS_BLOCK_SIZE - cnode->name_offset + 2;
						int elen = cnode->name_len - 2;
						if (elen == name_len && memcmp(ename, name, name_len) == 0) {
							brelse(cbh);
							return -EEXIST;
						}
					}

					/*
					 * Two different names share all bytes through
					 * this position.  Need to split: create an INTERM
					 * node and reparent the existing leaf under it.
					 */
					brelse(cbh);
					goto split_leaf;
				}
			}

			/*
			 * No existing child at this byte value.
			 * Create a new pure leaf node.
			 */
			new_leaf = briefs_trie_alloc_node(sb);
			if (new_leaf == 0)
				return -ENOSPC;

			bh = sb_getblk(sb, new_leaf);
			if (!bh) {
				briefs_trie_free_node(sb, new_leaf);
				return -EIO;
			}
			/* sb_getblk may return an unmapped buffer on loop devices */
			if (!buffer_mapped(bh)) {
				bh->b_blocknr = new_leaf;
				set_buffer_mapped(bh);
			}
			memset(bh->b_data, 0, sb->s_blocksize);
			node = (struct briefs_trie_node *)bh->b_data;
			node->magic = BRIEFS_TRIE_MAGIC;
			node->depth = pos;
			node->byte_val = bval;
			node->node_type = 0;
			TRIE_SET_FTYPE(node, type);
			node->inode = ino;
			node->name_len = 2 + name_len;
			{
				u16 n_off = 2 + name_len;
				char *n_dest = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - n_off;
				*(__u16 *)n_dest = (__u16)name_len;
				memcpy(n_dest + 2, name, name_len);
				node->name_offset = n_off;
			}
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);

			goto link_child;
		}

		/*
		 * Middle byte: find or create an INTERM child.
		 */
		child = briefs_trie_find_or_create_child(sb, cur, pos + 1, bval, NODE_TYPE_INTERM);
		if (child == 0)
			return -ENOSPC;

		cbh = sb_bread(sb, child);
		if (cbh) {
			cnode = (struct briefs_trie_node *)cbh->b_data;
			if (!(cnode->node_type & NODE_TYPE_INTERM)) {
				/*
				 * Found a pure leaf where we need an INTERM.
				 * Shared prefix collision — split.
				 */
				brelse(cbh);
				goto split_leaf;
			}
			brelse(cbh);
			cur = child;
		} else {
			cur = child;
		}

		continue;

split_leaf:
	{
		struct buffer_head *lbh;
		struct briefs_trie_node *lnode;
		int old_name_len;

		/*
		 * Read the old leaf to determine its full name length.
		 */
		lbh = sb_bread(sb, child);
		if (!lbh)
			return -EIO;
		lnode = (struct briefs_trie_node *)lbh->b_data;
		old_name_len = lnode->name_len - 2;

		if (old_name_len == pos + 1) {
			/*
			 * The old leaf is a prefix of the new name.
			 * Convert it to an INTERM node with
			 * NODE_STATUS_LEAF set, and continue descending
			 * for the remaining bytes of the new name.
			 * The old leaf's ftype is preserved in f_type (formerly
			 * reserved[0]).
			 */
			lnode->node_type = NODE_TYPE_INTERM | NODE_STATUS_LEAF;
			lnode->depth = pos + 1;
			mark_buffer_dirty(lbh);
			sync_dirty_buffer(lbh);
			brelse(lbh);
			cur = child;
		} else {
			/*
			 * The old leaf and new name differ at the next
			 * byte.  Create an INTERM node and reparent the
			 * old leaf under it.
			 */
			u64 internal;
			u64 *link;

				u64 old_sibling = lnode->next_sibling;
			brelse(lbh);

			internal = briefs_trie_alloc_node(sb);
			if (internal == 0)
				return -ENOSPC;
			briefs_trie_init_block(sb, internal, pos + 1, bval, NODE_TYPE_INTERM);

			/*
			 * Re-parent: find the link from grandparent (cur)
			 * to the leaf (`child`), redirect it to `internal`.
			 */
			bh = sb_bread(sb, cur);
			if (bh) {
				node = (struct briefs_trie_node *)bh->b_data;
				link = &node->first_child;
				while (*link && *link != child) {
					struct briefs_trie_node *tmp;
					struct buffer_head *tbh;

					tbh = sb_bread(sb, *link);
					if (!tbh)
						break;
					tmp = (struct briefs_trie_node *)tbh->b_data;
					link = &tmp->next_sibling;
					brelse(tbh);
				}
				if (*link == child)
					*link = internal;
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
			}

			/*
			 * Link the old leaf as a child of the new INTERM.
			 * The leaf retains its original byte_val (the byte
			 * that differentiates it from the new name at the
			 * next position).  The new INTERM has byte_val = bval
			 * (the byte of the new name at this position).
			 */
			bh = sb_bread(sb, internal);
			if (bh) {
				node = (struct briefs_trie_node *)bh->b_data;
				node->first_child = child;
					node->next_sibling = old_sibling;
				node->child_count = 1;
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);
			}

			/*
			 * If this split happened at the last byte of the
			 * new name, the new name is a prefix of the old leaf.
			 * Store the new name's entry on the new INTERM node
			 * with NODE_STATUS_LEAF.
			 */
			if (pos == name_len - 1) {
				bh = sb_bread(sb, internal);
				if (bh) {
					node = (struct briefs_trie_node *)bh->b_data;
					node->node_type |= NODE_STATUS_LEAF;
					TRIE_SET_FTYPE(node, type);
					node->inode = ino;
					node->name_len = 2 + name_len;
					{
						u16 n_off = 2 + name_len;
						char *n_dest = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - n_off;
						*(__u16 *)n_dest = (__u16)name_len;
						memcpy(n_dest + 2, name, name_len);
						node->name_offset = n_off;
					}
					mark_buffer_dirty(bh);
					sync_dirty_buffer(bh);
					brelse(bh);
				}
			}

			cur = internal;
		}
	}
	continue;

link_child:
		bh = sb_bread(sb, cur);
		if (!bh) {
			briefs_trie_free_node(sb, new_leaf);
			return -EIO;
		}
		node = (struct briefs_trie_node *)bh->b_data;
		if (node->first_child == 0) {
			node->first_child = new_leaf;
		} else {
			u64 last = node->first_child;

			while (last) {
				cbh = sb_bread(sb, last);
				if (!cbh)
					break;
				cnode = (struct briefs_trie_node *)cbh->b_data;
				if (cnode->next_sibling == 0) {
					cnode->next_sibling = new_leaf;
					mark_buffer_dirty(cbh);
					sync_dirty_buffer(cbh);
					brelse(cbh);
					break;
				}
				last = cnode->next_sibling;
				brelse(cbh);
			}
		}
		node->child_count++;
		mark_buffer_dirty(bh);
		sync_dirty_buffer(bh);
		brelse(bh);

		return 0;
	}

	return -EINVAL;
}

/*
 * briefs_trie_remove - remove an entry from the directory trie.
 *
 * Walks down the trie to find the target leaf node, unlinks it from its
 * parent's sibling chain, frees it, and then walks back up collapsing any
 * pure-INTERM routing nodes that become dead (no leaf entry, no children,
 * child_count == 0).
 *
 * Both removal sites (pure leaf removal and clearing NODE_STATUS_LEAF on
 * an INTERM node) trigger the collapse walk.
 *
 * Returns 0 on success, -ENOENT if not found, -ENOTEMPTY if the target
 * node is a pure INTERM with children (can't be removed through the trie).
 */
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_node *node, *pnode;
	u64 cur, parent, child_prev, child;
	/* Ancestry path from root to the leaf's parent, for collapse */
	/* u64 ancestry[256]; -- allocate ancestry explicitly*/
	u64 *ancestry;
	int anc;
	int pos;
	int ret;

	ancestry = kcalloc(TRIE_ANCESTRY_LIMIT, sizeof(u64), GFP_NOFS);
	ret = 0;

	if (!di->dir_trie_root) {
		ret = -ENOENT;
		goto out;
	}

	parent = 0;
	child_prev = 0;
	cur = di->dir_trie_root;
	anc = 0;
	ancestry[anc++] = cur;

	for (pos = 0; pos < name_len; pos++) {
		bh = sb_bread(sb, cur);
		if (!bh) {
			ret = -EIO;
			goto out;
		}
		pnode = (struct briefs_trie_node *)bh->b_data;
		child = pnode->first_child;
		child_prev = 0;

		while (child) {
			struct buffer_head *tbh;
			struct briefs_trie_node *tnode;

			tbh = sb_bread(sb, child);
			if (!tbh) {
				brelse(bh);
				ret = -EIO;
				goto out;
			}
			tnode = (struct briefs_trie_node *)tbh->b_data;
			if (tnode->byte_val == (u8)name[pos]) {
				brelse(tbh);
				break;
			}
			child_prev = child;
			child = tnode->next_sibling;
			brelse(tbh);
		}

		if (child == 0) {
			brelse(bh);
			ret = -ENOENT;
			goto out;
		}

		if (pos == name_len - 1) {
			cbh = sb_bread(sb, child);
			if (!cbh) {
				brelse(bh);
				ret = -EIO;
				goto out;
			}
			node = (struct briefs_trie_node *)cbh->b_data;

			if ((node->node_type & NODE_TYPE_INTERM) &&
			    !(node->node_type & NODE_STATUS_LEAF)) {
				/*
				 * Pure INTERM node — has children but no leaf
				 * entry.  Cannot unlink "a directory with children"
				 * through the trie.
				 */
				brelse(cbh);
				brelse(bh);
				ret = -ENOTEMPTY;
				goto out;
			}

			/*
			 * Found the node.  Unlink it from the parent's
			 * sibling chain.  If it's an INTERM with
			 * NODE_STATUS_LEAF, just clear the leaf flag
			 * — don't free the node, it still has children.
			 * If it's a pure leaf, unlink and free.
			 */
			if (node->node_type & NODE_TYPE_INTERM) {
				/*
				 * INTERM with NODE_STATUS_LEAF: clear leaf flag.
				 * Check whether the node has children BEFORE
				 * releasing cbh — node is invalid after brelse.
				 */
				bool has_children = (node->first_child != 0 ||
						    node->child_count != 0);

				node->node_type &= ~NODE_STATUS_LEAF;

				if (has_children) {
					/* Still has children, keep the node */
					mark_buffer_dirty(cbh);
					sync_dirty_buffer(cbh);
					brelse(cbh);
					brelse(bh);
					ret = 0;
					goto out;
				}

				/*
				 * No children — the node is dead.
				 * Unlink from parent, free, collapse up.
				 */
				mark_buffer_dirty(cbh);
				sync_dirty_buffer(cbh);
				brelse(cbh);

				if (child_prev == 0) {
					pnode->first_child = node->next_sibling;
				} else {
					struct buffer_head *ppbh;
					struct briefs_trie_node *ppn;
					ppbh = sb_bread(sb, child_prev);
					if (ppbh) {
						ppn = (struct briefs_trie_node *)ppbh->b_data;
						ppn->next_sibling = node->next_sibling;
						mark_buffer_dirty(ppbh);
						sync_dirty_buffer(ppbh);
						brelse(ppbh);
					}
				}
				pnode->child_count--;
				mark_buffer_dirty(bh);
				sync_dirty_buffer(bh);
				brelse(bh);

				briefs_trie_free_node(sb, child);
				goto collapse;
			}

			/* Pure leaf: unlink and free */
			if (child_prev == 0) {
				pnode->first_child = node->next_sibling;
			} else {
				struct buffer_head *ppbh;
				struct briefs_trie_node *ppn;

				ppbh = sb_bread(sb, child_prev);
				if (ppbh) {
					ppn = (struct briefs_trie_node *)ppbh->b_data;
					ppn->next_sibling = node->next_sibling;
					mark_buffer_dirty(ppbh);
					sync_dirty_buffer(ppbh);
					brelse(ppbh);
				}
			}
			pnode->child_count--;
			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(cbh);

			briefs_trie_free_node(sb, child);

			/*
			 * Now try to collapse dead routing nodes starting
			 * from cur (the leaf's parent).
			 */
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
	/*
	 * Walk up the ancestry from cur toward root, freeing any dead
	 * pure-INTERM routing nodes (no NODE_STATUS_LEAF, child_count == 0,
	 * first_child == 0).
	 *
	 * ancestry[anc-1] is the target node (already freed), ancestry[anc-2]
	 * is its parent, and ancestry[anc-3] is the grandparent.
	 */
	int i;

	for (i = anc - 2; i >= 1; i--) {
		u64 check = ancestry[i];
		struct buffer_head *cbh2;
		struct briefs_trie_node *cn2;
		struct buffer_head *pbh2;
		struct briefs_trie_node *pn2;
		u64 pblk = ancestry[i - 1];

		cbh2 = sb_bread(sb, check);
		if (!cbh2)
			break;
		cn2 = (struct briefs_trie_node *)cbh2->b_data;

		if (!(cn2->node_type & NODE_TYPE_INTERM) ||
		    (cn2->node_type & NODE_STATUS_LEAF) ||
		    cn2->child_count != 0 ||
		    cn2->first_child != 0) {
			brelse(cbh2);
			break;
		}

		/* Dead routing node.  Unlink from parent and free. */
		pbh2 = sb_bread(sb, pblk);
		if (!pbh2) {
			brelse(cbh2);
			break;
		}
		pn2 = (struct briefs_trie_node *)pbh2->b_data;

		if (pn2->first_child == check) {
			pn2->first_child = cn2->next_sibling;
		} else {
			u64 w = pn2->first_child;
			while (w) {
				struct buffer_head *wbh;
				struct briefs_trie_node *wn;
				wbh = sb_bread(sb, w);
				if (!wbh) break;
				wn = (struct briefs_trie_node *)wbh->b_data;
				if (wn->next_sibling == check) {
					wn->next_sibling = cn2->next_sibling;
					mark_buffer_dirty(wbh);
					sync_dirty_buffer(wbh);
					brelse(wbh);
					break;
				}
				w = wn->next_sibling;
				brelse(wbh);
			}
		}
		pn2->child_count--;
		mark_buffer_dirty(pbh2);
		sync_dirty_buffer(pbh2);
		brelse(pbh2);


		briefs_trie_free_node(sb, check);
		brelse(cbh2);
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
		u64 block;
		int state;
	} *stack;
	int sp, stack_cap;

	if (!di->dir_trie_root)
		return;

	stack_cap = 512;
	stack = __vmalloc(stack_cap * sizeof(struct trie_stack_entry), GFP_KERNEL);
	if (!stack) {
		pr_err("briefs: failed to allocate trie free stack\n");
		return;
	}

	sp = 0;
	stack[sp].block = di->dir_trie_root;
	stack[sp].state = 0;
	sp++;

	while (sp > 0) {
		u64 block;
		int state;
		struct buffer_head *bh;
		struct briefs_trie_node *node;

		block = stack[sp - 1].block;
		state = stack[sp - 1].state;

		if (state == 1) {
			briefs_trie_free_node(sb, block);
			sp--;
			continue;
		}

		bh = sb_bread(sb, block);
		if (!bh) {
			sp--;
			continue;
		}
		node = (struct briefs_trie_node *)bh->b_data;

		stack[sp - 1].state = 1;

		{
			u64 child;

			child = node->first_child;
			while (child) {
				struct buffer_head *cbh;
				struct briefs_trie_node *cn;
				u64 next;

				cbh = sb_bread(sb, child);
				if (!cbh)
					break;
				cn = (struct briefs_trie_node *)cbh->b_data;
				next = cn->next_sibling;
				brelse(cbh);

				if (sp < 512) {
					stack[sp].block = child;
					stack[sp].state = 0;
					sp++;
				}
				child = next;
			}
		}

		brelse(bh);
	}

	if (stack)
		vfree(stack);

	di->dir_trie_root = 0;
}

/*
 * briefs_trie_iter_init - initialize a trie iteration starting from root.
 */
void briefs_trie_iter_init(struct trie_iter *iter, struct briefs_inode *di, u64 gen)
{
	iter->sp = 0;
	iter->pending = false;
	iter->gen = gen;
	if (di->dir_trie_root) {
		iter->stack[0] = di->dir_trie_root;
		iter->sp = 1;
		iter->leaf_emitted[0] = 0;
	}
}

/*
 * briefs_trie_iter_next - get the next leaf entry from the trie.
 *
 * Walks the trie depth-first, emitting leaf entries in order.  When a node
 * has both leaf data (NODE_STATUS_LEAF) and children, the leaf is emitted
 * first (it represents a shorter name that is a prefix of longer names
 * branching from this node).  The node is re-pushed after emitting its leaf
 * so that its children are processed next; leaf_emitted[] tracks that we've
 * already emitted this node's leaf so we don't emit it again on re-visit.
 *
 * Returns 0 if an entry was found, -ENOENT if iteration is complete.
 */
int briefs_trie_iter_next(struct super_block *sb, struct trie_iter *iter,
                                  u64 current_gen, u64 *ino, u8 *type, char *name_buf, int *name_len)
{
	struct buffer_head *bh;
	struct briefs_trie_node *node;

	if (iter->gen != current_gen)
		return -ESTALE;


	/* Return a previously-saved pending entry (dir_emit failed on it) */
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
		u64 block;
		u8 emitted;

		block = iter->stack[iter->sp - 1];
		emitted = iter->leaf_emitted[iter->sp - 1];
		iter->sp--;

		bh = sb_bread(sb, block);
		if (!bh)
			continue;

		node = (struct briefs_trie_node *)bh->b_data;
		if (node->magic != BRIEFS_TRIE_MAGIC) {
			brelse(bh);
			continue;
		}

		if (emitted) {
			/*
			 * We've already emitted this node's leaf entry.
			 * Just push children (if any) for processing.
			 */
			goto push_children;
		}

		/*
		 * Emit leaf data if this node has any:
		 *   - NODE_STATUS_LEAF flag set (INTERM node with a leaf entry)
		 *   - Pure leaf (node_type is FILE/DIR, not INTERM)
		 */
		if ((node->node_type & NODE_STATUS_LEAF) ||
		    (node->node_type != NODE_TYPE_INTERM)) {
			char *src;
			int nlen;

			src = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - node->name_offset + 2;
			nlen = node->name_len - 2;

			if (nlen > 0 && nlen <= BRIEFS_NAME_LEN) {
				if (ino) *ino = node->inode;
				if (type) *type = TRIE_NODE_FTYPE(node);
				if (name_buf && name_len) {
					memcpy(name_buf, src, nlen);
					*name_len = nlen;
				}

				/*
				 * If this node also has children, push it back
				 * with leaf_emitted=1 so they are processed in
				 * subsequent calls without re-emitting the leaf.
				 */
				if (node->first_child && iter->sp < 256) {
					iter->stack[iter->sp] = block;
					iter->leaf_emitted[iter->sp] = 1;
					iter->sp++;
				}

				brelse(bh);
				return 0;
			}
		}

push_children:
		if (node->first_child) {
			u64 child = node->first_child;
			int pushed = 0;

			while (child) {
				if (iter->sp >= 510) {
					/*
					 * Stack full.  Push the current node back
					 * so we resume here, then push just this
					 * one child.  The remaining siblings will
					 * be picked up when we return to this node.
					 */
					iter->stack[iter->sp] = block;
					iter->leaf_emitted[iter->sp] = 1;
					iter->sp++;
					iter->stack[iter->sp] = child;
					iter->leaf_emitted[iter->sp] = 0;
					iter->sp++;
					pushed = 1;
					break;
				}

				iter->stack[iter->sp] = child;
				iter->leaf_emitted[iter->sp] = 0;
				iter->sp++;
				pushed++;

				struct buffer_head *cbh = sb_bread(sb, child);
				if (!cbh) break;
				struct briefs_trie_node *cn = (struct briefs_trie_node *)cbh->b_data;
				child = cn->next_sibling;
				brelse(cbh);
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
