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
 *     node and may also have NODE_STATUS_LEAF if the root itself is a leaf.
 *
 * Name storage in leaf nodes:
 *   The full name is stored in the trailing bytes of the block, referenced by
 *   name_offset (offset from block end). The storage format is [len:2][name:N].
 */

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/buffer_head.h>

#include "briefs.h"

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
 * briefs_trie_init_block - initialize a freshly-allocated block as a trie node.
 */
static void briefs_trie_init_block(struct super_block *sb, u64 block,
                                    u8 depth, u8 byte_val, u8 node_type)
{
	struct buffer_head *bh;
	struct briefs_trie_node *node;

	bh = sb_getblk(sb, block);
	if (!bh)
		return;

	memset(bh->b_data, 0, sb->s_blocksize);
	node = (struct briefs_trie_node *)bh->b_data;
	node->magic = BRIEFS_TRIE_MAGIC;
	node->depth = depth;
	node->byte_val = byte_val;
	node->node_type = node_type;
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
}

/*
 * briefs_trie_create_root - create a root trie node for a directory.
 * Sets di->dir_trie_root to the allocated block number.
 * Returns 0 on success, negative on error.
 */
int briefs_trie_create_root(struct super_block *sb, struct briefs_inode *di)
{
	u64 block;
	int ret;

	block = briefs_trie_alloc_node(sb);
	if (block == 0) {
		ret = -ENOSPC;
		return ret;
	}

	briefs_trie_init_block(sb, block, 0, 0, NODE_TYPE_INTERM);
	di->dir_trie_root = block;
	return 0;
}

/*
 * briefs_trie_find_child - find a child node by byte value.
 * Returns the absolute block number of the child, or 0 if not found.
 */
static u64 briefs_trie_find_child(struct super_block *sb, u64 node_block, u8 byte_val)
{
	struct buffer_head *bh;
	struct briefs_trie_node *node;
	u64 child;

	bh = sb_bread(sb, node_block);
	if (!bh)
		return 0;

	node = (struct briefs_trie_node *)bh->b_data;
	child = node->first_child;

	while (child) {
		struct buffer_head *cbh;
		struct briefs_trie_node *cn;

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
	u64 cur;
	int pos;

	if (!di->dir_trie_root)
		return -ENOENT;

	cur = di->dir_trie_root;

	for (pos = 0; pos < name_len; pos++) {
		struct buffer_head *bh;
		struct briefs_trie_node *node;
		u64 child;
		char *entry_name;
		int entry_len;

		child = briefs_trie_find_child(sb, cur, (u8)name[pos]);
		if (child == 0) {
			/*
			 * No child at this byte.  The current node `cur`
			 * might have NODE_STATUS_LEAF set if a prefix entry
			 * exists here.
			 */
			goto check_prefix;
		}

		bh = sb_bread(sb, child);
		if (!bh)
			return -EIO;

		node = (struct briefs_trie_node *)bh->b_data;
		if (node->magic != BRIEFS_TRIE_MAGIC) {
			brelse(bh);
			return -EIO;
		}

		if (node->node_type & NODE_TYPE_INTERM) {
			/*
			 * INTERM node — descend to children for the next
			 * byte.  But first check if this node also has
			 * NODE_STATUS_LEAF (prefix entry that shares this
			 * byte and all preceding bytes).
			 */
			if (node->node_type & NODE_STATUS_LEAF) {
				entry_name = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - node->name_offset + 2;
				entry_len = node->name_len - 2;
				if (entry_len == name_len && memcmp(entry_name, name, name_len) == 0) {
					if (found_ino)
						*found_ino = node->inode;
					if (found_type)
						*found_type = TRIE_NODE_FTYPE(node);
					brelse(bh);
					return 0;
				}
			}
			cur = child;
			brelse(bh);
			continue;
		}

		/* Pure leaf node — check full name match */
		entry_name = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - node->name_offset;
		entry_name += 2;
		entry_len = node->name_len - 2;

		if (entry_len == name_len && memcmp(entry_name, name, name_len) == 0) {
			if (found_ino)
				*found_ino = node->inode;
			if (found_type)
				*found_type = TRIE_NODE_FTYPE(node);
			brelse(bh);
			return 0;
		}

		brelse(bh);
		return -ENOENT;
	}

	/*
	 * All name_len bytes consumed without finding a child.
	 * The last node at `cur` may have NODE_STATUS_LEAF set —
	 * a prefix entry (e.g. "file_1" when "file_10" split at '0').
	 */
check_prefix:
	{
		struct buffer_head *nbh = sb_bread(sb, cur);
		if (nbh) {
			struct briefs_trie_node *nnode;
			nnode = (struct briefs_trie_node *)nbh->b_data;
			if (nnode->node_type & NODE_STATUS_LEAF) {
				char *zname;
				int zlen;

				zname = (char *)nbh->b_data + BRIEFS_BLOCK_SIZE - nnode->name_offset + 2;
				zlen = nnode->name_len - 2;

				if (zlen == name_len && memcmp(zname, name, name_len) == 0) {
					if (found_ino) *found_ino = nnode->inode;
					if (found_type) *found_type = TRIE_NODE_FTYPE(nnode);
					brelse(nbh);
					return 0;
				}
			}
			brelse(nbh);
		}
	}

	return -ENOENT;
}

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
 * Returns 0 on success, -ENOENT if not found.
 */
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len)
{
	struct buffer_head *bh, *cbh;
	struct briefs_trie_node *node, *pnode;
	u64 cur, parent, child_prev, child;
	int pos;

	if (!di->dir_trie_root)
		return -ENOENT;

	parent = 0;
	child_prev = 0;
	cur = di->dir_trie_root;

	for (pos = 0; pos < name_len; pos++) {
		bh = sb_bread(sb, cur);
		if (!bh)
			return -EIO;
		pnode = (struct briefs_trie_node *)bh->b_data;
		child = pnode->first_child;
		child_prev = 0;

		while (child) {
			struct buffer_head *tbh;
			struct briefs_trie_node *tnode;

			tbh = sb_bread(sb, child);
			if (!tbh) {
				brelse(bh);
				return -EIO;
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
			return -ENOENT;
		}

		if (pos == name_len - 1) {
			cbh = sb_bread(sb, child);
			if (!cbh) {
				brelse(bh);
				return -EIO;
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
				return -ENOTEMPTY;
			}

			/*
			 * Found the node.  Unlink it from the parent's
			 * sibling chain.  If it's an INTERM with
			 * NODE_STATUS_LEAF, just clear the leaf flag
			 * — don't free the node, it still has children.
			 * If it's a pure leaf, unlink and free.
			 */
			if (node->node_type & NODE_TYPE_INTERM) {
				/* INTERM with NODE_STATUS_LEAF: clear leaf flag */
				node->node_type &= ~NODE_STATUS_LEAF;
				mark_buffer_dirty(cbh);
				sync_dirty_buffer(cbh);
				brelse(cbh);
				brelse(bh);
				return 0;
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
			brelse(bh);

			briefs_trie_free_node(sb, child);
			return 0;
		}

		parent = cur;
		child_prev = 0;
		cur = child;
		brelse(bh);
	}

	return -ENOENT;
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
void briefs_trie_iter_init(struct trie_iter *iter, struct briefs_inode *di)
{
	iter->sp = 0;
	if (di->dir_trie_root) {
		iter->stack[0] = di->dir_trie_root;
		iter->sp = 1;
	}
}

/*
 * briefs_trie_iter_next - get the next leaf entry from the trie.
 * Returns 0 if an entry was found, -ENOENT if iteration is complete.
 */
int briefs_trie_iter_next(struct super_block *sb, struct trie_iter *iter,
                                  u64 *ino, u8 *type, char *name_buf, int *name_len)
{
	struct buffer_head *bh;
	struct briefs_trie_node *node;

	while (iter->sp > 0) {
		u64 block;

		block = iter->stack[iter->sp - 1];
		iter->sp--;

		bh = sb_bread(sb, block);
		if (!bh)
			continue;

		node = (struct briefs_trie_node *)bh->b_data;
		if (node->magic != BRIEFS_TRIE_MAGIC) {
			brelse(bh);
			continue;
		}

		/*
		 * Push children onto the stack for later processing.
		 * If the stack is too full to hold all children at once,
		 * push the current node back and process one child at a time.
		 */
		if (node->first_child) {
			u64 child = node->first_child;
			int pushed = 0;

			while (child) {
				if (iter->sp >= 256) {
					/*
					 * Stack full.  Push the current node back
					 * so we resume here, then push just this
					 * one child.  The remaining siblings will
					 * be picked up when we return to this node.
					 */
					iter->stack[iter->sp++] = block;
					iter->stack[iter->sp++] = child;
					pushed = 1;
					break;
				}

				iter->stack[iter->sp++] = child;
				pushed++;

				struct buffer_head *cbh = sb_bread(sb, child);
				if (!cbh) break;
				struct briefs_trie_node *cn = (struct briefs_trie_node *)cbh->b_data;
				child = cn->next_sibling;
				brelse(cbh);
			}

			if (pushed) {
				/*
				 * We pushed children (or one child + ourselves).
				 * Don't emit this node yet — we'll come back to
				 * it after processing children.  The children are
				 * on top of the stack, so they'll be processed first.
				 */
				brelse(bh);
				continue;
			}
		}

		/*
		 * No children (or all children already processed).
		 * Emit this node if it has leaf data:
		 *   - NODE_STATUS_LEAF flag set (INTERM with leaf entry)
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
				brelse(bh);
				return 0;
			}
		}

		brelse(bh);
	}

	return -ENOENT;
}
