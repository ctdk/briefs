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
#include <linux/blkdev.h>
#include <linux/workqueue.h>
#include <linux/vmalloc.h>
#include <linux/unaligned.h>
#include "briefs.h"
#include "briefs_alloc.h"

#define TRIE_ANCESTRY_LIMIT 256

/* Checkpoint policy for readdir seeks: snapshot the settled post-emit
 * iterator state every TRIE_CKPT_STRIDE emitted entries; keep at most
 * TRIE_CKPT_MAX checkpoints or TRIE_CKPT_BUDGET bytes of blob state per
 * iterator, whichever binds first.  When a cap is hit, drop every other
 * checkpoint and double the stride, so a restore is always followed by
 * < stride skip steps while the footprint halves. */
#define TRIE_CKPT_STRIDE	64
#define TRIE_CKPT_MAX		4096
#define TRIE_CKPT_BUDGET	(128 * 1024)

/* Forward decl: briefs_trie_seed_pool() (below) uses the iterator stack before
 * trie_iter_push() is defined later in this file.
 */
static int trie_iter_push(struct trie_iter *iter, u64 ref, u8 emitted);

/*
 * Initialize a deferred free list. Must be freed with trie_free_list_destroy.
 */
void trie_free_list_init(struct trie_free_list *list)
{
	list->refs = kmalloc(64 * sizeof(u64), GFP_NOFS);
	list->count = 0;
	list->cap = 64;
}

/*
 * Add a node ref to the deferred free list. Grows the list if needed.
 * Returns 0 on success, -ENOMEM on allocation failure.
 * If allocation fails, the ref is not added but the list remains valid.
 */
int trie_free_list_add(struct trie_free_list *list, u64 ref)
{
	if (list->count >= list->cap) {
		int new_cap = list->cap * 2;
		u64 *new_refs = krealloc(list->refs, new_cap * sizeof(u64), GFP_NOFS);
		if (!new_refs) {
			pr_warn_ratelimited("briefs: trie free list OOM, some nodes may leak\n");
			return -ENOMEM;
		}
		list->refs = new_refs;
		list->cap = new_cap;
	}
	list->refs[list->count++] = ref;
	return 0;
}

/*
 * Free all collected node refs and destroy the list.
 * Must be called WITHOUT trie_lock held, as briefs_trie_free_node
 * may sleep waiting for I/O.
 */
void trie_free_list_destroy(struct super_block *sb,
                             struct trie_free_list *list)
{
	int i;
	for (i = 0; i < list->count; i++) {
		briefs_trie_free_node(sb, list->refs[i]);
	}
	kfree(list->refs);
	list->refs = NULL;
	list->count = 0;
	list->cap = 0;
}

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
 * Speculative non-blocking readahead for a node's page.
 *
 * Cold directory walks are serialized by discovery: a node's
 * first_child / next_sibling references live in the node's own page, so
 * the next page of the walk is unknown until the current page has been
 * read, and reads issue strictly one at a time (~120 us each on a cold
 * cache=none device).  A cold kernel-tree find spends essentially its
 * whole wall time in that chain: 16k unmerged 4K reads, zero merging,
 * no readahead.  Submitting the read for the pages a freshly parsed
 * node points at lets the device pipeline the walk down to roughly the
 * trie's dependency depth instead of its node count.  The deeper chain
 * hops (a page's own next/sibling pages, discovered only after that
 * page arrives) are covered by the whole-directory prefetch cascade
 * below, not by this depth-1 readahead.
 *
 * Read-only and safe against concurrent modification: the readahead
 * populates the same buffer a later sb_bread() would hit, and a
 * concurrent writer dirties that same buffer, so no stale copy can be
 * served.  Worst case a prefetched page is never needed (a failed
 * lookup) and stays cached like any other read.
 */
static void trie_readahead_ref(struct super_block *sb, u64 ref)
{
	u64 block;

	if (TRIE_REF_IS_NULL(ref))
		return;

	/* Mirror briefs_trie_read_page()'s guards: never hand the block
	 * layer a block the walker itself would have rejected. */
	if (TRIE_REF_SLOT(ref) >= TRIE_SLOTS_PER_BLOCK)
		return;
	block = TRIE_REF_BLOCK(ref);
	if (!briefs_block_in_range(sb, block))
		return;

	sb_breadahead(sb, block);
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
	/* Per-call cap mirroring seed_pool/trie_iter_next: a healthy sibling
	 * chain (children of one node, at most 256 by byte_val) is far shorter
	 * than the whole-trie slot count, but a stale on-disk trie with a
	 * back-edge in next_sibling would otherwise loop here forever reading
	 * cached pages with no cond_resched, wedging the box unkillably at
	 * 100% CPU (generic/475 live-path under dm-error).  Abort with -EIO.
	 */
	u64 cap = BRIEFS_TRIE_SIBLING_MAX;
	u64 visited = 0;
	u64 prev = 0;
	u64 child;

	if (trie_read_node(sb, parent_ref, &pbh, &ppage, &pnode) != 0)
		return -EIO;

	child = trie_node_first_child(pnode);
	while (!TRIE_REF_IS_NULL(child)) {
		if (++visited > cap) {
			pr_warn("briefs: trie sibling walk exceeded %llu nodes (corrupt/cyclic trie), aborting\n",
				cap);
			brelse(pbh);
			return -EIO;
		}
		cbh = briefs_trie_read_page(sb, child, &cpage, &cnode);
		if (IS_ERR(cbh))
			break;
		if (cnode->byte_val == byte_val) {
			*child_ref = child;
			*prev_ref = prev;
			/* The descent will read this child's first_child page
			 * next; get it in flight while we still hold the CPU. */
			trie_readahead_ref(sb, trie_node_first_child(cnode));
			brelse(cbh);
			brelse(pbh);
			return 0;
		}
		prev = child;
		child = trie_node_next_sibling(cnode);
		/* Prefetch the rest of the sibling chain: without this the
		 * scan below is a serial one-read-per-sibling chain on a
		 * cold cache. */
		trie_readahead_ref(sb, child);
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
		briefs_mark_buffer_dirty(pbh, sb);
		brelse(pbh);
		return 0;
	}

	/* Walk to the last sibling. */
	{
		u64 cap = BRIEFS_TRIE_SIBLING_MAX;
		u64 visited = 0;

		last = trie_node_first_child(pnode);
		lbh = briefs_trie_get_page(sb, last, &lpage, &last_node);
		if (IS_ERR(lbh)) {
			brelse(pbh);
			return -EIO;
		}

		while (!TRIE_REF_IS_NULL(trie_node_next_sibling(last_node))) {
			u64 next = trie_node_next_sibling(last_node);

			if (++visited > cap) {
				pr_warn("briefs: trie link sibling walk exceeded %llu nodes (corrupt/cyclic trie), aborting\n",
					cap);
				brelse(lbh);
				brelse(pbh);
				return -EIO;
			}
			brelse(lbh);
			lbh = briefs_trie_get_page(sb, next, &lpage, &last_node);
			if (IS_ERR(lbh)) {
				brelse(pbh);
				return -EIO;
			}
			last = next;
		}
	}

	trie_node_set_next_sibling(last_node, child_ref);
	briefs_mark_buffer_dirty(lbh, sb);
	brelse(lbh);

	trie_node_set_child_count(pnode, trie_node_child_count(pnode) + 1);
	briefs_mark_buffer_dirty(pbh, sb);
	brelse(pbh);
	return 0;
}

/*
 * Create a child node of the given parent.  Returns child ref, or 0 on failure.
 * @hint_abs is the partial-page pool's cluster tag (the owning directory's
 * trie root block; 0 when unknown).
 */
static u64 briefs_trie_create_child(struct super_block *sb, u64 parent_ref,
                                     u8 depth, u8 byte_val, u8 node_type,
                                     size_t name_len, u64 hint_abs)
{
	u64 child_ref;
	struct buffer_head *cbh;
	struct briefs_trie_page *cpage;
	struct briefs_trie_node *cnode;

	child_ref = briefs_trie_alloc_node(sb, name_len, hint_abs);
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
	briefs_mark_buffer_dirty(cbh, sb);
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
                                             u8 depth, u8 byte_val, u8 node_type,
                                             u64 hint_abs)
{
	u64 child;

	child = briefs_trie_find_child(sb, parent_ref, byte_val);
	if (!TRIE_REF_IS_NULL(child))
		return child;
	return briefs_trie_create_child(sb, parent_ref, depth, byte_val, node_type,
	                                0, hint_abs);
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

	ret = briefs_trie_page_init(sb, 0, 0, NODE_TYPE_INTERM, &root_ref, 0);
	if (ret)
		return ret;

	/* Add the root page to the partial list immediately so future
	 * allocations can reuse its free slots.  The root page anchors its
	 * own cluster: pass its block so this directory's later pages
	 * (and nodes) prefer it over another directory's leftover pages.
	 */
	briefs_trie_page_add_partial(sb, TRIE_REF_BLOCK(root_ref),
	                              TRIE_REF_BLOCK(root_ref));
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
			/* Tag with this trie's root: replay re-derives each
			 * directory's entries with the same cluster anchor, so
			 * its re-allocated nodes prefer its own pages here
			 * too.  Pages shared across directories keep the
			 * first tag (add_partial dedups by block). */
			briefs_trie_page_add_partial(sb, TRIE_REF_BLOCK(ref),
			                              TRIE_REF_BLOCK(root_ref));
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
 * Max concurrent whole-directory trie-prefetch workers (see
 * briefs_trie_prefetch_ino_async below).  Each worker issues trie-page
 * readahead one window at a time, so this bounds the prefetch read
 * storm's queue depth.  0 disables the cascade entirely (no workqueue
 * is created).  Measured insensitive from 4 to 64 on a cache=none
 * device; 16 is the default.
 */
unsigned int trie_prefetch_depth = 16;
module_param(trie_prefetch_depth, uint, 0444);
MODULE_PARM_DESC(trie_prefetch_depth,
	"max concurrent trie prefetch workers (0 = disable whole-directory prefetch)");

/*
 * Whole-directory trie prefetch (cold-walk cascade).
 *
 * The cross-directory readdir pipeline (briefs_readahead_inode /
 * briefs_readahead_dir_trie_root in inode.c) overlaps each directory's
 * trie-root read with the walk's time in earlier siblings' subtrees, but
 * the in-directory walk stays serialized: a page's first_child /
 * next_sibling references live in the page itself, so the Nth page of a
 * directory is unknown until the (N-1)th has arrived, and depth-1
 * readahead is consumed within microseconds.  A cold kernel-tree find
 * spends essentially its whole wall time in those chains (~16k
 * unmerged 4K reads at ~120 us each).
 *
 * The chain is only serial for a reader that must return entries in
 * order.  A background thread can walk the same dependency graph while
 * the foreground walker is busy elsewhere -- earlier siblings'
 * subtrees, or simply the getdents/fts bookkeeping between a
 * subdirectory's dirent being emitted and the walker descending into
 * it.  So briefs_readdir() queues a prefetch work item at dirent-emit
 * time for every subdirectory it lists; the kworker resolves the
 * directory's trie root (reading the inode block, which the readdir
 * path only readahead'd and so cannot yet read itself) and
 * blocking-reads every page reachable from the root into the buffer
 * cache.  The foreground walk later finds those pages warm.
 *
 * Once a page is parsed the walker immediately readaheads every
 * distinct page its nodes point at, so the next discovery level
 * arrives in parallel instead of depth-first; with trie pages packed
 * in their own top-of-disk region (briefs_alloc_block_high) each
 * window is all trie pages, and the plug around the walk lets the
 * window's adjacent submissions merge at the elevator.
 *
 * Safety mirrors briefs_trie_seed_pool(): the walk is read-only, and
 * trie_read_node() validates page magic and reference bounds, so a
 * stale or torn reference under a concurrent creator/destroyer at
 * worst ends that branch.  Termination is bounded by the node and
 * page caps regardless of on-disk cycles, and destroy_workqueue() at
 * put_super drains every worker before any fs state is torn down.
 *
 * Both caps are comfortably above the largest real directories (a
 * kernel tree tops out around 3000 entries ~= 10k nodes): a walk that
 * hits them simply stops prefetching and the foreground walk falls
 * back to its own depth-1 readahead for the remainder.  A walk whose
 * root page is already cached is skipped entirely, so repeat
 * traversals of a warm tree cost one buffer probe per directory.
 */
#define TRIE_PREFETCH_MAX_NODES	16384
#define TRIE_PREFETCH_MAX_PAGES	512

struct trie_prefetch_work {
	struct work_struct work;
	struct super_block *sb;
	u64 root_ref;		/* 0: resolve from ino below */
	u64 ino;
};

/*
 * sb_breadahead a trie page exactly once per walk.  @seen is the walk's
 * set of already-handled blocks; it doubles as the backstop that keeps a
 * corrupt cycle from re-reading the same pages forever.
 *
 * Each handled page is issued as a small downward window of
 * trie_prefetch_window blocks (1 = the page alone).  Trie pages are
 * allocated top-down (briefs_alloc_block_high), so a directory's later
 * pages sit below its earlier ones; with every block in the region
 * being some directory's trie page, the window is all-but-free -- the
 * pages it pulls in are the ones neighbouring walks are about to ask
 * for anyway.  The walk's plug batches the window into one merged
 * request, collapsing a directory's scattered pages into a few 8-16K
 * reads instead of one 4K read per page.
 *
 * Volume stays bounded by the trie region itself: a block whose buffer
 * is already uptodate (another walk fetched it) or in flight (another
 * walk just issued it) is never resubmitted -- sb_breadahead would
 * happily re-read an in-flight buffer -- so overlapping windows from
 * thousands of concurrent walks converge on reading each page once.
 * Measured on a cold kernel-tree find (64G RAW cache=none): window 512
 * reads the 63MB of trie pages as ~7.7k requests (8.8k merged) in
 * ~650ms vs ~16k unmerged requests in ~1500ms with the cascade off.
 *
 * The window assumes the high-region layout: with trie pages scattered
 * through the data region (images populated by older kernels) a window
 * pulls in file data the walks never ask for.  Set 1 to disable.
 */
static unsigned int trie_prefetch_window = 512;
module_param(trie_prefetch_window, uint, 0644);
MODULE_PARM_DESC(trie_prefetch_window,
	"readahead window in 4K blocks issued per trie page during whole-directory prefetch (1 = off)");

static void trie_prefetch_issue(struct super_block *sb, u64 *seen,
				int *n_seen, u64 block)
{
	u64 b, first;
	int i;

	for (i = 0; i < *n_seen; i++)
		if (seen[i] == block)
			return;
	if (*n_seen >= TRIE_PREFETCH_MAX_PAGES)
		return;
	seen[(*n_seen)++] = block;

	first = block + 1 >= trie_prefetch_window ?
		block + 1 - trie_prefetch_window : 0;
	for (b = first; b <= block; b++) {
		struct buffer_head *bh;

		if (!briefs_block_in_range(sb, b))
			continue;
		bh = sb_getblk(sb, b);
		if (bh) {
			bool skip = buffer_uptodate(bh) || buffer_locked(bh);

			brelse(bh);
			if (skip)
				continue;
		}
		sb_breadahead(sb, b);
	}
}

static void briefs_trie_prefetch_subtree(struct super_block *sb, u64 root_ref)
{
	struct trie_iter *iter;
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	struct blk_plug plug;
	u64 ref, child, sib, nodes = 0;
	u64 *seen;
	int n_seen = 0;

	if (TRIE_REF_IS_NULL(root_ref) ||
	    TRIE_REF_SLOT(root_ref) >= TRIE_SLOTS_PER_BLOCK ||
	    !briefs_block_in_range(sb, TRIE_REF_BLOCK(root_ref)))
		return;

	/* Warm root: the tree was walked or prefetched recently and its
	 * pages are still cached -- the foreground walk will hit cache on
	 * its own, so do not burn a worker on it. */
	bh = sb_getblk(sb, TRIE_REF_BLOCK(root_ref));
	if (bh) {
		bool warm = buffer_uptodate(bh);

		brelse(bh);
		if (warm)
			return;
	}

	iter = briefs_trie_iter_alloc();
	if (!iter)
		return;
	seen = kmalloc_array(TRIE_PREFETCH_MAX_PAGES, sizeof(u64),
			    GFP_KERNEL);
	if (!seen) {
		briefs_trie_iter_free(iter);
		return;
	}

	if (trie_iter_push(iter, root_ref, 0))
		goto out;

	blk_start_plug(&plug);
	while (iter->sp > 0) {
		if (nodes >= TRIE_PREFETCH_MAX_NODES)
			break;
		ref = iter->stack[--iter->sp];
		if (TRIE_REF_IS_NULL(ref) ||
		    TRIE_REF_SLOT(ref) >= TRIE_SLOTS_PER_BLOCK ||
		    !briefs_block_in_range(sb, TRIE_REF_BLOCK(ref)))
			continue;
		trie_prefetch_issue(sb, seen, &n_seen, TRIE_REF_BLOCK(ref));
		if (trie_read_node(sb, ref, &bh, &page, &node) != 0)
			/* Unreadable page (bad magic under a concurrent
			 * mutation, or a real I/O error): drop this branch
			 * and keep prefetching the rest. */
			continue;
		nodes++;

		child = trie_node_first_child(node);
		sib = trie_node_next_sibling(node);
		brelse(bh);

		/* Burst every distinct page this node's links point at
		 * before descending, so one level arrives in parallel;
		 * a failed push (iterator stack OOM) just drops that
		 * branch -- the prefetch covers less, the foreground
		 * walk stays correct. */
		if (!TRIE_REF_IS_NULL(child) &&
		    TRIE_REF_SLOT(child) < TRIE_SLOTS_PER_BLOCK &&
		    briefs_block_in_range(sb, TRIE_REF_BLOCK(child))) {
			trie_prefetch_issue(sb, seen, &n_seen,
					    TRIE_REF_BLOCK(child));
			trie_iter_push(iter, child, 0);
		}
		if (!TRIE_REF_IS_NULL(sib) &&
		    TRIE_REF_SLOT(sib) < TRIE_SLOTS_PER_BLOCK &&
		    briefs_block_in_range(sb, TRIE_REF_BLOCK(sib))) {
			trie_prefetch_issue(sb, seen, &n_seen,
					    TRIE_REF_BLOCK(sib));
			trie_iter_push(iter, sib, 0);
		}

		if ((nodes & 1023) == 0)
			cond_resched();
	}
	blk_finish_plug(&plug);
out:
	kfree(seen);
	briefs_trie_iter_free(iter);
}

static void trie_prefetch_work_fn(struct work_struct *work)
{
	struct trie_prefetch_work *pw =
		container_of(work, struct trie_prefetch_work, work);
	u64 root = pw->root_ref;

	/* Root not yet known: the readdir path only readahead'd the inode
	 * block, so resolve the trie root here, off the critical path. */
	if (root == 0)
		root = briefs_load_dir_trie_root(pw->sb, pw->ino);
	if (root != 0)
		briefs_trie_prefetch_subtree(pw->sb, root);
	kfree(pw);
}

static void trie_prefetch_queue(struct super_block *sb, u64 root_ref, u64 ino)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct trie_prefetch_work *pw;

	if (!bsi || !bsi->trie_prefetch_wq)
		return;
	if (root_ref == 0 && ino == 0)
		return;
	pw = kmalloc(sizeof(*pw), GFP_KERNEL);
	if (!pw)
		return;
	INIT_WORK(&pw->work, trie_prefetch_work_fn);
	pw->sb = sb;
	pw->root_ref = root_ref;
	pw->ino = ino;
	queue_work(bsi->trie_prefetch_wq, &pw->work);
}

void briefs_trie_prefetch_dir_async(struct super_block *sb, u64 root_ref)
{
	if (TRIE_REF_IS_NULL(root_ref) ||
	    TRIE_REF_SLOT(root_ref) >= TRIE_SLOTS_PER_BLOCK ||
	    !briefs_block_in_range(sb, TRIE_REF_BLOCK(root_ref)))
		return;
	trie_prefetch_queue(sb, root_ref, 0);
}

/*
 * Queue a whole-directory prefetch keyed by inode number.  Called from
 * the readdir emit path, where the directory's trie root is not yet
 * readable (the inode block readahead issued moments earlier is still
 * in flight); the worker resolves it with a blocking read.
 */
void briefs_trie_prefetch_ino_async(struct super_block *sb, u64 ino)
{
	if (ino == 0)
		return;
	trie_prefetch_queue(sb, 0, ino);
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

		/* Next byte's find_child() will descend into this child's
		 * first_child page; start that read while we loop. */
		trie_readahead_ref(sb, trie_node_first_child(cnode));
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
			briefs_mark_buffer_dirty(prev_bh, sb);
			brelse(prev_bh);
		}
	}

	trie_node_set_child_count(pnode, trie_node_child_count(pnode) - 1);
	briefs_mark_buffer_dirty(pbh, sb);
	brelse(pbh);
}

/*
 * Split a pure leaf at the given position.  The leaf currently hangs off
 * parent `cur` at byte value `bval` (the new name's byte at `pos`).  On
 * return, *cur_out holds the node reference to continue descending from.
 */
static int trie_split_leaf(struct super_block *sb, u64 cur, u64 child,
                           int pos, u8 bval, const char *name, int name_len,
                           u64 ino, u8 type, u64 *cur_out, u64 hint_abs)
{
	struct buffer_head *lbh, *ibh, *gbh;
	struct briefs_trie_page *lpage, *ipage, *gpage;
	struct briefs_trie_node *lnode, *inode, *gnode;
	int old_name_len;
	int ret;
	u64 internal, old_sibling;

	if (trie_read_node(sb, child, &lbh, &lpage, &lnode) != 0)
		return -EIO;
	old_name_len = trie_node_name_len(lnode) - 2;

	if (old_name_len == pos + 1) {
		/* Old leaf is a prefix of the new name. */
		lnode->node_type = NODE_TYPE_INTERM | NODE_STATUS_LEAF;
		lnode->depth = pos + 1;
		briefs_mark_buffer_dirty(lbh, sb);
		brelse(lbh);
		*cur_out = child;
		return 0;
	}

	old_sibling = trie_node_next_sibling(lnode);
	brelse(lbh);

	internal = briefs_trie_create_child(sb, cur, pos + 1, bval,
	                                     NODE_TYPE_INTERM, 0, hint_abs);
	if (TRIE_REF_IS_NULL(internal))
		return -ENOSPC;

	/* Reparent: find link from cur to child, redirect to internal. */
	if (trie_get_node(sb, cur, &gbh, &gpage, &gnode) != 0)
		return -EIO;
	if (trie_node_first_child(gnode) == child) {
		trie_node_set_first_child(gnode, internal);
	} else {
		u64 prev = trie_node_first_child(gnode);
		u64 cap = BRIEFS_TRIE_SIBLING_MAX;
		u64 visited = 0;
		while (!TRIE_REF_IS_NULL(prev)) {
			struct buffer_head *tbh;
			struct briefs_trie_page *tpage;
			struct briefs_trie_node *tmp;
			if (++visited > cap) {
				pr_warn("briefs: trie split relink exceeded %llu nodes (corrupt/cyclic trie), aborting\n",
					cap);
				break;
			}
			if (trie_get_node(sb, prev, &tbh, &tpage, &tmp) != 0)
				break;
			if (trie_node_next_sibling(tmp) == child) {
				trie_node_set_next_sibling(tmp, internal);
				briefs_mark_buffer_dirty(tbh, sb);
				brelse(tbh);
				break;
			}
			prev = trie_node_next_sibling(tmp);
			brelse(tbh);
		}
	}
	briefs_mark_buffer_dirty(gbh, sb);
	brelse(gbh);

	/* Link old leaf as child of internal. */
	if (trie_get_node(sb, internal, &ibh, &ipage, &inode) == 0) {
		trie_node_set_first_child(inode, child);
		trie_node_set_next_sibling(inode, old_sibling);
		trie_node_set_child_count(inode, 1);
		briefs_mark_buffer_dirty(ibh, sb);
		brelse(ibh);
	}

	/* If split at last byte, store new name on internal. */
	if (pos == name_len - 1) {
		if (trie_get_node(sb, internal, &ibh, &ipage, &inode) == 0) {
			/*
			 * Store the name before committing the LEAF bit + inode.
			 * trie_store_name() can fail (-ENOSPC if the page's name
			 * heap is full, -EIO on a bad page).  Setting the LEAF bit
			 * and inode first and ignoring that error would leave a
			 * nameless leaf (LEAF + inode, name_len/name_offset == 0)
			 * that trie_lookup() can never match (elen == name_len-2
			 * == -2 != name_len), making the entry unfindable and
			 * orphaning its inode (generic/089).  On failure the split's
			 * structural change -- a new INTERM reparenting the old
			 * leaf -- is valid trie state, so just return the error and
			 * let the caller unwind the directory op.
			 */
			ret = trie_store_name(sb, internal, name, name_len);
			if (ret != 0) {
				brelse(ibh);
				return ret;
			}
			inode->node_type |= NODE_STATUS_LEAF;
			TRIE_SET_FTYPE(inode, type);
			trie_node_set_inode(inode, ino);
			briefs_mark_buffer_dirty(ibh, sb);
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
	u64 cur, child, new_leaf, existing, hint;
	int pos, ret;

	if (TRIE_REF_IS_NULL(di->dir_trie_root)) {
		ret = briefs_trie_create_root(sb, di);
		if (ret)
			return ret;
	}

	cur = di->dir_trie_root;

	/*
	 * Cluster tag: the directory's root block.  It labels the
	 * partial-page pool entries this insert creates
	 * (briefs_trie_alloc_node's tag-pass prefers a directory's own
	 * pages, so its nodes do not scatter onto other directories'
	 * leftovers) and nothing else -- fresh pages come from the
	 * top-down trie region allocator regardless (see
	 * briefs_trie_page_init).
	 */
	hint = TRIE_REF_BLOCK(di->dir_trie_root);

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

					/*
					 * Store the name before committing the
					 * LEAF bit + inode.  This existing INTERM
					 * node may have been freed and re-allocated
					 * (memset, name_len/name_offset == 0)
					 * since it last held a leaf, so
					 * trie_store_name() may need a fresh
					 * name-heap allocation that can fail
					 * (-ENOSPC, -EIO).  Ignoring the error
					 * would leave a nameless leaf (LEAF +
					 * inode set, name_len == 0) that
					 * trie_lookup() can never match,
					 * orphaning the linked inode
					 * (generic/089).  Propagate the error so
					 * the link fails cleanly instead.
					 */
					ret = trie_store_name(sb, existing, name, name_len);
					if (ret != 0) {
						brelse(cbh);
						return ret;
					}
					cnode->node_type |= NODE_STATUS_LEAF;
					TRIE_SET_FTYPE(cnode, type);
					trie_node_set_inode(cnode, ino);
					briefs_mark_buffer_dirty(cbh, sb);
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
				                     name, name_len, ino, type, &cur,
				                     hint);
				if (ret != 0)
					return ret;
				continue;
			}

			/* No existing child: create a new pure leaf. */
			new_leaf = briefs_trie_create_child(sb, cur, pos, bval, 0,
			                                   name_len, hint);
			if (TRIE_REF_IS_NULL(new_leaf))
				return -ENOSPC;

			if (trie_get_node(sb, new_leaf, &bh, &page, &node) != 0) {
				briefs_trie_free_node(sb, new_leaf);
				return -EIO;
			}
			/*
			 * Store the name before committing the leaf.  create_child()
			 * pre-reserved name-heap space for this node, so the store
			 * normally reuses that slot; but it can still fail (-EIO on a
			 * bad page).  Ignoring the error would leave a nameless leaf
			 * (LEAF + inode set, name_len == 0) that trie_lookup() can
			 * never match, orphaning the inode (generic/089).  Free the
			 * freshly created node and propagate the error.
			 */
			ret = trie_store_name(sb, new_leaf, name, name_len);
			if (ret != 0) {
				brelse(bh);
				briefs_trie_free_node(sb, new_leaf);
				return ret;
			}
			node->node_type = 0;
			TRIE_SET_FTYPE(node, type);
			trie_node_set_inode(node, ino);
			briefs_mark_buffer_dirty(bh, sb);
			brelse(bh);
			return 0;
		}

		/* Middle byte: find or create an INTERM child. */
		child = briefs_trie_find_or_create_child(sb, cur, pos + 1, bval,
		                                         NODE_TYPE_INTERM, hint);
		if (TRIE_REF_IS_NULL(child))
			return -ENOSPC;

		if (trie_read_node(sb, child, &cbh, &cpage, &cnode) != 0)
			return -EIO;
		if (!(cnode->node_type & NODE_TYPE_INTERM)) {
			brelse(cbh);
			ret = trie_split_leaf(sb, cur, child, pos, bval,
			                     name, name_len, ino, type, &cur,
			                     hint);
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
					briefs_mark_buffer_dirty(cbh, sb);
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
 *
 * This function collects node refs to free in @to_free, then returns.
 * The caller must call trie_free_list_destroy() AFTER releasing trie_lock
 * to actually free the nodes. This avoids holding trie_lock across
 * sync_dirty_buffer() I/O waits.
 */
int briefs_trie_remove(struct super_block *sb, struct briefs_inode *di,
                        const char *name, int name_len,
                        struct trie_free_list *to_free)
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
					briefs_mark_buffer_dirty(cbh, sb);
					brelse(cbh);
					brelse(bh);
					ret = 0;
					goto out;
				}
				briefs_mark_buffer_dirty(cbh, sb);
				brelse(cbh);

				trie_unlink_child(sb, cur, child_prev, child);
				trie_free_list_add(to_free, child);
				brelse(bh);
				goto collapse;
			}

			/* Pure leaf: unlink and free */
			brelse(cbh);
			trie_unlink_child(sb, cur, child_prev, child);
			trie_free_list_add(to_free, child);
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
		 *
		 * We collect nodes to free in @to_free and actually free them after
		 * releasing trie_lock to avoid holding the lock across sync_dirty_buffer.
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
				u64 cap = BRIEFS_TRIE_SIBLING_MAX;
				u64 visited = 0;
				while (!TRIE_REF_IS_NULL(w)) {
					struct buffer_head *wbh;
					struct briefs_trie_page *wpage;
					struct briefs_trie_node *wn;
					if (++visited > cap) {
						pr_warn("briefs: trie remove relink exceeded %llu nodes (corrupt/cyclic trie), aborting\n",
							cap);
						break;
					}
					if (trie_get_node(sb, w, &wbh, &wpage, &wn) != 0)
						break;
					if (trie_node_next_sibling(wn) == check) {
						trie_node_set_next_sibling(wn, trie_node_next_sibling(cn2));
						briefs_mark_buffer_dirty(wbh, sb);
						brelse(wbh);
						break;
					}
					w = trie_node_next_sibling(wn);
					brelse(wbh);
				}
			}
			trie_node_set_child_count(pn2, trie_node_child_count(pn2) - 1);
			briefs_mark_buffer_dirty(pbh2, sb);
			brelse(pbh2);

			/* Defer the actual free until after releasing trie_lock */
			trie_free_list_add(to_free, check);
			brelse(cbh2);
		}

		/*
		 * If the collapse left the root as an empty intermediate node
		 * (no children), free it too and clear the directory trie root.
		 * Otherwise an unlinked sole entry leaves the directory pointing
		 * at an empty trie page that is never referenced again.
		 *
		 * We defer the root free as well to avoid holding trie_lock across I/O.
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
					trie_free_list_add(to_free, di->dir_trie_root);
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
 *
 * This function collects all trie node references first while holding trie_lock,
 * then releases the lock and frees them. This avoids holding trie_lock across
 * sync_dirty_buffer() calls, which can sleep waiting for I/O completion.
 * Under heavy concurrent load (e.g., generic/013 fsstress), holding trie_lock
 * across I/O can cause deadlocks when multiple threads pile up waiting for
 * sync completion.
 */
void briefs_trie_free_all(struct super_block *sb, struct briefs_inode *di)
{
	struct briefs_sb_info *bsi;
	struct trie_stack_entry {
		u64 ref;
		int state;
	} *stack;
	int sp, stack_cap;

	/*
	 * Collect refs to free in a dynamically grown array. We use vmalloc
	 * because the array can be large for deep/wide directory trees.
	 */
	struct {
		u64 *refs;
		int count;
		int cap;
	} to_free = { NULL, 0, 0 };

	/*
	 * Check if sb_info is valid before starting the free. During teardown,
	 * sb->s_fs_info may be NULL. This prevents NULL pointer dereference
	 * (generic/013 crash).
	 */
	bsi = sb->s_fs_info;
	if (!bsi || !bsi->sb) {
		pr_warn_ratelimited("briefs: trie_free_all with no bsi (sb_info torn down)\n");
		return;
	}

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

	/* Phase 1: Walk the trie and collect all node refs to free */
	while (sp > 0) {
		u64 ref;
		int state;
		struct buffer_head *bh;
		struct briefs_trie_page *page;
		struct briefs_trie_node *node;

		ref = stack[sp - 1].ref;
		state = stack[sp - 1].state;

		if (state == 1) {
			/* Already visited children - just record this ref for later free */
			if (to_free.count >= to_free.cap) {
				/* Grow the array */
				int new_cap = to_free.cap == 0 ? 1024 : to_free.cap * 2;
				u64 *new_refs = vmalloc_array(new_cap, sizeof(u64));
				if (!new_refs) {
					pr_err("briefs: failed to grow trie free list\n");
					vfree(stack);
					vfree(to_free.refs);
					return;
				}
				if (to_free.refs && to_free.count > 0) {
					memcpy(new_refs, to_free.refs, to_free.count * sizeof(u64));
					vfree(to_free.refs);
				}
				to_free.refs = new_refs;
				to_free.cap = new_cap;
			}
			to_free.refs[to_free.count++] = ref;
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
			int sibling_count = 0;
			while (!TRIE_REF_IS_NULL(child)) {
				struct buffer_head *cbh;
				struct briefs_trie_page *cpage;
				struct briefs_trie_node *cn;
				u64 next;

				/* Detect potential infinite loop from corrupted sibling chain */
				if (++sibling_count > 1000) {
					pr_err("briefs: trie sibling chain too long (corrupted?) at ref %llu\n",
					       (unsigned long long)child);
					WARN_ON(1);
					break;
				}

				/* Validate the sibling ref before dereferencing */
				if (briefs_validate_trie_ref(sb, child) != 0) {
					pr_err("briefs: invalid trie ref %llu in sibling chain\n",
					       (unsigned long long)child);
					break;
				}

				if (trie_read_node(sb, child, &cbh, &cpage, &cn) != 0)
					break;
				next = trie_node_next_sibling(cn);

				/* Detect cycles: next should not point back to current or earlier sibling */
				if (next == child) {
					pr_err("briefs: trie sibling cycle detected at ref %llu\n",
					       (unsigned long long)child);
					WARN_ON(1);
					brelse(cbh);
					break;
				}

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

	/*
	 * Phase 2: Free all collected refs. This is done AFTER releasing trie_lock
	 * (the caller already released it before calling us, or we're in evict where
	 * no concurrent access is possible). Each free may call sync_dirty_buffer
	 * which can sleep waiting for I/O.
	 *
	 * Note: briefs_trie_free_node expects to be called without trie_lock held
	 * when used from this deferred free path. The trie structure is already
	 * fully torn down at this point - we're just freeing the blocks.
	 */
	{
		int i;
		for (i = 0; i < to_free.count; i++) {
			briefs_trie_free_node(sb, to_free.refs[i]);
		}
		vfree(to_free.refs);
	}
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
	iter->ckpts = NULL;
	iter->ckpt_cnt = 0;
	iter->ckpt_cap = 0;
	iter->ckpt_stride = TRIE_CKPT_STRIDE;
	iter->ckpt_bytes = 0;
	return iter;
}

/*
 * briefs_trie_iter_free - free a trie iterator and its dynamic stack.
 */
void briefs_trie_iter_free(struct trie_iter *iter)
{
	if (!iter)
		return;
	briefs_trie_ckpt_clear(iter);
	kfree(iter->ckpts);
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
	iter->visited = 0;
	iter->visit_cap = 0;
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

	/* Lazily compute the cyclic-trie backstop cap (mirrors seed_pool):
	 * a healthy trie visits far fewer than this many nodes; a stale
	 * on-disk trie with a back-edge in first_child/next_sibling would
	 * otherwise re-push forever and wedge the mount (generic/475).
	 * Hitting the cap aborts the walk with -EIO instead of hanging.
	 */
	if (!iter->visit_cap) {
		struct briefs_sb_info *bsi = sb->s_fs_info;

		iter->visit_cap = bsi->alloc.block_count *
				  TRIE_SLOTS_PER_BLOCK + 1024;
	}

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

		if (iter->visited > iter->visit_cap) {
			pr_warn("briefs: trie iter exceeded %llu visited nodes (corrupt/cyclic trie), aborting\n",
				iter->visit_cap);
			return -EIO;
		}

		ref = iter->stack[iter->sp - 1];
		emitted = iter->leaf_emitted[iter->sp - 1];
		iter->sp--;

		if (trie_read_node(sb, ref, &bh, &page, &node) != 0)
			continue;
		iter->visited++;

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
					/* This leaf is re-pushed to descend into
					 * its children later; start reading the
					 * first child's page now. */
					trie_readahead_ref(sb, trie_node_first_child(node));
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

			/* First read of the loop below would block on a cold
			 * cache; get it started before we go around. */
			trie_readahead_ref(sb, child);

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
					iter->visited++;
					if (iter->visited > iter->visit_cap) {
						pr_warn("briefs: trie iter push exceeded %llu visited nodes (corrupt/cyclic trie), aborting\n",
							iter->visit_cap);
						brelse(cbh);
						brelse(bh);
						return -EIO;
					}
					child = trie_node_next_sibling(cn);
					/* Keep the sibling chain AND the subtree
					 * descent in flight: each parsed node
					 * speculatively starts the read for
					 * the two pages it points at.  Without
					 * this a cold walk serializes on every
					 * trie page in the directory. */
					trie_readahead_ref(sb, child);
					trie_readahead_ref(sb, trie_node_first_child(cn));
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

/*
 * briefs_trie_ckpt_clear - drop all checkpoints, resetting the policy to
 * its initial stride.  The ckpts[] pointer array is kept for reuse.
 */
void briefs_trie_ckpt_clear(struct trie_iter *iter)
{
	int i;

	for (i = 0; i < iter->ckpt_cnt; i++) {
		kfree(iter->ckpts[i]->refs);
		kfree(iter->ckpts[i]->flags);
		kfree(iter->ckpts[i]);
	}
	iter->ckpt_cnt = 0;
	iter->ckpt_bytes = 0;
	iter->ckpt_stride = TRIE_CKPT_STRIDE;
}

/*
 * trie_ckpt_thin - drop every other checkpoint and double the stride.
 * Preserves the gap invariant "adjacent live checkpoints <= ckpt_stride"
 * (gaps double as the stride doubles), so a restore is still followed by
 * < stride skip steps.
 */
static void trie_ckpt_thin(struct trie_iter *iter)
{
	int i, keep;

	if (iter->ckpt_cnt < 2)
		return;

	for (i = 1; i < iter->ckpt_cnt; i += 2) {
		iter->ckpt_bytes -= sizeof(struct trie_ckpt) +
				    (size_t)iter->ckpts[i]->sp * 9;
		kfree(iter->ckpts[i]->refs);
		kfree(iter->ckpts[i]->flags);
		kfree(iter->ckpts[i]);
	}
	keep = 0;
	for (i = 0; i < iter->ckpt_cnt; i += 2)
		iter->ckpts[keep++] = iter->ckpts[i];
	iter->ckpt_cnt = keep;
	iter->ckpt_stride *= 2;
}

/*
 * briefs_trie_ckpt_record - snapshot the current settled walk state as a
 * checkpoint, if policy allows.  Called after each successful
 * briefs_trie_iter_next() whose entry was actually emitted (pending == false,
 * dir.c owns emit_idx).  Best-effort: on ENOMEM or a full budget the
 * checkpoint is skipped and seeks degrade to the full-skip path.
 */
void briefs_trie_ckpt_record(struct trie_iter *iter)
{
	struct trie_ckpt *cp;
	size_t size;

	/* Only the settled post-emit state is checkpointable: while an entry
	 * is pending, the walk has already consumed it and the stack sits
	 * one entry ahead of emit_idx. */
	if (iter->pending)
		return;

	if (iter->emit_idx % iter->ckpt_stride)
		return;

	/* Append-only keeps ckpts[] sorted and duplicate-free.  The walk
	 * state at a given (gen, emit_idx) is deterministic, so re-recording
	 * a position already held (e.g. re-crossed boundaries after a
	 * restore) is never needed. */
	if (iter->ckpt_cnt &&
	    iter->ckpts[iter->ckpt_cnt - 1]->emit_idx >= iter->emit_idx)
		return;

	size = sizeof(*cp) + (size_t)iter->sp * 9;
	if (iter->ckpt_cnt >= TRIE_CKPT_MAX ||
	    iter->ckpt_bytes + size > TRIE_CKPT_BUDGET) {
		trie_ckpt_thin(iter);
		if (iter->ckpt_cnt >= TRIE_CKPT_MAX ||
		    iter->ckpt_bytes + size > TRIE_CKPT_BUDGET)
			return;	/* nothing to thin / blob alone over budget */
	}

	if (iter->ckpt_cnt == iter->ckpt_cap) {
		struct trie_ckpt **grown;
		int newcap = iter->ckpt_cap ? iter->ckpt_cap * 2 : 16;

		grown = krealloc(iter->ckpts, newcap * sizeof(*grown),
				GFP_KERNEL);
		if (!grown)
			return;
		iter->ckpts = grown;
		iter->ckpt_cap = newcap;
	}

	cp = kmalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return;

	cp->refs = NULL;
	cp->flags = NULL;
	if (iter->sp) {
		cp->refs = kmalloc_array(iter->sp, sizeof(u64), GFP_KERNEL);
		cp->flags = kmalloc(iter->sp, GFP_KERNEL);
		if (!cp->refs || !cp->flags) {
			kfree(cp->refs);
			kfree(cp->flags);
			kfree(cp);
			return;
		}
		memcpy(cp->refs, iter->stack, iter->sp * sizeof(u64));
		memcpy(cp->flags, iter->leaf_emitted, iter->sp);
	}

	cp->emit_idx = iter->emit_idx;
	cp->gen = iter->gen;
	cp->sp = iter->sp;

	iter->ckpts[iter->ckpt_cnt++] = cp;
	iter->ckpt_bytes += size;
}

/*
 * trie_ckpt_find - binary search for the checkpoint with the largest
 * emit_idx <= target, or NULL if none.
 */
static struct trie_ckpt *trie_ckpt_find(struct trie_iter *iter, u64 target)
{
	int lo = 0, hi = iter->ckpt_cnt - 1, best = -1;

	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;

		if (iter->ckpts[mid]->emit_idx <= target) {
			best = mid;
			lo = mid + 1;
		} else {
			hi = mid - 1;
		}
	}
	return best >= 0 ? iter->ckpts[best] : NULL;
}

/*
 * trie_ckpt_restore - restore the walk state from a checkpoint.  Returns
 * false (and leaves the iterator alone) if the blob does not fit the
 * current stack; the caller falls back to a root re-walk.
 */
static bool trie_ckpt_restore(struct trie_iter *iter, struct trie_ckpt *cp)
{
	/* iter->cap only ever grows and the blob was captured from this same
	 * iterator, so cp->sp always fits; guard anyway in case that
	 * invariant is ever broken. */
	if (cp->sp > iter->cap)
		return false;

	if (cp->sp) {
		memcpy(iter->stack, cp->refs, cp->sp * sizeof(u64));
		memcpy(iter->leaf_emitted, cp->flags, cp->sp);
	}
	iter->sp = cp->sp;
	iter->emit_idx = cp->emit_idx;
	iter->gen = cp->gen;
	iter->pending = false;
	return true;
}

/*
 * trie_iter_skip - advance the walk `steps` real entries, recording
 * checkpoints at stride boundaries as we go.  Stops early (seek past EOF)
 * on any briefs_trie_iter_next() error, leaving emit_idx at the number of
 * entries actually consumed, exactly as the old dir.c skip loop did.
 */
static void trie_iter_skip(struct trie_iter *iter, struct super_block *sb,
			   u64 current_gen, u64 steps)
{
	char name[BRIEFS_NAME_LEN + 1];
	u64 k;

	for (k = 0; k < steps; k++) {
		if (briefs_trie_iter_next(sb, iter, current_gen,
					  NULL, NULL, name, NULL) != 0)
			break;	/* seek past EOF */
		iter->emit_idx++;
		briefs_trie_ckpt_record(iter);
	}
}

/*
 * briefs_trie_iter_seek - position the iterator so its next yield is the
 * `target`-th real entry.  Replaces the old always-re-init-and-skip-from-
 * root seek in briefs_readdir, which cost O(target) walks per out-of-line
 * seek (quadratic under generic/676's random-seek phases).
 */
void briefs_trie_iter_seek(struct trie_iter *iter, struct super_block *sb,
			   struct briefs_inode *di, u64 current_gen,
			   u64 target)
{
	struct trie_ckpt *cp;
	u64 cur;

	/* `visited` is a cyclic-trie backstop only; a long-lived fd doing
	 * many seeks would otherwise ratchet it toward visit_cap across
	 * calls and abort a healthy trie with a bogus -EIO. */
	iter->visited = 0;

	if (iter->gen != current_gen) {
		/* Rewinddir-after-growth (generic/471) or any add/remove/
		 * rename since the last call: everything cached about the
		 * old trie -- including every checkpoint -- is invalid. */
		briefs_trie_ckpt_clear(iter);
		briefs_trie_iter_init(iter, di, current_gen);
		iter->emit_idx = 0;
		trie_iter_skip(iter, sb, current_gen, target);
		return;
	}

	/* While an entry sits in the pending buffer the walk has already
	 * consumed it; fold its index into emit_idx so the invariant
	 * "emit_idx == index of the next entry _next yields" holds (the
	 * buffered entry is abandoned, exactly what a re-init would do). */
	cur = iter->emit_idx;
	if (iter->pending) {
		iter->pending = false;
		iter->emit_idx++;
		cur = iter->emit_idx;
	}

	if (target == cur)
		return;

	if (target > cur) {
		/* Forward seek: continue from the current position; no root
		 * restart (previously even forward seeks re-walked from 0).
		 * At EOF the first _next returns -ENOENT: an O(1) empty read,
		 * same output as today's re-init-and-walk-to-EOF. */
		trie_iter_skip(iter, sb, current_gen, target - cur);
		return;
	}

	/* Backward seek: restore the nearest checkpoint at or below the
	 * target, then skip the remainder (< ckpt_stride steps). */
	cp = trie_ckpt_find(iter, target);
	if (cp && trie_ckpt_restore(iter, cp)) {
		trie_iter_skip(iter, sb, current_gen, target - iter->emit_idx);
		return;
	}

	/* No usable checkpoint below the target: re-walk from the root.
	 * Existing checkpoints stay put; they are still valid for this
	 * generation.  Position 0 is a virtual checkpoint (the state
	 * briefs_trie_iter_init just built). */
	briefs_trie_iter_init(iter, di, current_gen);
	iter->emit_idx = 0;
	trie_iter_skip(iter, sb, current_gen, target);
}
