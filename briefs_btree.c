// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * Offset-keyed B+ tree extent index.
 *
 * Replaces the flat singly-linked chain list. The tree is keyed by
 * briefs_disk_extent.offset (logical block number); every leaf is maintained
 * sorted by offset, and internal nodes carry separator keys (the min key of
 * each child's right neighbor) plus absolute child block pointers. Leaves are
 * threaded left-to-right by next_leaf for O(E) in-order traversal.
 *
 * Inode states:
 *   inline-only : InodeFlagIndexed clear, extent_inline_base == 0, up to 8
 *                 extents kept sorted in inline_extents[].
 *   tree-backed  : InodeFlagIndexed set, extent_inline_base == root block,
 *                 num_extents_inline == 0, all extents live in the tree.
 * The 9th extent (inline full, no merge) spills inline->tree.
 *
 * Insertion uses proactive (top-down) splitting: before descending into a full
 * child, split it first and absorb its separator into the (guaranteed non-full)
 * parent. The root is split the same way, growing the tree height. This means
 * the leaf we finally insert into always has room, so the insert itself never
 * needs to allocate or split — every block allocation (one per split) happens
 * BEFORE any extent is added. An allocation failure (ENOSPC) therefore leaves a
 * consistent tree: the splits done so far are linked into their parents, lower
 * nodes are untouched, and the extent was never added. No rollback is needed.
 *
 * Crash-safety: every split mark_buffer_dirty()s the old-root-as-child, the new
 * sibling, and (for a root split) the new root BEFORE the new root pointer is
 * published under extent_seq. The lock-free briefs_btree_drain() then syncs every
 * reachable dirty node before the JRN_INODE_FULL snapshot, so the snapshot never
 * references a tree block that is not yet on disk.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/seqlock.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"
#include "briefs_debug.h"

/* Result of a node split, propagated up so the parent can absorb the separator.
 * For a leaf split, separator is the min key of the right half. For an internal
 * split, separator is the median key pushed up. */
struct btree_split {
	bool happened;
	u64 sibling;      /* absolute block of the new right half */
	u64 separator;    /* key pushed up to the parent */
};

/* Read a B-tree node, verifying magic + checksum. trust_verified lets locked
 * callers skip the CRC on a buffer whose BH_Verified bit is set (no concurrent
 * modifier can have torn it). Returns the buffer_head (caller brelse) and a
 * pointer to the node, or NULL on failure. */
static struct briefs_extent_btree_node *
btree_read_node(struct super_block *sb, u64 block, bool trust_verified,
		 struct buffer_head **bhp)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_err("briefs: btree: failed to read node %llu\n", block);
		return NULL;
	}
	node = (struct briefs_extent_btree_node *)bh->b_data;

	if (!trust_verified || !buffer_verified(bh)) {
		if (le32_to_cpu(node->hdr.magic) != BRIEFS_BTREE_MAGIC) {
			pr_err("briefs: btree: node %llu bad magic 0x%08x\n",
			       block, le32_to_cpu(node->hdr.magic));
			brelse(bh);
			return NULL;
		}
		if (briefs_verify_chain_checksum(bh->b_data, node->checksum) != 0) {
			pr_err("briefs: btree: node %llu checksum mismatch\n",
			       block);
			brelse(bh);
			return NULL;
		}
		if (trust_verified)
			set_buffer_verified(bh);
	}

	*bhp = bh;
	return node;
}

/* Recompute a node's checksum, memoize it, and mark the buffer dirty. */
static void btree_commit_node(struct buffer_head *bh)
{
	struct briefs_extent_btree_node *node =
		(struct briefs_extent_btree_node *)bh->b_data;

	node->checksum = cpu_to_le64(briefs_chain_checksum(bh->b_data));
	set_buffer_verified(bh);
	mark_buffer_dirty(bh);
}

/* Allocate a fresh zeroed node buffer (caller fills header + payload, then
 * btree_commit_node). Returns the buffer_head or NULL. */
static struct buffer_head *btree_new_node(struct super_block *sb, u64 block)
{
	return briefs_get_zero_block(sb, block);
}

/* Leaf test. */
static inline bool btree_node_is_leaf(const struct briefs_extent_btree_node *n)
{
	return le32_to_cpu(n->hdr.flags) & BRIEFS_BTREE_LEAF;
}

/* Find the child index (0..num_keys) of an internal node whose subtree holds
 * @key. index == num_keys means the trailing_child. */
static int btree_internal_find_child(const struct briefs_extent_btree_node *n,
				     u64 key)
{
	int i, num_keys = le16_to_cpu(n->hdr.num_keys);

	for (i = 0; i < num_keys; i++) {
		if (key < le64_to_cpu(n->u.internal.idx[i].high_key))
			return i;
	}
	return num_keys;
}

/* First index in a leaf whose extent.offset > @offset (= the insertion slot). */
static int btree_leaf_find_pos(const struct briefs_extent_btree_node *n,
				u64 offset)
{
	int i, num_keys = le16_to_cpu(n->hdr.num_keys);

	for (i = 0; i < num_keys; i++) {
		if (le64_to_cpu(n->u.leaf.extents[i].offset) > offset)
			return i;
	}
	return num_keys;
}

/* Binary-search-free lookup: descend the tree for the extent covering @iblock.
 * Returns 0 and fills *ext, -ENOENT, or -EIO. */
static int btree_lookup_block(struct super_block *sb, u64 block, u64 iblock,
			      struct briefs_extent *ext, bool trust_verified)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int ret;

	node = btree_read_node(sb, block, trust_verified, &bh);
	if (!node)
		return -EIO;

	if (btree_node_is_leaf(node)) {
		int i, num_keys = le16_to_cpu(node->hdr.num_keys);

		for (i = 0; i < num_keys; i++) {
			struct briefs_disk_extent *de = &node->u.leaf.extents[i];
			u64 off = le64_to_cpu(de->offset);
			u64 len = le64_to_cpu(de->len);

			if (iblock >= off && iblock < off + len) {
				briefs_disk_extent_to_cpu(de, ext);
				brelse(bh);
				return 0;
			}
			if (off > iblock)
				break;	/* sorted: no later extent can cover it */
		}
		brelse(bh);
		return -ENOENT;
	}

	{
		int p = btree_internal_find_child(node, iblock);
		u64 child = (p < le16_to_cpu(node->hdr.num_keys))
			    ? le64_to_cpu(node->u.internal.idx[p].child)
			    : le64_to_cpu(node->u.internal.trailing_child);
		brelse(bh);
		if (child == 0) {
			pr_err("briefs: btree: internal node %llu has null child\n",
			       block);
			return -EIO;
		}
		ret = btree_lookup_block(sb, child, iblock, ext, trust_verified);
		return ret;
	}
}

int briefs_btree_lookup(struct super_block *sb, u64 root_block, u64 iblock,
			struct briefs_extent *ext, bool trust_verified)
{
	if (root_block == 0)
		return -ENOENT;
	return btree_lookup_block(sb, root_block, iblock, ext, trust_verified);
}

/* ---------- insertion ---------- */

/* Absorb a child split into parent @node_bh at child position @p. The separator
 * becomes the new upper bound of the left (existing) child; the sibling becomes
 * a new child to its right, inheriting the left child's old upper bound. */
static void btree_absorb_split(struct briefs_extent_btree_node *parent,
			       struct buffer_head *parent_bh, int p,
			       u64 sibling, u64 separator)
{
	int num_keys = le16_to_cpu(parent->hdr.num_keys);

	if (p == num_keys) {
		/* The split child was the trailing child. It gains an upper
		 * bound (separator) and becomes an idx entry; the sibling
		 * becomes the new trailing child. */
		parent->u.internal.idx[p].child =
			cpu_to_le64(le64_to_cpu(parent->u.internal.trailing_child));
		parent->u.internal.idx[p].high_key = cpu_to_le64(separator);
		parent->u.internal.trailing_child = cpu_to_le64(sibling);
	} else {
		u64 old_high = le64_to_cpu(parent->u.internal.idx[p].high_key);

		/* Shift the tail right to make room for the new entry. */
		memmove(&parent->u.internal.idx[p + 2],
			&parent->u.internal.idx[p + 1],
			(size_t)(num_keys - p - 1) * sizeof(parent->u.internal.idx[0]));
		parent->u.internal.idx[p].high_key = cpu_to_le64(separator);
		parent->u.internal.idx[p + 1].child = cpu_to_le64(sibling);
		parent->u.internal.idx[p + 1].high_key = cpu_to_le64(old_high);
	}

	parent->hdr.num_keys = cpu_to_le16(num_keys + 1);
	btree_commit_node(parent_bh);
}

/* If the child at position @p of @parent is full, split it and absorb the
 * separator into @parent (which is guaranteed non-full). Returns 0, or -ENOSPC
 * if the sibling block could not be allocated (in which case nothing is
 * modified). */
static int btree_maybe_split_child(struct super_block *sb,
				   struct briefs_inode *di,
				   struct briefs_extent_btree_node *parent,
				   struct buffer_head *parent_bh, int p)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	int num_keys = le16_to_cpu(parent->hdr.num_keys);
	u64 child_block;
	struct buffer_head *child_bh, *sib_bh;
	struct briefs_extent_btree_node *child, *sib;
	u64 sib_block, rel;
	bool is_leaf;
	int child_n, mid, move;

	child_block = (p < num_keys)
		? le64_to_cpu(parent->u.internal.idx[p].child)
		: le64_to_cpu(parent->u.internal.trailing_child);

	child = btree_read_node(sb, child_block, true, &child_bh);
	if (!child)
		return -EIO;

	is_leaf = btree_node_is_leaf(child);
	child_n = le16_to_cpu(child->hdr.num_keys);

	if (is_leaf) {
		if (child_n < BRIEFS_BTREE_LEAF_FANOUT)
			goto no_split;	/* has room */
	} else {
		if (child_n < BRIEFS_BTREE_IDX_KEYS)
			goto no_split;	/* has room */
	}

	/* Full: split. Allocate the sibling first; on failure nothing is
	 * modified (clean ENOSPC). */
	briefs_stat_inc(bsi, btree_splits);
	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0) {
		brelse(child_bh);
		return -ENOSPC;
	}
	sib_block = data_to_abs(bsi->sb, rel);

	sib_bh = btree_new_node(sb, sib_block);
	if (!sib_bh) {
		briefs_free_block(&bsi->alloc, rel);
		brelse(child_bh);
		return -EIO;
	}
	sib = (struct briefs_extent_btree_node *)sib_bh->b_data;

	if (is_leaf) {
		/* Split 126 into 63 + 63. */
		mid = BRIEFS_BTREE_LEAF_FANOUT / 2;	/* 63 */
		move = child_n - mid;			/* 63 */

		/* Right half -> sibling. */
		sib->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
		sib->hdr.flags = cpu_to_le32(BRIEFS_BTREE_LEAF);
		sib->hdr.level = child->hdr.level;
		sib->hdr.num_keys = cpu_to_le16(move);
		sib->hdr.next_leaf = child->hdr.next_leaf;
		memcpy(sib->u.leaf.extents, &child->u.leaf.extents[mid],
		       (size_t)move * sizeof(child->u.leaf.extents[0]));

		/* Left half stays in place (just truncate the count + link). */
		child->hdr.num_keys = cpu_to_le16(mid);
		child->hdr.next_leaf = cpu_to_le64(sib_block);

		btree_commit_node(child_bh);
		btree_commit_node(sib_bh);

		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, sib_block, 1, -1);

		btree_absorb_split(parent, parent_bh, p, sib_block,
				   le64_to_cpu(sib->u.leaf.extents[0].offset));
	} else {
		/* Internal split: push the median separator up. With 253 keys
		 * (idx[0..252], 254 children), median index = 126. Left keeps
		 * idx[0..125] + trailing = idx[126].child; right gets idx[127..252]
		 * reindexed + the old trailing_child; idx[126].high_key is pushed. */
		mid = BRIEFS_BTREE_IDX_KEYS / 2;	/* 126 */
		move = child_n - mid - 1;		/* 126 */

		sib->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
		sib->hdr.flags = cpu_to_le32(0);
		sib->hdr.level = child->hdr.level;
		sib->hdr.num_keys = cpu_to_le16(move);
		sib->hdr.next_leaf = cpu_to_le64(0);
		memcpy(sib->u.internal.idx, &child->u.internal.idx[mid + 1],
		       (size_t)move * sizeof(child->u.internal.idx[0]));
		sib->u.internal.trailing_child = child->u.internal.trailing_child;

		/* Left half: truncate to idx[0..mid-1], trailing = old idx[mid].child. */
		child->u.internal.trailing_child =
			child->u.internal.idx[mid].child;
		child->hdr.num_keys = cpu_to_le16(mid);

		btree_commit_node(child_bh);
		btree_commit_node(sib_bh);

		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, sib_block, 1, -1);

		btree_absorb_split(parent, parent_bh, p, sib_block,
				   le64_to_cpu(child->u.internal.idx[mid].high_key));
	}

	brelse(sib_bh);
	brelse(child_bh);
	return 0;

no_split:
	brelse(child_bh);
	return 0;
}

/* Insert into a leaf that is guaranteed non-full. Handles merge-with-neighbor
 * and sorted insert. Sets *added = false on merge, true on insert. */
static int btree_leaf_insert(struct super_block *sb, struct briefs_inode *di,
			     struct buffer_head *bh,
			     struct briefs_extent_btree_node *node,
			     const struct briefs_extent *ext, bool *added)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	int n = le16_to_cpu(node->hdr.num_keys);
	int pos = btree_leaf_find_pos(node, ext->offset);
	struct briefs_disk_extent *ents = node->u.leaf.extents;

	/* Merge with the left neighbor (extent just before the insertion slot). */
	if (pos > 0) {
		struct briefs_extent left;

		briefs_disk_extent_to_cpu(&ents[pos - 1], &left);
		if (left.offset + left.len == ext->offset &&
		    left.phys + left.len == ext->phys) {
			left.len += ext->len;
			briefs_cpu_extent_to_disk(&left, &ents[pos - 1]);
			btree_commit_node(bh);
			briefs_journal_extent_alloc(bsi->journal,
						    di->inode_number, ext->offset,
						    ext->phys, ext->len, -1);
			*added = false;
			return 0;
		}
	}

	/* Merge with the right neighbor. */
	if (pos < n) {
		struct briefs_extent right;

		briefs_disk_extent_to_cpu(&ents[pos], &right);
		if (ext->offset + ext->len == right.offset &&
		    ext->phys + ext->len == right.phys) {
			right.offset = ext->offset;
			right.phys = ext->phys;
			right.len += ext->len;
			briefs_cpu_extent_to_disk(&right, &ents[pos]);
			btree_commit_node(bh);
			briefs_journal_extent_alloc(bsi->journal,
						    di->inode_number, ext->offset,
						    ext->phys, ext->len, -1);
			*added = false;
			return 0;
		}
	}

	/* Plain sorted insert. */
	memmove(&ents[pos + 1], &ents[pos],
		(size_t)(n - pos) * sizeof(ents[0]));
	briefs_cpu_extent_to_disk(ext, &ents[pos]);
	node->hdr.num_keys = cpu_to_le16(n + 1);
	btree_commit_node(bh);
	briefs_journal_extent_alloc(bsi->journal, di->inode_number, ext->offset,
				    ext->phys, ext->len, -1);
	*added = true;
	return 0;
}

/* Descend from @block (guaranteed non-full) inserting @ext, proactively
 * splitting full children before entering them. */
static int btree_descend_insert(struct super_block *sb, struct briefs_inode *di,
				u64 block, const struct briefs_extent *ext,
				bool *added)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int ret, p, num_keys;
	u64 child_block;

	node = btree_read_node(sb, block, true, &bh);
	if (!node)
		return -EIO;

	if (btree_node_is_leaf(node)) {
		ret = btree_leaf_insert(sb, di, bh, node, ext, added);
		brelse(bh);
		return ret;
	}

	num_keys = le16_to_cpu(node->hdr.num_keys);
	p = btree_internal_find_child(node, ext->offset);

	/* Proactive: split the child if full, absorbing the separator here. */
	ret = btree_maybe_split_child(sb, di, node, bh, p);
	if (ret) {
		brelse(bh);
		return ret;
	}

	/* Re-find the child (a split may have routed ext->offset to the sibling). */
	num_keys = le16_to_cpu(node->hdr.num_keys);
	p = btree_internal_find_child(node, ext->offset);
	child_block = (p < num_keys)
		? le64_to_cpu(node->u.internal.idx[p].child)
		: le64_to_cpu(node->u.internal.trailing_child);
	brelse(bh);

	if (child_block == 0)
		return -EIO;
	return btree_descend_insert(sb, di, child_block, ext, added);
}

/* Ensure the root has room for one more descent. If it is full, split it and
 * grow the tree height, updating di->extent_inline_base under extent_seq.
 * Returns 0, -ENOSPC (nothing modified), or -EIO. */
static int btree_ensure_root_room(struct super_block *sb, struct briefs_inode *di)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *root_bh, *sib_bh, *newroot_bh;
	struct briefs_extent_btree_node *root, *sib, *newroot;
	u64 root_block = di->extent_inline_base, sib_block, newroot_block, rel, rel2;
	u64 separator;
	bool is_leaf;
	int n, mid, move;

	root = btree_read_node(sb, root_block, true, &root_bh);
	if (!root)
		return -EIO;

	is_leaf = btree_node_is_leaf(root);
	n = le16_to_cpu(root->hdr.num_keys);

	if (is_leaf ? (n < BRIEFS_BTREE_LEAF_FANOUT) : (n < BRIEFS_BTREE_IDX_KEYS)) {
		brelse(root_bh);
		return 0;	/* not full */
	}

	/* Full root: split into left (in place) + right (sib) + a new internal
	 * root. Two allocations; both must succeed before we modify anything. */
	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0) {
		brelse(root_bh);
		return -ENOSPC;
	}
	sib_block = data_to_abs(bsi->sb, rel);

	rel2 = briefs_alloc_block(&bsi->alloc);
	if (rel2 == 0) {
		briefs_free_block(&bsi->alloc, rel);
		brelse(root_bh);
		return -ENOSPC;
	}
	newroot_block = data_to_abs(bsi->sb, rel2);

	sib_bh = btree_new_node(sb, sib_block);
	newroot_bh = btree_new_node(sb, newroot_block);
	if (!sib_bh || !newroot_bh) {
		if (sib_bh)
			brelse(sib_bh);
		if (newroot_bh)
			brelse(newroot_bh);
		briefs_free_block(&bsi->alloc, rel);
		briefs_free_block(&bsi->alloc, rel2);
		brelse(root_bh);
		return -EIO;
	}
	sib = (struct briefs_extent_btree_node *)sib_bh->b_data;
	newroot = (struct briefs_extent_btree_node *)newroot_bh->b_data;

	if (is_leaf) {
		mid = BRIEFS_BTREE_LEAF_FANOUT / 2;	/* 63 */
		move = n - mid;				/* 63 */

		sib->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
		sib->hdr.flags = cpu_to_le32(BRIEFS_BTREE_LEAF);
		sib->hdr.level = root->hdr.level;
		sib->hdr.num_keys = cpu_to_le16(move);
		sib->hdr.next_leaf = root->hdr.next_leaf;
		memcpy(sib->u.leaf.extents, &root->u.leaf.extents[mid],
		       (size_t)move * sizeof(root->u.leaf.extents[0]));

		root->hdr.num_keys = cpu_to_le16(mid);
		root->hdr.next_leaf = cpu_to_le64(sib_block);

		separator = le64_to_cpu(sib->u.leaf.extents[0].offset);
	} else {
		mid = BRIEFS_BTREE_IDX_KEYS / 2;	/* 126 */
		move = n - mid - 1;			/* 126 */

		sib->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
		sib->hdr.flags = cpu_to_le32(0);
		sib->hdr.level = root->hdr.level;
		sib->hdr.num_keys = cpu_to_le16(move);
		sib->hdr.next_leaf = cpu_to_le64(0);
		memcpy(sib->u.internal.idx, &root->u.internal.idx[mid + 1],
		       (size_t)move * sizeof(root->u.internal.idx[0]));
		sib->u.internal.trailing_child = root->u.internal.trailing_child;

		root->u.internal.trailing_child = root->u.internal.idx[mid].child;
		root->hdr.num_keys = cpu_to_le16(mid);

		separator = le64_to_cpu(root->u.internal.idx[mid].high_key);
	}

	btree_commit_node(root_bh);
	btree_commit_node(sib_bh);

	/* New internal root, one level above the old root. */
	newroot->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
	newroot->hdr.flags = cpu_to_le32(0);
	newroot->hdr.level = cpu_to_le16(le16_to_cpu(root->hdr.level) + 1);
	newroot->hdr.num_keys = cpu_to_le16(1);
	newroot->hdr.next_leaf = cpu_to_le64(0);
	newroot->u.internal.idx[0].child = cpu_to_le64(root_block);
	newroot->u.internal.idx[0].high_key = cpu_to_le64(separator);
	newroot->u.internal.trailing_child = cpu_to_le64(sib_block);
	btree_commit_node(newroot_bh);

	briefs_journal_extent_alloc(bsi->journal, di->inode_number,
				    0, sib_block, 1, -1);
	briefs_journal_extent_alloc(bsi->journal, di->inode_number,
				    0, newroot_block, 1, -1);

	/* Publish the new root last: root, sibling, and new root are all dirty
	 * on disk (or will be via the drain) before the snapshot can reference
	 * newroot_block. */
	write_seqcount_begin(&binfo->extent_seq);
	di->extent_inline_base = newroot_block;
	write_seqcount_end(&binfo->extent_seq);

	brelse(sib_bh);
	brelse(newroot_bh);
	brelse(root_bh);
	return 0;
}

/* Spill inline->tree: build a single root leaf from the sorted+merged inline
 * extents plus the new one. Caller verified the inline array is full (8) and no
 * inline merge applies. */
static int btree_spill_inline(struct super_block *sb, struct briefs_inode *di,
			      const struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct briefs_extent merged[9];
	struct briefs_extent tmp[9];
	int i, n = 0, m;
	u64 rel, root_block;
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;

	/* Collect the 8 inline extents + the new one. */
	for (i = 0; i < di->num_extents_inline; i++)
		tmp[n++] = di->inline_extents[i];
	tmp[n++] = *ext;

	/* Sort by offset (insertion sort, n <= 9). */
	for (i = 1; i < n; i++) {
		struct briefs_extent k = tmp[i];
		int j = i - 1;
		while (j >= 0 && tmp[j].offset > k.offset) {
			tmp[j + 1] = tmp[j];
			j--;
		}
		tmp[j + 1] = k;
	}

	/* Merge contiguous extents (same offset/phys adjacency). */
	m = 0;
	for (i = 0; i < n; i++) {
		if (m > 0 &&
		    merged[m - 1].offset + merged[m - 1].len == tmp[i].offset &&
		    merged[m - 1].phys + merged[m - 1].len == tmp[i].phys) {
			merged[m - 1].len += tmp[i].len;
		} else {
			merged[m++] = tmp[i];
		}
	}

	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return -ENOSPC;
	root_block = data_to_abs(bsi->sb, rel);

	bh = btree_new_node(sb, root_block);
	if (!bh) {
		briefs_free_block(&bsi->alloc, rel);
		return -EIO;
	}
	node = (struct briefs_extent_btree_node *)bh->b_data;

	node->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
	node->hdr.flags = cpu_to_le32(BRIEFS_BTREE_LEAF);
	node->hdr.level = cpu_to_le16(0);
	node->hdr.num_keys = cpu_to_le16(m);
	node->hdr.next_leaf = cpu_to_le64(0);
	for (i = 0; i < m; i++)
		briefs_cpu_extent_to_disk(&merged[i], &node->u.leaf.extents[i]);
	btree_commit_node(bh);
	brelse(bh);

	briefs_journal_extent_alloc(bsi->journal, di->inode_number, 0,
				    root_block, 1, -1);
	/* The new extent's data blocks: journal for bitmap recovery. */
	briefs_journal_extent_alloc(bsi->journal, di->inode_number, ext->offset,
				    ext->phys, ext->len, -1);

	write_seqcount_begin(&binfo->extent_seq);
	memset(di->inline_extents, 0, sizeof(di->inline_extents));
	di->flags |= InodeFlagIndexed;
	di->extent_inline_base = root_block;
	di->num_extents_inline = 0;
	di->num_extents_total = m;
	write_seqcount_end(&binfo->extent_seq);

	return 0;
}

/* Inline-only sorted insert + merge. Returns 0, -ENOSPC (only via spill), or
 * -EIO. *spill_needed is set true when the inline array is full and the caller
 * must spill. */
static int btree_inline_insert(struct super_block *sb, struct briefs_inode *di,
			       const struct briefs_extent *ext, bool *spill_needed)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_sb_info *bsi = sb->s_fs_info;
	int n = di->num_extents_inline;
	int pos, i;

	*spill_needed = false;

	/* Find the insertion slot (first index with offset > ext->offset). */
	for (pos = 0; pos < n; pos++)
		if (di->inline_extents[pos].offset > ext->offset)
			break;

	/* Merge with the left neighbor. */
	if (pos > 0) {
		struct briefs_extent *left = &di->inline_extents[pos - 1];
		if (left->offset + left->len == ext->offset &&
		    left->phys + left->len == ext->phys) {
			left->len += ext->len;
			briefs_journal_extent_alloc(bsi->journal,
						    di->inode_number, ext->offset,
						    ext->phys, ext->len, -1);
			return 0;
		}
	}

	/* Merge with the right neighbor. */
	if (pos < n) {
		struct briefs_extent *right = &di->inline_extents[pos];
		if (ext->offset + ext->len == right->offset &&
		    ext->phys + ext->len == right->phys) {
			right->offset = ext->offset;
			right->phys = ext->phys;
			right->len += ext->len;
			briefs_journal_extent_alloc(bsi->journal,
						    di->inode_number, ext->offset,
						    ext->phys, ext->len, -1);
			return 0;
		}
	}

	/* No merge: need a new slot. */
	if (n >= 8) {
		*spill_needed = true;
		return 0;
	}

	write_seqcount_begin(&binfo->extent_seq);
	for (i = n; i > pos; i--)
		di->inline_extents[i] = di->inline_extents[i - 1];
	di->inline_extents[pos] = *ext;
	di->num_extents_inline = n + 1;
	di->num_extents_total++;
	write_seqcount_end(&binfo->extent_seq);

	briefs_journal_extent_alloc(bsi->journal, di->inode_number, ext->offset,
				    ext->phys, ext->len, pos);
	return 0;
}

int briefs_btree_insert_locked(struct super_block *sb, struct briefs_inode *di,
				struct briefs_extent *ext)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	bool added, spill;
	int ret;

	briefs_stat_inc(briefs_sb(sb), btree_extents_added);

	/* Maintain the extent-tail cache. For both merge and insert, the new
	 * extent's end is the only value that can raise the running max. */
	{
		u64 new_end = ext->offset + ext->len;
		write_seqcount_begin(&binfo->extent_seq);
		if (new_end > binfo->cached_max_end)
			binfo->cached_max_end = new_end;
		write_seqcount_end(&binfo->extent_seq);
	}

	if (di->flags & InodeFlagIndexed) {
		/* Tree-backed. */
		ret = btree_ensure_root_room(sb, di);
		if (ret)
			return ret;
		added = false;
		ret = btree_descend_insert(sb, di, di->extent_inline_base, ext,
					   &added);
		if (ret)
			return ret;
		if (added) {
			write_seqcount_begin(&binfo->extent_seq);
			di->num_extents_total++;
			write_seqcount_end(&binfo->extent_seq);
		}
		return 0;
	}

	/* Inline-only. */
	ret = btree_inline_insert(sb, di, ext, &spill);
	if (ret)
		return ret;
	if (spill)
		return btree_spill_inline(sb, di, ext);
	return 0;
}

/* ---------- iteration ---------- */

/* Recursive in-order descent of the subtree rooted at @block, calling @cb for
 * each leaf extent in ascending offset order. Visits children via the internal
 * idx array + trailing_child -- NOT next_leaf -- so leaves freed and dropped
 * from their parent during a range delete (whose next_leaf link would dangle at
 * a freed/reused block) are never visited. @depth bounds recursion as a guard
 * against a corrupt/cyclic tree. Stops early if @cb returns negative. */
static int btree_walk_descend(struct super_block *sb, u64 block, int depth,
			      int (*cb)(const struct briefs_extent *, void *),
			      void *ctx)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int i, n, ret;

	if (block == 0)
		return 0;
	if (depth > 16)
		return -EIO;

	node = btree_read_node(sb, block, false, &bh);
	if (!node)
		return -EIO;

	if (btree_node_is_leaf(node)) {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++) {
			struct briefs_extent ext;
			briefs_disk_extent_to_cpu(&node->u.leaf.extents[i], &ext);
			ret = cb(&ext, ctx);
			if (ret < 0) {
				brelse(bh);
				return ret;
			}
		}
		brelse(bh);
		return 0;
	}

	n = le16_to_cpu(node->hdr.num_keys);
	for (i = 0; i < n; i++) {
		ret = btree_walk_descend(sb,
				le64_to_cpu(node->u.internal.idx[i].child),
				depth + 1, cb, ctx);
		if (ret < 0) {
			brelse(bh);
			return ret;
		}
	}
	ret = btree_walk_descend(sb,
			le64_to_cpu(node->u.internal.trailing_child),
			depth + 1, cb, ctx);
	brelse(bh);
	return ret;
}

int briefs_btree_for_each_extent(struct super_block *sb, struct briefs_inode *di,
				 int (*cb)(const struct briefs_extent *, void *),
				 void *ctx)
{
	int i, ret;

	/* Inline-data inodes have no extents. */
	if (di->flags & InodeFlagInlineData)
		return 0;

	if (!(di->flags & InodeFlagIndexed)) {
		/* Inline-only: walk the inline array (already sorted). */
		for (i = 0; i < di->num_extents_inline; i++) {
			ret = cb(&di->inline_extents[i], ctx);
			if (ret < 0)
				return ret;
		}
		return 0;
	}

	/* Tree-backed: descend the idx tree in order. This visits only leaves
	 * still referenced by their parent's idx; leaves freed and dropped during
	 * a range delete (whose next_leaf link would dangle) are skipped. */
	if (di->extent_inline_base == 0)
		return 0;
	return btree_walk_descend(sb, di->extent_inline_base, 0, cb, ctx);
}

/* ---------- free + drain ---------- */

/* Recursive free of a subtree: free data blocks of leaf extents, then free all
 * node blocks (leaves and internal). @cap bounds the number of nodes visited as
 * a guard against corrupt/cyclic trees. */
static void btree_free_subtree(struct super_block *sb, struct briefs_inode *di,
			       u64 block, u64 *cap)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int i, n;

	if (block == 0 || *cap == 0)
		return;
	(*cap)--;

	node = btree_read_node(sb, block, false, &bh);
	if (!node)
		return;

	if (btree_node_is_leaf(node)) {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++) {
			struct briefs_extent ext;
			briefs_disk_extent_to_cpu(&node->u.leaf.extents[i], &ext);
			if (ext.len > 0 && ext.phys > 0) {
				briefs_journal_extent_free(bsi->journal,
							   di->inode_number,
							   ext.offset, ext.phys,
							   ext.len);
				briefs_free_blocks_range(bsi, ext.phys, ext.len);
			}
		}
	} else {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++)
			btree_free_subtree(sb, di,
				le64_to_cpu(node->u.internal.idx[i].child), cap);
		btree_free_subtree(sb, di,
			le64_to_cpu(node->u.internal.trailing_child), cap);
	}

	brelse(bh);

	briefs_journal_extent_free(bsi->journal, 0, 0, block, 1);
	briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
}

void briefs_btree_free_all(struct super_block *sb, struct briefs_inode *di)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	u64 cap;

	/* Inline-only: free inline data blocks directly. */
	if (!(di->flags & InodeFlagIndexed)) {
		int i;
		for (i = 0; i < di->num_extents_inline; i++) {
			struct briefs_extent *e = &di->inline_extents[i];
			if (e->len > 0 && e->phys > 0) {
				briefs_journal_extent_free(bsi->journal,
							   di->inode_number,
							   e->offset, e->phys,
							   e->len);
				briefs_free_blocks_range(bsi, e->phys, e->len);
			}
		}
		return;
	}

	if (di->extent_inline_base == 0)
		return;

	/* Bound the walk by a generous multiple of the extent count (each node
	 * holds up to 126 extents, so nodes <= extents/1 + height). */
	cap = di->num_extents_total + 16;
	if (cap > (1ull << 20))
		cap = 1ull << 20;
	btree_free_subtree(sb, di, di->extent_inline_base, &cap);
}

/* Like btree_free_subtree but frees only the tree NODE blocks (leaves +
 * internal), not the data extents the leaves reference. Used by the
 * truncate/punch rebuild: the kept extents' data blocks must survive so they
 * can be re-inserted into the new tree. */
static void btree_free_nodes_only_subtree(struct super_block *sb,
					   struct briefs_inode *di, u64 block,
					   u64 *cap)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int i, n;

	if (block == 0 || *cap == 0)
		return;
	(*cap)--;

	node = btree_read_node(sb, block, false, &bh);
	if (!node)
		return;

	if (!btree_node_is_leaf(node)) {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++)
			btree_free_nodes_only_subtree(sb, di,
				le64_to_cpu(node->u.internal.idx[i].child), cap);
		btree_free_nodes_only_subtree(sb, di,
			le64_to_cpu(node->u.internal.trailing_child), cap);
	}
	/* Leaf: free the node block only (NOT its data extents). */

	brelse(bh);

	briefs_journal_extent_free(bsi->journal, 0, 0, block, 1);
	briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
}

/* Free every B-tree NODE block (leaves + internal levels) but leave the data
 * extents' blocks allocated, so a rebuild can re-insert the kept extents.
 * No-op for inline-only inodes (no tree nodes). Caller holds extent_lock. */
void briefs_btree_free_nodes_only(struct super_block *sb, struct briefs_inode *di)
{
	u64 cap;

	if (!(di->flags & InodeFlagIndexed))
		return;
	if (di->extent_inline_base == 0)
		return;

	cap = di->num_extents_total + 16;
	if (cap > (1ull << 20))
		cap = 1ull << 20;
	btree_free_nodes_only_subtree(sb, di, di->extent_inline_base, &cap);
}

/* ---------- range delete (truncate / punch hole) ---------- */

/* Remove from leaf @node every extent that overlaps [start,end), freeing the
 * data blocks in the overlap. Surviving extents (and a shortened left
 * straddler) are compacted to the front of the leaf, preserving sort order.
 * A right straddler — an extent with ext.offset+len > end — is recorded into
 * @right for the caller to re-insert (its data blocks beyond @end are kept).
 * Returns the new num_keys. The caller commits the leaf if non-empty. */
static int btree_leaf_delete_range(struct briefs_extent_btree_node *node,
				   u64 start, u64 end, struct briefs_inode *di,
				   struct briefs_sb_info *bsi,
				   struct briefs_extent *right, int *nright)
{
	int n = le16_to_cpu(node->hdr.num_keys);
	int i, out = 0;

	for (i = 0; i < n; i++) {
		struct briefs_disk_extent *de = &node->u.leaf.extents[i];
		struct briefs_extent ext;
		u64 ext_end;

		briefs_disk_extent_to_cpu(de, &ext);
		ext_end = ext.offset + ext.len;

		if (ext_end <= start || ext.offset >= end) {
			/* No overlap: keep verbatim (compacted forward). */
			if (out != i)
				node->u.leaf.extents[out] = *de;
			out++;
			continue;
		}

		/* Overlap: free the data blocks in [max(offset,start), min(end,ext_end)). */
		if (ext.phys > 0 && ext.len > 0) {
			u64 free_start = max(ext.offset, start);
			u64 free_end = min(ext_end, end);
			u64 free_len = free_end - free_start;

			if (free_len > 0) {
				u64 free_phys = ext.phys + (free_start - ext.offset);
				briefs_journal_extent_free(bsi->journal,
							   di->inode_number,
							   free_start, free_phys,
							   free_len);
				briefs_free_blocks_range(bsi, free_phys, free_len);
			}
		}

		/* Left straddler: keep the head [ext.offset, start) in place. */
		if (ext.offset < start) {
			struct briefs_extent left = ext;

			left.len = start - ext.offset;
			briefs_cpu_extent_to_disk(&left, &node->u.leaf.extents[out]);
			out++;
		}

		/* Right straddler: record [end, ext_end) for re-insertion. Its
		 * data blocks (beyond @end) are NOT freed here. At most one
		 * extent in a sorted, non-overlapping tree can straddle @end, so
		 * @right never fills in practice; the bound guards a corrupt tree. */
		if (ext_end > end && *nright < 4) {
			struct briefs_extent r = ext;

			r.offset = end;
			r.phys = ext.phys + (end - ext.offset);
			r.len = ext_end - end;
			right[(*nright)++] = r;
		}
	}
	return out;
}

/* Recursively delete [start,end) from the subtree rooted at @block. Frees data
 * blocks of removed extents, removes emptied leaves, and drops their parent
 * pointers (no rebalancing: sparse nonempty leaves stay). Returns true if the
 * subtree became completely empty and was freed (so the caller drops this child
 * pointer); false if it still holds extents (the node was committed). @cap
 * bounds recursion as a corrupt/cyclic-tree guard. */
static bool btree_delete_range_subtree(struct super_block *sb,
				       struct briefs_inode *di, u64 block,
				       u64 start, u64 end,
				       struct briefs_extent *right, int *nright,
				       u64 *cap)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int n, i, nc, out, last_surv, new_n;
	/* child i covers [low_i, high_i): low_i = idx[i-1].high_key (0 for i=0),
	 * high_i = idx[i].high_key (U64_MAX for the trailing child). The child
	 * pointer itself is idx[i].child, or trailing_child for the last. We read
	 * these directly from the node (no big stack cache) and track which
	 * children emptied in a bitmap. */
	DECLARE_BITMAP(child_empty, 254);

	if (block == 0 || *cap == 0)
		return false;
	(*cap)--;

	node = btree_read_node(sb, block, true, &bh);	/* trust_verified: extent_lock held */
	if (!node)
		return false;	/* unreadable: don't drop, stay conservative */

	if (btree_node_is_leaf(node)) {
		new_n = btree_leaf_delete_range(node, start, end, di, bsi,
						right, nright);
		if (new_n == 0) {
			/* Leaf emptied: free it; caller drops the pointer. */
			brelse(bh);
			briefs_journal_extent_free(bsi->journal, 0, 0, block, 1);
			briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
			return true;
		}
		node->hdr.num_keys = cpu_to_le16(new_n);
		btree_commit_node(bh);
		brelse(bh);
		return false;
	}

	/* Internal node: recurse into children whose key range intersects
	 * [start,end), marking emptied children in the bitmap. */
	n = le16_to_cpu(node->hdr.num_keys);
	nc = n + 1;
	bitmap_zero(child_empty, nc);
	for (i = 0; i < nc; i++) {
		u64 low = (i == 0) ? 0 :
			  le64_to_cpu(node->u.internal.idx[i - 1].high_key);
		u64 high = (i < n) ?
			  le64_to_cpu(node->u.internal.idx[i].high_key) : U64_MAX;
		u64 child = (i < n) ?
			    le64_to_cpu(node->u.internal.idx[i].child) :
			    le64_to_cpu(node->u.internal.trailing_child);

		if (child != 0 && low < end && high > start) {
			if (btree_delete_range_subtree(sb, di, child, start, end,
						       right, nright, cap))
				set_bit(i, child_empty);
		}
	}

	/* Rebuild the idx array + trailing_child in place, dropping emptied
	 * children. The separator between two surviving children a < b (with the
	 * children between them dropped) is idx[b-1].high_key = the lower bound of
	 * child b. Safe to compact in place: the write position out never exceeds
	 * the read positions (a = last_surv and b-1 = i-1, both >= out), so each
	 * source field is read before its slot is overwritten. */
	out = 0;
	last_surv = -1;
	for (i = 0; i < nc; i++) {
		u64 child_a;

		if (test_bit(i, child_empty))
			continue;
		if (last_surv >= 0) {
			child_a = (last_surv < n) ?
				  le64_to_cpu(node->u.internal.idx[last_surv].child) :
				  le64_to_cpu(node->u.internal.trailing_child);
			node->u.internal.idx[out].child = cpu_to_le64(child_a);
			node->u.internal.idx[out].high_key =
				node->u.internal.idx[i - 1].high_key;
			out++;
		}
		last_surv = i;
	}

	if (last_surv < 0) {
		/* All children dropped: this node is empty; free it. */
		brelse(bh);
		briefs_journal_extent_free(bsi->journal, 0, 0, block, 1);
		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
		return true;
	}

	{
		u64 trailing = (last_surv < n) ?
			       le64_to_cpu(node->u.internal.idx[last_surv].child) :
			       le64_to_cpu(node->u.internal.trailing_child);
		node->u.internal.trailing_child = cpu_to_le64(trailing);
	}
	node->hdr.num_keys = cpu_to_le16(out);
	btree_commit_node(bh);
	brelse(bh);
	return false;
}

/* Callback: track the max (offset+len) and count of surviving extents, for
 * cached_max_end and num_extents_total recompute after a range delete. */
struct btree_max_ctx { u64 max_end; u64 count; };
static int btree_max_cb(const struct briefs_extent *ext, void *ctx)
{
	struct btree_max_ctx *c = ctx;
	u64 end = ext->offset + ext->len;

	if (end > c->max_end)
		c->max_end = end;
	c->count++;
	return 0;
}

/*
 * briefs_btree_delete_range - remove every extent overlapping [start,end) from
 * a tree-backed inode, freeing the data blocks in the overlap. Leaves that
 * become empty are freed and dropped from their parents (no rebalancing). An
 * extent straddling @end is split: its head is removed, its tail [end, ...) is
 * re-inserted (data kept). The tail cache is recomputed.
 *
 * Caller holds extent_lock. Inline-only inodes are NOT handled here (the
 * caller's collect+rebuild path covers them); this returns 0 immediately if
 * the inode is not tree-backed.
 */
int briefs_btree_delete_range(struct super_block *sb, struct briefs_inode *di,
			      u64 start, u64 end)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_extent right[4];	/* at most one right straddler */
	int nright = 0, i, ret;
	u64 cap;
	bool root_empty;
	struct btree_max_ctx mc = { .max_end = 0, .count = 0 };

	if (!(di->flags & InodeFlagIndexed) || di->extent_inline_base == 0)
		return 0;
	if (start >= end)
		return 0;

	briefs_stat_inc(briefs_sb(sb), btree_extents_freed);

	cap = di->num_extents_total + 16;
	if (cap > (1ull << 20))
		cap = 1ull << 20;

	root_empty = btree_delete_range_subtree(sb, di, di->extent_inline_base,
						start, end, right, &nright, &cap);

	if (root_empty) {
		/* Every extent was removed: free the root (already freed by the
		 * subtree walk) and clear the tree-backed state. */
		write_seqcount_begin(&binfo->extent_seq);
		di->flags &= ~InodeFlagIndexed;
		di->extent_inline_base = 0;
		di->num_extents_inline = 0;
		di->num_extents_total = 0;
		memset(di->inline_extents, 0, sizeof(di->inline_extents));
		binfo->cached_max_end = 0;
		write_seqcount_end(&binfo->extent_seq);
		return 0;
	}

	/* Re-insert right-straddler tails (extents that extended past @end). */
	for (i = 0; i < nright; i++) {
		ret = briefs_btree_insert_locked(sb, di, &right[i]);
		if (ret)
			return ret;	/* delete applied; remainders lost — consistent */
	}

	/* Recompute the tail cache and the extent count: a tail delete may have
	 * lowered the max, and removed extents lower num_extents_total (used by
	 * the journal drain cap and fsck's count check). */
	briefs_btree_for_each_extent(sb, di, btree_max_cb, &mc);
	write_seqcount_begin(&binfo->extent_seq);
	binfo->cached_max_end = mc.max_end;
	di->num_extents_total = mc.count;
	write_seqcount_end(&binfo->extent_seq);
	return 0;
}

/* Lock-free recursive sync of every dirty tree node, for the journal snapshot
 * ordering. Bounded by @max_nodes. */
static void btree_drain_subtree(struct super_block *sb, u64 block, u64 *cap)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int i, n;

	if (block == 0 || *cap == 0)
		return;
	(*cap)--;

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_warn("briefs: btree drain: sb_bread node %llu failed\n",
			block);
		return;
	}
	if (buffer_dirty(bh))
		sync_dirty_buffer(bh);

	node = (struct briefs_extent_btree_node *)bh->b_data;
	if (le32_to_cpu(node->hdr.magic) == BRIEFS_BTREE_MAGIC &&
	    !(le32_to_cpu(node->hdr.flags) & BRIEFS_BTREE_LEAF)) {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++)
			btree_drain_subtree(sb,
				le64_to_cpu(node->u.internal.idx[i].child), cap);
		btree_drain_subtree(sb,
			le64_to_cpu(node->u.internal.trailing_child), cap);
	}
	brelse(bh);
}

void briefs_btree_drain(struct super_block *sb, u64 root_block, u64 max_nodes)
{
	u64 cap = max_nodes;

	if (root_block == 0)
		return;
	if (cap == 0)
		cap = 1ull << 20;
	btree_drain_subtree(sb, root_block, &cap);
}