// SPDX-License-Identifier: GPL-2.0-only OR MIT

/*
 * Offset-keyed B+ tree extent index.
 *
 * Replaces the flat singly-linked chain list. The tree is keyed by
 * briefs_disk_extent.offset (logical block number); every leaf is maintained
 * sorted by offset, and internal nodes carry separator keys (the min key of
 * each child's right neighbor) plus absolute child block pointers. Leaves are
 * threaded left-to-right by next_leaf, which is maintained on every split but
 * never traversed by the kernel: a range delete frees emptied leaves and drops
 * only their parent idx entries, so a surviving predecessor's chain link
 * dangles at the freed block. All kernel walks (in-order and lower-bound)
 * descend the idx structure, which cannot reference a freed leaf.
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

/*
 * Read a B-tree node, verifying magic + checksum.  trust_verified lets locked
 * callers skip the CRC on a buffer whose BH_Verified bit is set (no concurrent
 * modifier can have torn it).  Returns the buffer_head (caller brelse) and a
 * pointer to the node, or NULL on failure.
 *
 * Checksum errors are rate-limited to avoid flooding dmesg (the generic/299
 * spiral).  A per-superblock circuit breaker counts errors; after
 * BRIEFS_BTREE_ERROR_LIMIT the filesystem is forced read-only to stop the
 * retry loop.
 */
static struct briefs_extent_btree_node *
btree_read_node(struct super_block *sb, u64 block, bool trust_verified,
		 bool will_modify, struct buffer_head **bhp)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	struct briefs_sb_info *bsi = briefs_sb(sb);
	int count;

	/* A stale/corrupt extent-tree pointer can name a block past the device
	 * end; sb_bread() would then busy-loop unkillably (grow_buffers returns
	 * NULL, __bread_gfp retries forever with no signal-check point). Reject
	 * it before the read instead of wedging the box.
	 */
	if (!briefs_block_in_range(sb, block)) {
		pr_warn_ratelimited("briefs: btree: node %llu out of range\n", block);
		return NULL;
	}

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_warn_ratelimited("briefs: btree: failed to read node %llu\n", block);
		return NULL;
	}

	/* Wait for any in-flight writeback of this buffer to settle before the
	 * caller touches b_data -- but only on modify paths.  BrieFS edits
	 * extent-btree node buffers in place (no lock_buffer), so a modification
	 * made while the buffer's *previous* writeback is still in flight races
	 * the DMA: the device reads the page after the b_data edit but before
	 * btree_commit_node() updates the checksum, leaving disk with
	 * new-content/old-checksum -- a transient node that btree_read_node()
	 * later rejects as -EIO (generic/299: btree checksum mismatches and DIO
	 * stalls under memory-pressure writeback churn).  sb_bread() returns a
	 * cached uptodate buffer without waiting, so the wait belongs here, on
	 * the read that precedes a modify+commit.  After it the buffer is clean
	 * (the dirty bit was cleared at writeback submit), so no further
	 * writeback can be submitted before the caller's modify+commit, and the
	 * next writeback carries the new content + new checksum atomically.
	 *
	 * Read-only callers pass will_modify=false and skip the wait: the
	 * dominant such caller is fallocate's briefs_block_mapped(), which
	 * probes per run (16M per-block probes were eliminated by bounding the
	 * hole in O(1)); waiting on every probe, under writeback pressure, made
	 * the fallocate/truncate loop unable to keep up with fio and the test
	 * timed out.  A trust_verified=false reader used to rely on the CRC as
	 * a torn-read detector (a mid-modification snapshot failed the check
	 * and returned -EIO, which callers treated conservatively) -- but that
	 * left a real race: the read side never retries, and every mismatch
	 * ticks btree_error_count toward the 100-error forced-read-only
	 * circuit breaker.  Unlocked tree reads now hold extent_lock shared
	 * (taken by the extent.c dispatch layer), which excludes every
	 * in-place editor outright: a torn node cannot be observed at all, and
	 * the CRC check is pure defense-in-depth that must now always pass.
	 * No fs lock is held across the will_modify wait itself: this is a
	 * plain metadata buffer sync (no get_block, no extent_lock), so it
	 * cannot trip the mmap/writeback AB-BA of generic/074.
	 */
	if (will_modify)
		wait_on_buffer(bh);

	node = (struct briefs_extent_btree_node *)bh->b_data;

	if (!trust_verified || !buffer_verified(bh)) {
		if (le32_to_cpu(node->hdr.magic) != BRIEFS_BTREE_MAGIC) {
			pr_warn_ratelimited("briefs: btree: node %llu bad magic 0x%08x\n",
			       block, le32_to_cpu(node->hdr.magic));
			brelse(bh);
			count = atomic_inc_return(&bsi->btree_error_count);
			if (count >= BRIEFS_BTREE_ERROR_LIMIT) {
				pr_err("briefs: btree: %d errors, forcing read-only\n",
				       count);
				bsi->mount_flags |= BRIEFS_MF_ERROR_FS;
			}
			return NULL;
		}
		if (briefs_verify_chain_checksum(bh->b_data, node->checksum) != 0) {
			pr_warn_ratelimited("briefs: btree: node %llu checksum mismatch\n",
			       block);
			brelse(bh);
			count = atomic_inc_return(&bsi->btree_error_count);
			if (count >= BRIEFS_BTREE_ERROR_LIMIT) {
				pr_err("briefs: btree: %d errors, forcing read-only\n",
				       count);
				bsi->mount_flags |= BRIEFS_MF_ERROR_FS;
			}
			return NULL;
		}
		if (trust_verified)
			set_buffer_verified(bh);
	}

	*bhp = bh;
	return node;
}

/* Recompute a node's checksum, memoize it, and mark the buffer dirty. */
static void btree_commit_node(struct buffer_head *bh, struct super_block *sb)
{
	struct briefs_extent_btree_node *node =
		(struct briefs_extent_btree_node *)bh->b_data;

	node->checksum = cpu_to_le64(briefs_chain_checksum(bh->b_data));
	set_buffer_verified(bh);
	briefs_mark_buffer_dirty(bh, sb);
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

/* Leaf callback: invoked once the descent reaches the leaf whose subtree holds
 * @iblock. @node is the mapped leaf node (in @bh->b_data); @ctx is the caller's
 * context. Returns 0, -ENOENT, or -EIO. The helper owns bh lifetime and brelse's
 * after the callback returns, so the callback must NOT brelse(bh) (it may use bh
 * for a commit, which only marks dirty). */
typedef int (*btree_leaf_cb)(struct briefs_extent_btree_node *node,
			     struct buffer_head *bh, u64 iblock, void *ctx);

/* Leaf callback for the lookup walk: scan the leaf for the extent covering
 * @iblock and fill *@ext. Returns 0, -ENOENT if none covers it. */
static int btree_lookup_leaf(struct briefs_extent_btree_node *node,
			     struct buffer_head *bh, u64 iblock, void *ctx)
{
	struct briefs_extent *ext = ctx;
	int i, num_keys = le16_to_cpu(node->hdr.num_keys);

	for (i = 0; i < num_keys; i++) {
		struct briefs_disk_extent *de = &node->u.leaf.extents[i];
		u64 off = le64_to_cpu(de->offset);
		u64 len = le64_to_cpu(de->len);

		if (iblock >= off && iblock < off + len) {
			briefs_disk_extent_to_cpu(de, ext);
			return 0;
		}
		if (off > iblock)
			break;	/* sorted: no later extent can cover it */
	}
	return -ENOENT;
}

/* Descend the B+ tree to the leaf whose subtree holds @iblock and invoke @cb to
 * perform the leaf-specific work. The internal-node descent skeleton (read
 * node, find the child whose subtree holds @iblock, null-check, recurse) is
 * shared by the lookup and convert_unwritten walks, which differ only in their
 * leaf behavior; @cb supplies that. The helper owns bh lifetime: it brelse's
 * after @cb returns on the leaf path and before recursing on the internal path.
 * Returns @cb's result (0/-ENOENT) or -EIO on a read or null-child failure.
 *
 * btree_lower_bound_block is NOT routed here: its leaf work (scan for the first
 * extent with offset > @iblock) is simple enough, but on a miss it must retry
 * right-sibling subtrees at each internal level, which the single-callback
 * skeleton here cannot express. */
static int btree_descend_to_leaf(struct super_block *sb, u64 block, u64 iblock,
				 btree_leaf_cb cb, void *ctx, bool trust_verified)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int p;
	u64 child;

	node = btree_read_node(sb, block, trust_verified, false, &bh);
	if (!node)
		return -EIO;

	if (btree_node_is_leaf(node)) {
		int ret = cb(node, bh, iblock, ctx);

		brelse(bh);
		return ret;
	}

	p = btree_internal_find_child(node, iblock);
	child = (p < le16_to_cpu(node->hdr.num_keys))
		? le64_to_cpu(node->u.internal.idx[p].child)
		: le64_to_cpu(node->u.internal.trailing_child);
	brelse(bh);
	if (child == 0) {
		pr_err("briefs: btree: internal node %llu has null child\n",
		       block);
		return -EIO;
	}
	return btree_descend_to_leaf(sb, child, iblock, cb, ctx, trust_verified);
}

/* Lookup: descend to the leaf covering @iblock and return the matching extent.
 * Returns 0 and fills *ext, -ENOENT, or -EIO. */
static int btree_lookup_block(struct super_block *sb, u64 block, u64 iblock,
			      struct briefs_extent *ext, bool trust_verified)
{
	return btree_descend_to_leaf(sb, block, iblock, btree_lookup_leaf, ext,
				     trust_verified);
}

/* Lower-bound descent: find the first extent with offset > @iblock (the extent
 * that bounds a hole at @iblock on the right). Used by the iomap path to bound a
 * hole mapping so the iomap iterator does not crawl one block at a time, and so a
 * hole past the last extent (or on an inode whose cached_max_end is unknown)
 * terminates in one step.
 *
 * Descends exactly like btree_lookup_block (to the leaf whose subtree holds
 * @iblock's range), then scans that leaf for the first extent with offset >
 * @iblock. If that subtree holds no such extent (@iblock sits in a hole that
 * spans past the leaf), the bound is the leftmost live extent of the next
 * sibling child in idx order, so each internal level retries the children to
 * the right of the one covering @iblock. The bound is therefore derived from
 * the parent idx structure, never from the leaf next_leaf chain: a range delete
 * frees emptied leaves and drops only their parent idx entries, so a surviving
 * predecessor's next_leaf dangles at the freed block, whose cached buffer
 * holds a zeroed-payload corpse that fails the CRC (observed as "node N
 * checksum mismatch" under generic/112's punch+fallocate mix) -- and once the
 * block is reused, the chain would name a valid node at the WRONG key range.
 * Returns 0 and fills *ext, -ENOENT if no extent has offset > @iblock (hole
 * runs to EOF / the query end), or -EIO on a read/checksum failure. */
static int btree_lower_bound_block(struct super_block *sb, u64 block, u64 iblock,
				   struct briefs_extent *ext, bool trust_verified)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int p, c, n, ret;

	node = btree_read_node(sb, block, trust_verified, false, &bh);
	if (!node)
		return -EIO;

	if (btree_node_is_leaf(node)) {
		int i, num_keys = le16_to_cpu(node->hdr.num_keys);

		for (i = 0; i < num_keys; i++) {
			struct briefs_disk_extent *de = &node->u.leaf.extents[i];
			u64 off = le64_to_cpu(de->offset);

			if (off > iblock) {
				briefs_disk_extent_to_cpu(de, ext);
				brelse(bh);
				return 0;
			}
		}
		/* No extent > @iblock in this leaf: tell the parent level so it
		 * can take the bound from a right sibling subtree. */
		brelse(bh);
		return -ENOENT;
	}

	/* Internal node: children 0..n-1 via idx[], child n via trailing_child.
	 * Descend the child whose key range holds @iblock first; on -ENOENT,
	 * retry the following children in idx order. Each retry's range lies
	 * entirely above @iblock, so its own descent returns its leftmost live
	 * extent -- which is exactly the lower bound the (dangle-prone)
	 * next_leaf chain used to approximate. The internal node's buffer is
	 * held across the retries because the child pointers are re-read from
	 * it (the same hold-across-recursion pattern as
	 * btree_delete_range_subtree). */
	n = le16_to_cpu(node->hdr.num_keys);
	p = btree_internal_find_child(node, iblock);
	for (c = p; c <= n; c++) {
		u64 child = (c < n) ? le64_to_cpu(node->u.internal.idx[c].child)
				    : le64_to_cpu(node->u.internal.trailing_child);

		if (child == 0) {
			brelse(bh);
			pr_err("briefs: btree: internal node %llu has null child\n",
			       block);
			return -EIO;
		}
		ret = btree_lower_bound_block(sb, child, iblock, ext,
					      trust_verified);
		if (ret != -ENOENT) {
			brelse(bh);
			return ret;	/* 0 or -EIO */
		}
	}
	brelse(bh);
	return -ENOENT;
}

int briefs_btree_next_extent(struct super_block *sb, u64 root_block, u64 iblock,
			     struct briefs_extent *ext, bool trust_verified)
{
	if (root_block == 0)
		return -ENOENT;
	return btree_lower_bound_block(sb, root_block, iblock, ext, trust_verified);
}

int briefs_btree_lookup(struct super_block *sb, u64 root_block, u64 iblock,
			struct briefs_extent *ext, bool trust_verified)
{
	if (root_block == 0)
		return -ENOENT;
	return btree_lookup_block(sb, root_block, iblock, ext, trust_verified);
}

/* Context for the convert_unwritten leaf callback: the sub-range [start_blk,
 * end_blk) to convert to written, and any unwritten prefix/suffix split off for
 * the caller to re-insert via briefs_btree_insert_locked().  The in-place leaf
 * edit only shrinks the found record to the written middle; the prefix/suffix
 * are re-inserted after the descent releases the leaf buffer (avoiding holding
 * two leaf buffers, which could deadlock a concurrent split).
 */
struct btree_conv_ctx {
	struct super_block *sb;
	u64 start_blk;
	u64 end_blk;
	struct briefs_extent prefix;
	struct briefs_extent suffix;
	bool have_prefix;
	bool have_suffix;
	u64 converted;		/* blocks flipped unwritten->written (the middle) */
};

/* Leaf callback: find the unwritten extent covering start_blk and convert the
 * sub-range [cstart, cend) to written.  If the write covers the whole extent,
 * clear BRIEFS_EXT_UNWRITTEN in place.  Otherwise shrink the record to the
 * written middle [cstart, cend) (in place, no block free) and stash the
 * unwritten prefix [off, cstart) and suffix [cend, off+len) for the caller to
 * re-insert.  Splitting -- rather than clearing the whole extent -- keeps the
 * un-written blocks as IOMAP_UNWRITTEN so reads return zeros; the data blocks
 * are never zeroed, which avoids the millions of synchronous writes a large
 * fallocate would otherwise issue.  Caller holds extent_lock.
 */
static int btree_convert_unwritten_leaf(struct briefs_extent_btree_node *node,
					 struct buffer_head *bh, u64 iblock,
					 void *ctx)
{
	struct btree_conv_ctx *c = ctx;
	struct super_block *sb = c->sb;
	int i, num_keys = le16_to_cpu(node->hdr.num_keys);

	/* This callback edits the leaf in place then btree_commit_node()s it.
	 * btree_descend_to_leaf() read the leaf with will_modify=false (it is a
	 * shared read/modify descent), so wait for any in-flight writeback of
	 * this leaf to settle before we touch b_data -- same commit-under-
	 * writeback race as the direct modify paths (generic/299).
	 */
	wait_on_buffer(bh);

	for (i = 0; i < num_keys; i++) {
		struct briefs_disk_extent *de = &node->u.leaf.extents[i];
		u64 off = le64_to_cpu(de->offset);
		u64 len = le64_to_cpu(de->len);
		u64 phys = le64_to_cpu(de->phys);
		u32 flags = le32_to_cpu(de->flags);
		u64 eend = off + len;
		u64 cstart, cend;

		if (!(c->start_blk >= off && c->start_blk < eend)) {
			if (off > c->start_blk)
				break;	/* sorted: no later extent covers it */
			continue;
		}

		if (!(flags & BRIEFS_EXT_UNWRITTEN))
			return 0;		/* already written */

		cstart = c->start_blk;
		if (cstart < off)
			cstart = off;
		cend = c->end_blk;
		if (cend > eend)
			cend = eend;

		/* Record the converted count for the caller's reservation release. */
		c->converted = cend - cstart;

		if (cstart == off && cend == eend) {
			/* Whole extent is written: clear the flag in place. */
			de->flags = cpu_to_le32(flags & ~BRIEFS_EXT_UNWRITTEN);
			btree_commit_node(bh, sb);
			return 0;
		}

		/* Partial write: shrink this record to the written middle.  The
		 * record stays sorted (prev ends <= off <= cstart, next starts
		 * >= eend >= cend).  Re-insert the unwritten wings afterwards.
		 */
		de->offset = cpu_to_le64(cstart);
		de->phys = cpu_to_le64(phys + (cstart - off));
		de->len = cpu_to_le64(cend - cstart);
		de->flags = cpu_to_le32(flags & ~BRIEFS_EXT_UNWRITTEN);
		btree_commit_node(bh, sb);

		if (cstart > off) {
			c->prefix.offset = off;
			c->prefix.phys = phys;
			c->prefix.len = cstart - off;
			c->prefix.flags = flags;	/* keep UNWRITTEN */
			c->have_prefix = true;
		}
		if (cend < eend) {
			c->suffix.offset = cend;
			c->suffix.phys = phys + (cend - off);
			c->suffix.len = eend - cend;
			c->suffix.flags = flags;	/* keep UNWRITTEN */
			c->have_suffix = true;
		}
		return 0;
	}
	return -ENOENT;
}

/* Convert the unwritten blocks in [start_blk, end_blk) of the tree-backed extent
 * covering start_blk to written, splitting the extent so the un-written wings
 * stay unwritten (and read back as zeros).  Caller holds extent_lock.  Returns
 * 0, -ENOENT, or -EIO.  No-op (-ENOENT) for inline-only inodes.
 */
int briefs_btree_convert_unwritten_range(struct super_block *sb,
					  struct briefs_inode *di,
					  u64 start_blk, u64 end_blk)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct btree_conv_ctx c = {
		.sb = sb,
		.start_blk = start_blk,
		.end_blk = end_blk,
		.have_prefix = false,
		.have_suffix = false,
		.converted = 0,
	};
	int ret;

	if (!(di->flags & InodeFlagIndexed) || di->extent_inline_base == 0)
		return -ENOENT;

	ret = btree_descend_to_leaf(sb, di->extent_inline_base, start_blk,
				    btree_convert_unwritten_leaf, &c, true);
	if (ret)
		return ret;

	/* Re-insert the unwritten wings.  briefs_btree_insert_locked() merges
	 * them with same-state neighbors (unwritten+unwritten) and handles leaf
	 * overflow / inline->btree spill; it never merges a written neighbor
	 * with an unwritten one (generic/092).  Each insert may itself allocate
	 * blocks and journal, which is safe under the held extent_lock (the
	 * normal insert path takes the same lock).
	 */
	if (c.have_prefix) {
		ret = briefs_btree_insert_locked(sb, di, &c.prefix);
		if (ret)
			return ret;
	}
	if (c.have_suffix) {
		ret = briefs_btree_insert_locked(sb, di, &c.suffix);
		if (ret)
			return ret;
	}

	/* Release the reservation for the converted middle (the wings stay
	 * unwritten and remain counted).  No-op if nothing was converted. */
	briefs_release_unwritten_reserve(&binfo->vfs_inode, c.converted);
	return 0;
}

/* ---------- insertion ---------- */

/* Absorb a child split into parent @node_bh at child position @p. The separator
 * becomes the new upper bound of the left (existing) child; the sibling becomes
 * a new child to its right, inheriting the left child's old upper bound. */
static void btree_absorb_split(struct briefs_extent_btree_node *parent,
			       struct buffer_head *parent_bh, int p,
			       u64 sibling, u64 separator,
			       struct super_block *sb)
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
	btree_commit_node(parent_bh, sb);
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
	u64 sib_block, rel, sep;
	bool is_leaf;
	int child_n, mid, move;

	child_block = (p < num_keys)
		? le64_to_cpu(parent->u.internal.idx[p].child)
		: le64_to_cpu(parent->u.internal.trailing_child);

	child = btree_read_node(sb, child_block, true, true, &child_bh);
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
	rel = briefs_alloc_block_meta(&bsi->alloc);
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
		/* Zero the unused tail of the sibling (indices move to BRIEFS_BTREE_LEAF_FANOUT-1) */
		if (move < BRIEFS_BTREE_LEAF_FANOUT) {
			memset(&sib->u.leaf.extents[move], 0,
			       (BRIEFS_BTREE_LEAF_FANOUT - move) * sizeof(child->u.leaf.extents[0]));
		}

		/* Left half stays in place (just truncate the count + link). */
		child->hdr.num_keys = cpu_to_le16(mid);
		/* Zero the unused tail of the child (indices mid to BRIEFS_BTREE_LEAF_FANOUT-1) */
		if (mid < BRIEFS_BTREE_LEAF_FANOUT) {
			memset(&child->u.leaf.extents[mid], 0,
			       (BRIEFS_BTREE_LEAF_FANOUT - mid) * sizeof(child->u.leaf.extents[0]));
		}
		child->hdr.next_leaf = cpu_to_le64(sib_block);

		btree_commit_node(child_bh, sb);
		btree_commit_node(sib_bh, sb);

		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, sib_block, 1, -1);

		btree_absorb_split(parent, parent_bh, p, sib_block,
				   le64_to_cpu(sib->u.leaf.extents[0].offset),
				   sb);
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
		/* Zero the unused tail of the sibling (indices move to BRIEFS_BTREE_IDX_KEYS-1) */
		if (move < BRIEFS_BTREE_IDX_KEYS) {
			memset(&sib->u.internal.idx[move], 0,
			       (BRIEFS_BTREE_IDX_KEYS - move) * sizeof(child->u.internal.idx[0]));
		}

		/* Left half: truncate to idx[0..mid-1], trailing = old idx[mid].child. */
		child->u.internal.trailing_child =
			child->u.internal.idx[mid].child;
		/* Save the median separator before the tail is zeroed below. */
		sep = le64_to_cpu(child->u.internal.idx[mid].high_key);
		child->hdr.num_keys = cpu_to_le16(mid);
		/* Zero the unused tail of the child (indices mid to BRIEFS_BTREE_IDX_KEYS-1) */
		if (mid < BRIEFS_BTREE_IDX_KEYS) {
			memset(&child->u.internal.idx[mid], 0,
			       (BRIEFS_BTREE_IDX_KEYS - mid) * sizeof(child->u.internal.idx[0]));
		}

		btree_commit_node(child_bh, sb);
		btree_commit_node(sib_bh, sb);

		briefs_journal_extent_alloc(bsi->journal, di->inode_number,
					    0, sib_block, 1, -1);

		btree_absorb_split(parent, parent_bh, p, sib_block, sep, sb);
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

	/*
	 * Reject overlapping extents.  btree_leaf_find_pos() returns the first
	 * index whose offset is strictly greater than ext->offset, so any entry
	 * at exactly ext->offset sits at pos-1 -- and the merge checks below do
	 * NOT catch it (they require the neighbor to *end at* ext->offset, not
	 * equal it).  Without this guard, a check-then-insert race (notably
	 * fallocate's unlocked briefs_block_mapped() test vs. background writeback
	 * converting a BH_Delay page) lets two extents for the same logical range
	 * coexist: lookup returns one phys while a pagecache buffer_head is pinned
	 * to the other, so writeback writes data to a block the btree no longer
	 * points at and the file silently diverges after drop_caches/crash
	 * (generic/547).  The scan also covers a run overlapping an existing
	 * middle block.  Return -EEXIST so the caller frees its just-allocated
	 * block and adopts the existing mapping / skips.  This runs under
	 * extent_lock, so it is atomic with respect to other inserters.
	 */
	{
		int start_i = (pos > 0) ? pos - 1 : pos;
		int i;

		for (i = start_i; i < n; i++) {
			struct briefs_extent e;

			briefs_disk_extent_to_cpu(&ents[i], &e);
			if (e.offset >= ext->offset + ext->len)
				break;		/* strictly to the right; no overlap */
			if (e.offset + e.len > ext->offset)
				return -EEXIST;	/* overlaps [ext->offset, ext->end) */
		}
	}

	/* Merge with the left neighbor (extent just before the insertion slot).
	 * Only merge extents of the same state: a written neighbor must not
	 * absorb an unwritten new extent (or vice versa), or the merged extent
	 * would inherit the neighbor's flag and the unwritten range would read
	 * back as zeros-valid data / lose its preallocated-state semantics
	 * (generic/092: fallocate adjoining a written extent must produce a
	 * separate unwritten extent, not a single written one). */
	if (pos > 0) {
		struct briefs_extent left;

		briefs_disk_extent_to_cpu(&ents[pos - 1], &left);
		if (left.offset + left.len == ext->offset &&
		    left.phys + left.len == ext->phys &&
		    left.flags == ext->flags) {
			left.len += ext->len;
			briefs_cpu_extent_to_disk(&left, &ents[pos - 1]);
			btree_commit_node(bh, sb);
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
		    ext->phys + ext->len == right.phys &&
		    ext->flags == right.flags) {
			right.offset = ext->offset;
			right.phys = ext->phys;
			right.len += ext->len;
			briefs_cpu_extent_to_disk(&right, &ents[pos]);
			btree_commit_node(bh, sb);
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
	btree_commit_node(bh, sb);
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

	node = btree_read_node(sb, block, true, true, &bh);
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

	root = btree_read_node(sb, root_block, true, true, &root_bh);
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
	rel = briefs_alloc_block_meta(&bsi->alloc);
	if (rel == 0) {
		brelse(root_bh);
		return -ENOSPC;
	}
	sib_block = data_to_abs(bsi->sb, rel);

	rel2 = briefs_alloc_block_meta(&bsi->alloc);
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
		/* Zero the unused tail of the sibling */
		if (move < BRIEFS_BTREE_LEAF_FANOUT) {
			memset(&sib->u.leaf.extents[move], 0,
			       (BRIEFS_BTREE_LEAF_FANOUT - move) * sizeof(root->u.leaf.extents[0]));
		}

		root->hdr.num_keys = cpu_to_le16(mid);
		/* Zero the unused tail of the root */
		if (mid < BRIEFS_BTREE_LEAF_FANOUT) {
			memset(&root->u.leaf.extents[mid], 0,
			       (BRIEFS_BTREE_LEAF_FANOUT - mid) * sizeof(root->u.leaf.extents[0]));
		}
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
		/* Zero the unused tail of the sibling */
		if (move < BRIEFS_BTREE_IDX_KEYS) {
			memset(&sib->u.internal.idx[move], 0,
			       (BRIEFS_BTREE_IDX_KEYS - move) * sizeof(root->u.internal.idx[0]));
		}

		root->u.internal.trailing_child = root->u.internal.idx[mid].child;
		/* Save the median separator before the tail is zeroed below. */
		separator = le64_to_cpu(root->u.internal.idx[mid].high_key);
		root->hdr.num_keys = cpu_to_le16(mid);
		/* Zero the unused tail of the root */
		if (mid < BRIEFS_BTREE_IDX_KEYS) {
			memset(&root->u.internal.idx[mid], 0,
			       (BRIEFS_BTREE_IDX_KEYS - mid) * sizeof(root->u.internal.idx[0]));
		}
	}

	btree_commit_node(root_bh, sb);
	btree_commit_node(sib_bh, sb);

	/* New internal root, one level above the old root. */
	newroot->hdr.magic = cpu_to_le32(BRIEFS_BTREE_MAGIC);
	newroot->hdr.flags = cpu_to_le32(0);
	newroot->hdr.level = cpu_to_le16(le16_to_cpu(root->hdr.level) + 1);
	newroot->hdr.num_keys = cpu_to_le16(1);
	newroot->hdr.next_leaf = cpu_to_le64(0);
	newroot->u.internal.idx[0].child = cpu_to_le64(root_block);
	newroot->u.internal.idx[0].high_key = cpu_to_le64(separator);
	newroot->u.internal.trailing_child = cpu_to_le64(sib_block);
	btree_commit_node(newroot_bh, sb);

	briefs_journal_extent_alloc(bsi->journal, di->inode_number,
				    0, sib_block, 1, -1);
	briefs_journal_extent_alloc(bsi->journal, di->inode_number,
				    0, newroot_block, 1, -1);

	/* Publish the new root last: root, sibling, and new root are all dirty
	 * on disk (or will be via the drain) before the snapshot can reference
	 * newroot_block. */
	briefs_extent_write_begin(binfo);
	di->extent_inline_base = newroot_block;
	briefs_extent_write_end(binfo);

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
		    merged[m - 1].phys + merged[m - 1].len == tmp[i].phys &&
		    merged[m - 1].flags == tmp[i].flags) {
			merged[m - 1].len += tmp[i].len;
		} else {
			merged[m++] = tmp[i];
		}
	}

	rel = briefs_alloc_block_meta(&bsi->alloc);
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
	/* Zero the unused tail slots */
	if (m < BRIEFS_BTREE_LEAF_FANOUT) {
		memset(&node->u.leaf.extents[m], 0,
		       (BRIEFS_BTREE_LEAF_FANOUT - m) * sizeof(node->u.leaf.extents[0]));
	}
	btree_commit_node(bh, sb);
	brelse(bh);

	briefs_journal_extent_alloc(bsi->journal, di->inode_number, 0,
				    root_block, 1, -1);
	/* The new extent's data blocks: journal for bitmap recovery. */
	briefs_journal_extent_alloc(bsi->journal, di->inode_number, ext->offset,
				    ext->phys, ext->len, -1);

	briefs_extent_write_begin(binfo);
	memset(di->inline_extents, 0, sizeof(di->inline_extents));
	di->flags |= InodeFlagIndexed;
	di->extent_inline_base = root_block;
	di->num_extents_inline = 0;
	di->num_extents_total = m;
	briefs_extent_write_end(binfo);

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

	/* Reject overlapping extents -- see btree_leaf_insert(). */
	{
		int start_i = (pos > 0) ? pos - 1 : pos;
		int j;

		for (j = start_i; j < n; j++) {
			struct briefs_extent *e = &di->inline_extents[j];

			if (e->offset >= ext->offset + ext->len)
				break;
			if (e->offset + e->len > ext->offset)
				return -EEXIST;
		}
	}

	/* Merge with the left neighbor. */
	if (pos > 0) {
		struct briefs_extent *left = &di->inline_extents[pos - 1];
		if (left->offset + left->len == ext->offset &&
		    left->phys + left->len == ext->phys &&
		    left->flags == ext->flags) {
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
		    ext->phys + ext->len == right->phys &&
		    ext->flags == right->flags) {
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

	briefs_extent_write_begin(binfo);
	for (i = n; i > pos; i--)
		di->inline_extents[i] = di->inline_extents[i - 1];
	di->inline_extents[pos] = *ext;
	di->num_extents_inline = n + 1;
	di->num_extents_total++;
	briefs_extent_write_end(binfo);

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
		briefs_extent_write_begin(binfo);
		if (new_end > binfo->cached_max_end)
			binfo->cached_max_end = new_end;
		briefs_extent_write_end(binfo);
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
			briefs_extent_write_begin(binfo);
			di->num_extents_total++;
			briefs_extent_write_end(binfo);
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
 * from their parent during a range delete (whose next_leaf link dangles at a
 * freed/reused block) are never visited; no kernel reader traverses the chain.
 * @depth bounds recursion as a guard against a corrupt/cyclic tree. Stops early
 * if @cb returns negative. */
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

	node = btree_read_node(sb, block, false, false, &bh);
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
	 * a range delete (whose dangling next_leaf is unreachable -- no kernel
	 * reader traverses the chain) are skipped. */
	if (di->extent_inline_base == 0)
		return 0;
	return btree_walk_descend(sb, di->extent_inline_base, 0, cb, ctx);
}

/* Locking wrapper for callers NOT holding extent_lock: takes it shared around
 * the walk (root snapshot included) so no node can be observed mid-edit and
 * the root block cannot be freed and reused mid-descent. Callers holding the
 * lock must use briefs_btree_for_each_extent directly -- down_read on a lock
 * already held self-deadlocks. Takes the briefs_inode_info (not the embedded
 * disk inode) so the locked object and the walked image always match; a caller
 * holding a stack-local copy of the disk inode must copy it into binfo first.
 */
int briefs_btree_walk_extents_unlocked(struct super_block *sb,
				       struct briefs_inode_info *binfo,
				       int (*cb)(const struct briefs_extent *, void *),
				       void *ctx)
{
	int ret;

	briefs_extent_read_lock(binfo);
	ret = briefs_btree_for_each_extent(sb, &binfo->disk_inode, cb, ctx);
	briefs_extent_read_unlock(binfo);
	return ret;
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

	node = btree_read_node(sb, block, false, false, &bh);
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

	node = btree_read_node(sb, block, false, false, &bh);
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
				   struct briefs_extent *right, int *nright,
				   bool *modified, u64 *unwritten_freed,
				   s64 *count_delta, u64 *blocks_freed)
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
		*modified = true;
		/* Net extent-count change from this walk: the original extent is
		 * removed (-1); a left straddler keeps its head in place (+1).  A
		 * right straddler tail is re-inserted later by the caller via
		 * briefs_btree_insert_locked, which increments the count itself. */
		*count_delta += (ext.offset < start ? 1 : 0) - 1;
		if (ext.phys > 0 && ext.len > 0) {
			u64 free_start = max(ext.offset, start);
			u64 free_end = min(ext_end, end);
			u64 free_len = free_end - free_start;

			if (free_len > 0) {
				u64 free_phys = ext.phys + (free_start - ext.offset);

				*blocks_freed += free_len;
				/* Count unwritten blocks returned to the allocator so
				 * briefs_btree_delete_range can release the metadata
				 * reservation held for them.  The left/right straddlers
				 * kept below retain ext.flags (stay unwritten, still
				 * counted), so only the freed middle is counted. */
				if (ext.flags & BRIEFS_EXT_UNWRITTEN)
					*unwritten_freed += free_len;
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

	/* Zero the stale tail entries so the checksum is deterministic.
	 * The checksum covers the entire block (0-4079), including unused slots. */
	if (out < n) {
		memset(&node->u.leaf.extents[out], 0,
		       (n - out) * sizeof(struct briefs_disk_extent));
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
				       u64 *cap, bool *modified,
				       u64 *unwritten_freed, s64 *count_delta,
				       u64 *blocks_freed)
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

	/* trust_verified: extent_lock held; will_modify: we edit this node */
	node = btree_read_node(sb, block, true, true, &bh);
	if (!node)
		return false;	/* unreadable: don't drop, stay conservative */

	if (btree_node_is_leaf(node)) {
		new_n = btree_leaf_delete_range(node, start, end, di, bsi,
						right, nright, modified,
						unwritten_freed, count_delta,
						blocks_freed);
		if (new_n == 0) {
			/* Leaf emptied: free it; caller drops the pointer. */
			brelse(bh);
			briefs_journal_extent_free(bsi->journal, 0, 0, block, 1);
			briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
			return true;
		}
		node->hdr.num_keys = cpu_to_le16(new_n);
		btree_commit_node(bh, sb);
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
						       right, nright, cap,
						       modified, unwritten_freed,
						       count_delta, blocks_freed))
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

	/* Zero the stale tail entries so the checksum is deterministic.
	 * The checksum covers the entire block (0-4079), including unused slots. */
	if (out < n) {
		memset(&node->u.internal.idx[out], 0,
		       (n - out) * sizeof(struct briefs_btree_idx_entry));
	}

	node->hdr.num_keys = cpu_to_le16(out);
	btree_commit_node(bh, sb);
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
			      u64 start, u64 end, bool *modified,
			      u64 *blocks_freed)
{
	struct briefs_inode_info *binfo =
		container_of(di, struct briefs_inode_info, disk_inode);
	struct briefs_extent right[4];	/* at most one right straddler */
	int nright = 0, i, ret;
	u64 cap;
	bool root_empty;
	u64 unwritten_freed = 0;
	s64 count_delta = 0;
	struct btree_max_ctx mc = { .max_end = 0, .count = 0 };

	*modified = false;
	*blocks_freed = 0;

	if (!(di->flags & InodeFlagIndexed) || di->extent_inline_base == 0)
		return 0;
	if (start >= end)
		return 0;

	briefs_stat_inc(briefs_sb(sb), btree_extents_freed);

	cap = di->num_extents_total + 16;
	if (cap > (1ull << 20))
		cap = 1ull << 20;

	root_empty = btree_delete_range_subtree(sb, di, di->extent_inline_base,
						start, end, right, &nright, &cap,
						modified, &unwritten_freed,
						&count_delta, blocks_freed);

	/* Release the metadata reservation held for the unwritten blocks just
	 * freed (punch hole / truncate-down over a preallocated region).  No-op
	 * when no unwritten extents were removed.  Done right after the walk so
	 * the root_empty early return and the straddler re-insert path below
	 * cannot skip it: the walk is the only thing that frees data blocks, so
	 * unwritten_freed is final here. */
	briefs_release_unwritten_reserve(&binfo->vfs_inode, unwritten_freed);

	if (root_empty) {
		/* Every extent was removed from the tree: free the root (already
		 * freed by the subtree walk) and clear the tree-backed state so the
		 * freed root block is not dereferenced below.
		 *
		 * A right straddler that extended past @end was extracted to @right
		 * BEFORE its leaf was freed, so its data blocks survive but are now
		 * unreferenced. When nright > 0 we must NOT return here: that would
		 * orphan the straddler's blocks (allocated but in no extent, read
		 * back as a hole). Instead continue past this block to re-insert
		 * the straddlers into the cleared (now inline-only) inode, which
		 * promotes back to indexed if it spills, then recompute the tail
		 * cache and count.
		 */
		*modified = true;
		briefs_extent_write_begin(binfo);
		di->flags &= ~InodeFlagIndexed;
		di->extent_inline_base = 0;
		di->num_extents_inline = 0;
		di->num_extents_total = 0;
		memset(di->inline_extents, 0, sizeof(di->inline_extents));
		binfo->cached_max_end = 0;
		briefs_extent_write_end(binfo);

		if (nright == 0)
			return 0;
	}

	/* Re-insert right-straddler tails (extents that extended past @end). */
	for (i = 0; i < nright; i++) {
		ret = briefs_btree_insert_locked(sb, di, &right[i]);
		if (ret)
			return ret;	/* delete applied; remainders lost — consistent */
	}

	/* Update the tail cache and the extent count.
	 *
	 * cached_max_end is the end of the last (highest-offset) extent.  It only
	 * decreases when that extent is fully removed, i.e. when the delete range
	 * reaches it (end >= cached_max_end).  Otherwise the last extent survives
	 * (untouched, or split at @end whose tail keeps the same end), so the max
	 * is unchanged -- and leaving it overstated is safe (the fast path just
	 * fires less; understating would make a mapped block read as a hole).
	 *
	 * num_extents_total is maintained incrementally: the subtree walk recorded
	 * its net change in count_delta (the straddler re-inserts above already
	 * added their own +1 each via briefs_btree_insert_locked).  Only the rare
	 * tail-delete case needs the full walk, which recomputes both exactly.
	 */
	if (end >= binfo->cached_max_end) {
		briefs_btree_for_each_extent(sb, di, btree_max_cb, &mc);
		briefs_extent_write_begin(binfo);
		binfo->cached_max_end = mc.max_end;
		di->num_extents_total = mc.count;
		briefs_extent_write_end(binfo);
	} else if (!root_empty) {
		briefs_extent_write_begin(binfo);
		di->num_extents_total =
			(u64)((s64)di->num_extents_total + count_delta);
		briefs_extent_write_end(binfo);
	}
	return 0;
}

/* Lock-free recursive sync of every dirty tree node, for the journal snapshot
 * ordering. Bounded by @max_nodes. Returns 0 on success, -EIO on write error. */
static int btree_drain_subtree(struct super_block *sb, u64 block, u64 *cap)
{
	struct buffer_head *bh;
	struct briefs_extent_btree_node *node;
	int i, n, err = 0;

	if (block == 0 || *cap == 0)
		return 0;
	(*cap)--;

	if (!briefs_block_in_range(sb, block)) {
		pr_debug("briefs: btree drain: node %llu out of range\n", block);
		return 0;
	}

	bh = sb_bread(sb, block);
	if (!bh) {
		pr_debug("briefs: btree drain: sb_bread node %llu failed\n",
			 block);
		return -EIO;
	}
	/*
	 * Don't do synchronous I/O here! This function is called from
	 * briefs_journal_inode_full() under j->write_lock. Doing sync_dirty_buffer()
	 * for every dirty btree node would block the journal for milliseconds
	 * while waiting for I/O, causing severe lock contention under load.
	 *
	 * The journal sync path (__briefs_journal_sync_locked) calls sync_blockdev()
	 * which flushes ALL dirty buffers on the device, including these btree nodes.
	 * Since the JRN_INODE_FULL journal record is written AFTER the nodes are
	 * dirtied, sync_blockdev() ensures they reach disk before the journal commit
	 * point is persisted.
	 *
	 * We still check for write errors on buffers that were submitted by
	 * concurrent writeback; if a buffer has a write error, clear it and
	 * report -EIO so the caller knows the drain didn't complete cleanly.
	 */
	if (buffer_dirty(bh)) {
		/* Buffer is dirty - it will be flushed by sync_blockdev() in
		 * the journal sync path. Just leave it marked dirty. */
	} else if (buffer_write_io_error(bh)) {
		/* A previous writeback failed; clear the error and report it. */
		clear_buffer_write_io_error(bh);
		err = -EIO;
	}

	node = (struct briefs_extent_btree_node *)bh->b_data;
	if (le32_to_cpu(node->hdr.magic) == BRIEFS_BTREE_MAGIC &&
	    !(le32_to_cpu(node->hdr.flags) & BRIEFS_BTREE_LEAF)) {
		n = le16_to_cpu(node->hdr.num_keys);
		for (i = 0; i < n; i++) {
			if (err == 0)
				err = btree_drain_subtree(sb,
					le64_to_cpu(node->u.internal.idx[i].child), cap);
		}
		if (err == 0)
			err = btree_drain_subtree(sb,
				le64_to_cpu(node->u.internal.trailing_child), cap);
	}
	brelse(bh);
	return err;
}

/*
 * briefs_btree_drain - sync all dirty btree nodes in a subtree.
 * Returns 0 on success, -EIO if any node write failed.
 * Used before journaling an inode snapshot to ensure btree nodes are on disk.
 *
 * Deliberately lock-free: this runs from the journal checkpoint path under
 * j->write_lock (and from fsync), where taking the per-inode extent_lock
 * would invert the established extent_lock -> j->write_lock order (mutators
 * journal extent records under extent_lock).  Safe because it reads node
 * headers via raw sb_bread() with NO CRC verification, so it is not exposed
 * to the torn-read -EIO surface; the split ordering (all affected buffers
 * dirty before a new root is published) is what keeps the drained set
 * complete.
 */
int briefs_btree_drain(struct super_block *sb, u64 root_block, u64 max_nodes)
{
	u64 cap = max_nodes;

	if (root_block == 0)
		return 0;
	if (cap == 0)
		cap = 1ull << 20;
	return btree_drain_subtree(sb, root_block, &cap);
}