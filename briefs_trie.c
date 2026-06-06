/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * BrieFS directory trie.
 *
 * Each directory has a trie structure stored in individual blocks.
 * The root block number is in inode->dir_trie_root.
 *
 * Trie layout:
 *   - Internal nodes (NODE_TYPE_INTERM) have child_count > 0 and dispatch
 *     by the byte value at the node's depth in the filename.
 *   - Leaf nodes (NODE_TYPE_FILE / NODE_TYPE_DIR) have child_count = 0
 *     and store the full filename + inode in the trailing bytes of the block.
 *   - Children at each depth level are linked via first_child / next_sibling.
 *   - The root node is special: depth=0, byte_val=0 represents the empty prefix.
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
	node->magic = 0x54524E20;
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
		if (cn->magic != 0x54524E20) {
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
		if (child == 0)
			return -ENOENT;

		bh = sb_bread(sb, child);
		if (!bh)
			return -EIO;

		node = (struct briefs_trie_node *)bh->b_data;
		if (node->magic != 0x54524E20) {
			brelse(bh);
			return -EIO;
		}

		if (node->node_type == NODE_TYPE_INTERM) {
			cur = child;
			brelse(bh);
			continue;
		}

		entry_name = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - node->name_offset;
		entry_name += 2;
		entry_len = node->name_len - 2;

		if (entry_len == name_len && memcmp(entry_name, name, name_len) == 0) {
			if (found_ino)
				*found_ino = node->inode;
			if (found_type)
				*found_type = node->node_type;
			brelse(bh);
			return 0;
		}

		brelse(bh);
		return -ENOENT;
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
	u8 leaf_type;
	int pos, stored_len, ret;

	if (!di->dir_trie_root) {
		ret = briefs_trie_create_root(sb, di);
		if (ret)
			return ret;
	}

	cur = di->dir_trie_root;

	if (name_len > TRIE_MAX_NAME_LEN)
		return -ENAMETOOLONG;

	for (pos = 0; pos < name_len; pos++) {
		u8 bval;

		bval = (u8)name[pos];
		leaf_type = type;

		if (pos == name_len - 1) {
			u64 existing;

			existing = briefs_trie_find_child(sb, cur, bval);
			if (existing) {
				cbh = sb_bread(sb, existing);
				if (cbh) {
					cnode = (struct briefs_trie_node *)cbh->b_data;
					if (cnode->node_type != NODE_TYPE_INTERM) {
						brelse(cbh);
						return -EEXIST;
					}
					brelse(cbh);
				}
			}

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
			node->magic = 0x54524E20;
			node->depth = pos;
			node->byte_val = bval;
			node->node_type = leaf_type;
			node->inode = ino;
			node->name_len = 2 + name_len;

			stored_len = 2 + name_len;
			{
				u16 name_off;
				char *name_dest;

				name_off = stored_len;
				name_dest = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - name_off;
				*(__u16 *)name_dest = (__u16)name_len;
				memcpy(name_dest + 2, name, name_len);
				node->name_offset = name_off;
			}

			mark_buffer_dirty(bh);
			sync_dirty_buffer(bh);
			brelse(bh);

			goto link_child;
		}

		child = briefs_trie_find_or_create_child(sb, cur, pos + 1, bval, NODE_TYPE_INTERM);
		if (child == 0)
			return -ENOSPC;

		cbh = sb_bread(sb, child);
		if (cbh) {
			cnode = (struct briefs_trie_node *)cbh->b_data;
			if (cnode->node_type != NODE_TYPE_INTERM) {
				u64 internal;
				u64 *link;

				internal = briefs_trie_alloc_node(sb);
				if (internal == 0) {
					brelse(cbh);
					return -ENOSPC;
				}
				briefs_trie_init_block(sb, internal, pos + 1, bval, NODE_TYPE_INTERM);

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

				bh = sb_bread(sb, internal);
				if (bh) {
					node = (struct briefs_trie_node *)bh->b_data;
					node->first_child = child;
					node->child_count = 1;
					mark_buffer_dirty(bh);
					sync_dirty_buffer(bh);
					brelse(bh);
				}

				cnode->depth = pos + 1;
				mark_buffer_dirty(cbh);
				sync_dirty_buffer(cbh);
				brelse(cbh);
				cur = internal;
			} else {
				brelse(cbh);
				cur = child;
			}
		} else {
			cur = child;
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
			u64 last;

			last = node->first_child;
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

			if (node->node_type == NODE_TYPE_INTERM) {
				brelse(cbh);
				brelse(bh);
				return -ENOTEMPTY;
			}

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
		if (node->magic != 0x54524E20) {
			brelse(bh);
			continue;
		}

		/* Push children onto stack for later processing */
		if (node->first_child) {
			/* Walk sibling chain, pushing each onto the stack.
			 * Entries will be processed in reverse-sibling order
			 * (last sibling first), but BrieFS does not guarantee
			 * readdir ordering. */
			u64 child = node->first_child;

			while (child && iter->sp < 256) {
				struct buffer_head *cbh;
				struct briefs_trie_node *cn;

				iter->stack[iter->sp++] = child;
				cbh = sb_bread(sb, child);
				if (!cbh) break;
				cn = (struct briefs_trie_node *)cbh->b_data;
				child = cn->next_sibling;
				brelse(cbh);
			}
		}

		/* If this is a leaf, emit it */
		if (node->node_type != NODE_TYPE_INTERM) {
			char *src;
			int nlen;

			src = (char *)bh->b_data + BRIEFS_BLOCK_SIZE - node->name_offset + 2;
			nlen = node->name_len - 2; /* subtract 2-byte length prefix */

			if (nlen > 0 && nlen <= BRIEFS_NAME_LEN) {
				if (ino) *ino = node->inode;
				if (type) *type = node->node_type;
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
