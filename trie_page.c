/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

/*
 * BrieFS packed directory trie page allocator and helpers.
 *
 * Starting with BrieFS 0.7.0, directory trie nodes live in packed 4 KiB
 * pages allocated from the data-block allocator.  Each page has a header,
 * a fixed-size array of node slots (TRIE_SLOTS_PER_BLOCK), and a shared
 * variable-length name heap that grows upward from the end of the block.
 *
 * Node references are 64-bit values:
 *   ref = (absolute_block << TRIE_SLOT_BITS) | slot_index
 * with ref == 0 meaning "null".  Slots are numbered 0..TRIE_SLOTS_PER_BLOCK-1
 * inside each page; slot 0 is the root node for that page.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include "briefs.h"
#include "briefs_alloc.h"
#include "briefs_journal.h"

/*
 * Per-superblock state tracking partially-full trie pages.  We use a mutex
 * (not a spinlock) because the I/O helpers can sleep.
 */
struct briefs_trie_pages {
	struct list_head partial;       /* pages with at least one free slot */
	struct mutex lock;                /* protects the list and page metadata */
	u64 hot_block;                  /* recently used partial page (fast path) */
};

/*
 * We hang a private state off the superblock so we don't need to change
 * the briefs_sb_info layout in this patch.  A static per-sb lookup keyed by
 * the VFS superblock pointer keeps the change localized.  Future work can add
 * this struct directly to briefs_sb_info.
 */
struct briefs_trie_sb_state {
	struct list_head list;          /* global list of all states */
	struct super_block *sb;
	struct briefs_trie_pages pages;
};

/* List entry stored on the partial page list. */
struct briefs_trie_page_entry {
	struct list_head list;
	u64 block;
};

static LIST_HEAD(briefs_trie_states);
static DEFINE_SPINLOCK(briefs_trie_states_lock);

static struct briefs_trie_sb_state *briefs_trie_get_state(struct super_block *sb)
{
	struct briefs_trie_sb_state *st;

	spin_lock(&briefs_trie_states_lock);
	list_for_each_entry(st, &briefs_trie_states, list) {
		if (st->sb == sb) {
			spin_unlock(&briefs_trie_states_lock);
			return st;
		}
	}
	spin_unlock(&briefs_trie_states_lock);
	return NULL;
}

/* Number of partial trie pages currently cached for @sb. Used by the debugfs
 * "trie_pool" file as a quick gauge of trie-page allocator pressure. 0 if this
 * sb has no trie state yet (e.g. no directory has been modified). */
unsigned int briefs_trie_pool_depth(struct super_block *sb)
{
	struct briefs_trie_sb_state *st;
	struct list_head *p;
	unsigned int depth = 0;

	st = briefs_trie_get_state(sb);
	if (!st)
		return 0;

	mutex_lock(&st->pages.lock);
	list_for_each(p, &st->pages.partial)
		depth++;
	mutex_unlock(&st->pages.lock);
	return depth;
}

static struct briefs_trie_sb_state *briefs_trie_ensure_state(struct super_block *sb)
{
	struct briefs_trie_sb_state *st;

	st = briefs_trie_get_state(sb);
	if (st)
		return st;

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st)
		return NULL;

	st->sb = sb;
	INIT_LIST_HEAD(&st->pages.partial);
	mutex_init(&st->pages.lock);
	st->pages.hot_block = 0;

	spin_lock(&briefs_trie_states_lock);
	list_add(&st->list, &briefs_trie_states);
	spin_unlock(&briefs_trie_states_lock);

	return st;
}

void briefs_trie_cleanup_state(struct super_block *sb)
{
	struct briefs_trie_sb_state *st;
	struct briefs_trie_page_entry *entry, *tmp;

	st = briefs_trie_get_state(sb);
	if (!st)
		return;

	spin_lock(&briefs_trie_states_lock);
	list_del(&st->list);
	spin_unlock(&briefs_trie_states_lock);

	mutex_lock(&st->pages.lock);
	list_for_each_entry_safe(entry, tmp, &st->pages.partial, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	mutex_unlock(&st->pages.lock);

	kfree(st);
}

/* Slot offset within a page buffer. */
static inline struct briefs_trie_node *trie_slot_at(void *page_base, u64 slot)
{
	return (struct briefs_trie_node *)
		((char *)page_base + sizeof(struct briefs_trie_page) +
		 slot * sizeof(struct briefs_trie_node));
}

/*
 * Read a node by reference.  Returns the buffer head (caller must brelse),
 * with *page and *node set to pointers into the mapped buffer.
 */
struct buffer_head *briefs_trie_read_page(struct super_block *sb, u64 node_ref,
                                          struct briefs_trie_page **page,
                                          struct briefs_trie_node **node)
{
	struct buffer_head *bh;
	u64 block, slot;

	if (TRIE_REF_IS_NULL(node_ref))
		return ERR_PTR(-EINVAL);

	block = TRIE_REF_BLOCK(node_ref);
	slot = TRIE_REF_SLOT(node_ref);
	if (slot >= TRIE_SLOTS_PER_BLOCK)
		return ERR_PTR(-EINVAL);

	/* A stale/corrupt trie ref can decode to a block past the device end.
	 * sb_bread() of such a block busy-loops unkillably (grow_buffers returns
	 * NULL, __bread_gfp retries forever with no signal-check point), so reject
	 * it here instead of looping the whole box into an unkillable wedge.
	 */
	if (!briefs_block_in_range(sb, block))
		return ERR_PTR(-EIO);

	bh = sb_bread(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);

	*page = (struct briefs_trie_page *)bh->b_data;
	*node = trie_slot_at(bh->b_data, slot);

	if (trie_page_magic(*page) != BRIEFS_TRIE_PAGE_MAGIC) {
		pr_err("briefs: trie page %llu has bad magic 0x%08x\n",
		       block, trie_page_magic(*page));
		brelse(bh);
		return ERR_PTR(-EIO);
	}

	return bh;
}

/*
 * Get a writable page by reference.  Same as read, but uses sb_getblk.
 */
struct buffer_head *briefs_trie_get_page(struct super_block *sb, u64 node_ref,
                                         struct briefs_trie_page **page,
                                         struct briefs_trie_node **node)
{
	struct buffer_head *bh;
	u64 block, slot;

	if (TRIE_REF_IS_NULL(node_ref))
		return ERR_PTR(-EINVAL);

	block = TRIE_REF_BLOCK(node_ref);
	slot = TRIE_REF_SLOT(node_ref);
	if (slot >= TRIE_SLOTS_PER_BLOCK)
		return ERR_PTR(-EINVAL);

	if (!briefs_block_in_range(sb, block))
		return ERR_PTR(-EIO);

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
	}

	*page = (struct briefs_trie_page *)bh->b_data;
	*node = trie_slot_at(bh->b_data, slot);

	if (trie_page_magic(*page) != BRIEFS_TRIE_PAGE_MAGIC) {
		pr_err("briefs: trie page %llu has bad magic 0x%08x\n",
		       block, trie_page_magic(*page));
		brelse(bh);
		return ERR_PTR(-EIO);
	}

	return bh;
}

/*
 * Allocate a data block and initialize it as a new trie page with the
 * first slot set up as requested.  Returns the node reference for slot 0,
 * or 0 on failure.
 */
int briefs_trie_page_init(struct super_block *sb, u8 depth, u8 byte_val,
                          u8 node_type, u64 *out_ref)
{
	struct briefs_sb_info *bsi = sb->s_fs_info;
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	u64 rel, block;

	*out_ref = 0;

	/*
	 * During journal replay, reuse a trie-block that pass-1 reserved from a
	 * JRN_TRIE_ALLOC record instead of re-allocating from the data allocator.
	 * Re-allocating would (a) -ENOSPC on a full fs -- the blocks are already
	 * reserved and thus invisible to briefs_alloc_block()'s free-scan
	 * (generic/475), and (b) risk aliasing a later-reserved data block.  The
	 * FIFO holds exactly the trie blocks the journal recorded; popping one in
	 * replay page_init order yields a self-consistent re-derived trie (parent
	 * child-pointers reference whatever block we return) even though the
	 * physical block number need not match the live path's assignment.  If the
	 * FIFO is empty (more page_inits than recorded allocs -- should not happen
	 * with pool seeding + the unbounded scan, see briefs_trie_alloc_node),
	 * fall back to the normal allocator, preserving today's behaviour.
	 */
	if (bsi->journal && bsi->journal->in_replay &&
	    briefs_journal_replay_pop_trie_block(bsi->journal, &rel) == 0) {
		/* reused a reserved trie block */
	} else {
		rel = briefs_alloc_block(&bsi->alloc);
	}
	if (rel == 0)
		return -ENOSPC;
	block = data_to_abs(bsi->sb, rel);

	bh = briefs_get_zero_block(sb, block);
	if (!bh) {
		briefs_free_block(&bsi->alloc, rel);
		return -EIO;
	}

	page = (struct briefs_trie_page *)bh->b_data;
	trie_page_set_magic(page, BRIEFS_TRIE_PAGE_MAGIC);
	trie_page_set_version(page, 1);
	trie_page_set_live_count(page, 1);
	trie_page_set_free_name_off(page, 0);
	trie_page_set_free_slots(page, ~1ULL);       /* slot 0 allocated; rest free */

	node = trie_slot_at(bh->b_data, 0);
	node->depth = depth;
	node->byte_val = byte_val;
	node->node_type = node_type;

	/*
	 * Make the freshly-initialized trie page durable on disk immediately.
	 * briefs_get_zero_block() left the buffer dirty in the page cache; without
	 * this sync the page's content (magic, slot 0, ...) is only written back
	 * lazily by pdflush.  If a crash hits before that writeback, the block is
	 * reserved on replay (JRN_TRIE_ALLOC below records only the block NUMBER)
	 * but its on-disk content is stale/garbage, so replay_dir_update()'s
	 * briefs_trie_insert() reads a page with bad magic and fails -ENOSPC
	 * (generic/065: "trie page N has bad magic 0xaaaaaaaa").  Syncing here
	 * guarantees every allocated trie page has a valid initialized form on
	 * disk before any later journal record (DIR_UPDATE) that traverses into it
	 * can become durable; replay then re-derives the entries on top of a valid
	 * page.  This is the single chokepoint for new trie-page block allocation.
	 * Safe in syscall context (mkdir/create/split) and during replay: a metadata
	 * buffer sync, not pagecache writeback, so it cannot trip the mmap/writeback
	 * AB-BA of generic/074.  See briefs_journal_replay()'s pre-scan pass, which
	 * reserves all file-data extent blocks before re-derivation runs, so this
	 * sync'd trie page cannot alias a later-reserved data block.
	 *
	 * NOTE: During journal replay, we MUST sync to ensure the page is on disk
	 * before replay continues. During normal operation, we can skip the sync
	 * and let writeback happen asynchronously - the journal record will carry
	 * the trie page data if needed for recovery.
	 */
	/*
	 * The underlying device can fail a synchronous write (dm-thin pool
	 * exhaustion after its no_space_timeout, a failing loop backing store,
	 * etc.).  During journal replay we sync the page to disk before replay
	 * continues (see NOTE above); route that sync through
	 * briefs_sync_dirty_buffer() so a write failure is quiesced (clearing
	 * dirty+EIO so pdflush does not later write stale trie content to the
	 * freed block and a future re-allocation via briefs_get_zero_block() --
	 * which memsets + set_buffer_uptodate + re-dirties -- does not WARN) and
	 * handled per the errors= policy.  In normal operation the sync is
	 * skipped (async writeback; the journal record carries the page data for
	 * recovery), but a stale write-error flag from a prior failed writeback
	 * is still quiesced and handled here so the caller (briefs_trie_alloc_node)
	 * does not re-mark this buffer dirty and trip the kernel write-error WARN.
	 * Either way, free the block and propagate -EIO so the caller unwinds
	 * (does not insert a dir entry pointing at the zeroed page).
	 */
	if (bsi->journal && bsi->journal->in_replay) {
		if (briefs_sync_dirty_buffer(bh, sb, "trie page init")) {
			briefs_free_block(&bsi->alloc, rel);
			brelse(bh);
			return -EIO;
		}
	} else if (briefs_check_meta_write_error(bh)) {
		briefs_handle_meta_write_error(sb, "trie page init");
		briefs_free_block(&bsi->alloc, rel);
		brelse(bh);
		return -EIO;
	}

	/* Journal the trie page allocation so recovery knows this block is in use.
	 * Skip during replay: this block came from a JRN_TRIE_ALLOC record already
	 * in the journal, and appending a fresh record would advance write_pos into
	 * the range still being replayed, clobbering unprocessed records.
	 */
	if (!bsi->journal || !bsi->journal->in_replay)
		briefs_journal_trie_alloc(bsi->journal, block);

	brelse(bh);

	*out_ref = TRIE_MAKE_REF(block, 0);
	return 0;
}

/*
 * Byte offset where the name heap starts (end of slot array).
 */
static inline u16 trie_page_data_end(void)
{
	return sizeof(struct briefs_trie_page) +
		TRIE_SLOTS_PER_BLOCK * sizeof(struct briefs_trie_node);
}

/*
 * Does a trie page have room for another node (a free slot AND name-heap
 * space)?  This is the inverse of the "page became full" check in
 * trie_alloc_from_block(), i.e. the predicate for "this page belongs in the
 * partial-page pool".  Exposed so journal replay can seed the pool from the
 * on-disk trie (briefs_trie_seed_pool).
 */
bool briefs_trie_page_has_room(struct briefs_trie_page *page)
{
	return trie_page_free_slots(page) != 0 &&
	       trie_page_free_name_off(page) <
			(BRIEFS_BLOCK_SIZE - trie_page_data_end());
}

/*
 * Find a free slot in a page.  Does not touch the name heap.
 */
static int trie_page_alloc_slot(struct briefs_trie_page *page, u64 *out_slot)
{
	u64 slot;

	for (slot = 0; slot < TRIE_SLOTS_PER_BLOCK; slot++) {
		if (trie_page_free_slots(page) & (1ULL << slot))
			break;
	}
	if (slot >= TRIE_SLOTS_PER_BLOCK)
		return -ENOSPC;

	trie_page_set_free_slots(page, trie_page_free_slots(page) & ~(1ULL << slot));
	trie_page_set_live_count(page, trie_page_live_count(page) + 1);
	*out_slot = slot;
	return 0;
}

/*
 * trie_page_compact_names - reclaim dead name-heap space on a trie page.
 *
 * The name heap is a bump allocator whose high-water mark (free_name_off) grows
 * from the end of the block toward the slot array and is never decremented when
 * a node is freed: briefs_trie_free_node() memsets the freed node (clearing its
 * name_len/name_offset) but leaves its name bytes orphaned above the live
 * names.  Under create/delete-heavy directories (e.g. generic/089's concurrent
 * link/unlink churn) the heap therefore fills with dead space from freed nodes
 * while only a handful of names stay live, until a fresh allocation fails
 * -ENOSPC even though most of the heap is unused.
 *
 * Compaction rewrites every live node's name compactly from the end of the
 * block, reclaiming all dead space, and resets free_name_off to the live total.
 * Only the name heap is repacked; node references (block, slot) and trie
 * structure are untouched, so this is safe under the per-directory trie_lock
 * that serializes all trie access.  The page buffer is marked dirty so the
 * compacted layout is persisted regardless of whether the triggering allocation
 * ultimately succeeds (the compacted heap is a valid state either way).
 *
 * Called lazily from trie_page_alloc_name() only when a fresh allocation would
 * otherwise fail -ENOSPC, so directories with little churn never pay for it.
 */
static void trie_page_compact_names(struct buffer_head *bh, struct super_block *sb)
{
	struct briefs_trie_page *page = (struct briefs_trie_page *)bh->b_data;
	char *base = (char *)page;
	u16 old_off = trie_page_free_name_off(page);
	u64 live_slots = ~trie_page_free_slots(page);
	char *tmp;
	u16 tmp_used = 0;
	u16 new_off = 0;
	int slot;

	if (old_off == 0)
		return;

	tmp = kmalloc(old_off, GFP_NOFS);
	if (!tmp)
		return;	/* OOM: leave the heap untouched; caller fails -ENOSPC */

	/*
	 * Pass 1: copy each live node's name into the scratch buffer, in slot
	 * order.  A freed slot (bit set in free_slots) was memset and has
	 * name_offset == 0; its orphaned name bytes are the dead space skipped
	 * here and reclaimed below.  A live INTERM-only node also has
	 * name_offset == 0 (it stores no name) and is skipped.  Validate each
	 * name against the heap bounds; on any anomaly (corrupt node) abort
	 * without modifying the page so the caller's -ENOSPC surfaces a real
	 * error rather than scrambling the heap.
	 */
	for (slot = 0; slot < TRIE_SLOTS_PER_BLOCK; slot++) {
		struct briefs_trie_node *n;
		u16 noff, nlen;

		if (!(live_slots & (1ULL << slot)))
			continue;		/* free slot */
		n = trie_slot_at(base, slot);
		noff = trie_node_name_offset(n);
		nlen = trie_node_name_len(n);
		if (noff == 0 || nlen == 0)
			continue;		/* live node without a stored name */
		if (noff > old_off || nlen > noff || tmp_used + nlen > old_off) {
			kfree(tmp);
			return;		/* corrupt heap; leave untouched */
		}
		memcpy(tmp + tmp_used, base + BRIEFS_BLOCK_SIZE - noff, nlen);
		tmp_used += nlen;
	}

	/*
	 * Pass 2: rewrite the names compactly from the end of the block,
	 * re-iterating in the same slot order so each name is read from its
	 * scratch-buffer position.  The compact region lies within the old
	 * heap, but writes draw from the scratch buffer, so no live name is
	 * clobbered before it is copied.
	 */
	tmp_used = 0;
	for (slot = 0; slot < TRIE_SLOTS_PER_BLOCK; slot++) {
		struct briefs_trie_node *n;
		u16 noff, nlen;

		if (!(live_slots & (1ULL << slot)))
			continue;
		n = trie_slot_at(base, slot);
		noff = trie_node_name_offset(n);
		nlen = trie_node_name_len(n);
		if (noff == 0 || nlen == 0)
			continue;
		new_off += nlen;
		trie_node_set_name_offset(n, new_off);
		memcpy(base + BRIEFS_BLOCK_SIZE - new_off, tmp + tmp_used, nlen);
		tmp_used += nlen;
	}

	trie_page_set_free_name_off(page, new_off);
	briefs_mark_buffer_dirty(bh, sb);
	kfree(tmp);
}

/*
 * Allocate name-heap space for a node.  The slot must already be allocated.
 * If the node already has a name allocation that is large enough, reuse it;
 * otherwise grow the heap.  On a full heap, compact dead name space first and
 * retry before returning -ENOSPC.  Returns 0 on success, negative on error.
 */
static int trie_page_alloc_name(struct buffer_head *bh, struct super_block *sb,
				struct briefs_trie_page *page,
				struct briefs_trie_node *node,
				size_t name_len)
{
	u16 data_end = trie_page_data_end();
	u16 name_size = name_len + 2;   /* 2-byte length prefix + name bytes */
	u16 name_base;

	if (name_len == 0)
		return 0;

	if (name_len > BRIEFS_NAME_LEN)
		return -ENAMETOOLONG;

	/* If an existing allocation is already large enough, reuse it. */
	if (trie_node_name_offset(node) > 0 && trie_node_name_len(node) >= 2 + name_len)
		return 0;

	name_base = BRIEFS_BLOCK_SIZE - trie_page_free_name_off(page);
	if (name_base - name_size < data_end) {
		/*
		 * Heap full against the slot array.  free_name_off is a monotonic
		 * high-water mark never decremented on free, so freed nodes'
		 * name bytes accumulate as dead space.  Compact the heap to
		 * reclaim it, then recheck; if still no room, the heap is
		 * genuinely full of live names.
		 */
		trie_page_compact_names(bh, sb);
		name_base = BRIEFS_BLOCK_SIZE - trie_page_free_name_off(page);
		if (name_base - name_size < data_end)
			return -ENOSPC;
	}

	trie_page_set_free_name_off(page, trie_page_free_name_off(page) + name_size);
	trie_node_set_name_offset(node, trie_page_free_name_off(page));
	trie_node_set_name_len(node, 2 + name_len);
	return 0;
}

/*
 * Return true if the page has enough free name heap for a name of size name_size.
 */
static inline bool trie_page_has_name_heap(struct briefs_trie_page *page, u16 name_size)
{
	if (name_size == 0)
		return true;
	return trie_page_free_name_off(page) + name_size <=
		BRIEFS_BLOCK_SIZE - trie_page_data_end();
}

/*
 * Add a page to the partial list.  Caller must hold pages->lock.
 */
static void __trie_page_add_partial_locked(struct briefs_trie_pages *pages, u64 block)
{
	struct briefs_trie_page_entry *entry;
	bool found = false;

	list_for_each_entry(entry, &pages->partial, list) {
		if (entry->block == block) {
			found = true;
			break;
		}
	}
	if (!found) {
		entry = kmalloc(sizeof(*entry), GFP_KERNEL);
		if (entry) {
			entry->block = block;
			list_add(&entry->list, &pages->partial);
		}
	}
}

/*
 * Add a page to the partial list so future allocations can reuse its slots.
 * Safe to call multiple times.
 */
void briefs_trie_page_add_partial(struct super_block *sb, u64 block)
{
	struct briefs_trie_sb_state *st = briefs_trie_ensure_state(sb);
	struct briefs_trie_pages *pages;

	if (!st)
		return;
	pages = &st->pages;

	mutex_lock(&pages->lock);
	__trie_page_add_partial_locked(pages, block);
	if (pages->hot_block == 0)
		pages->hot_block = block;
	mutex_unlock(&pages->lock);
}

/*
 * Allocate a new trie node somewhere in any trie page.
 *
 * name_len is the actual name length (not including the 2-byte prefix).  Pass 0
 * for internal/root nodes that have no name.  Returns a node reference, or 0
 * on failure.
 */
/*
 * Try to allocate a node from a specific block.  Caller must hold pages->lock.
 * Returns a node reference on success, 0 on failure.
 */
static u64 trie_alloc_from_block(struct super_block *sb, struct briefs_trie_pages *pages,
                                  u64 block, u16 name_size)
{
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	u64 slot;
	u64 ref = 0;

	bh = sb_getblk(sb, block);
	if (!bh)
		return 0;
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
	}

	page = (struct briefs_trie_page *)bh->b_data;
	if (trie_page_magic(page) != BRIEFS_TRIE_PAGE_MAGIC) {
		brelse(bh);
		return 0;
	}

	if (!trie_page_has_name_heap(page, name_size)) {
		brelse(bh);
		return 0;
	}

	if (trie_page_alloc_slot(page, &slot) != 0) {
		brelse(bh);
		return 0;
	}

	node = trie_slot_at(bh->b_data, slot);
	memset(node, 0, sizeof(*node));
	if (name_size > 0) {
		trie_page_set_free_name_off(page, trie_page_free_name_off(page) + name_size);
		trie_node_set_name_len(node, name_size);
		trie_node_set_name_offset(node, trie_page_free_name_off(page));
	}
	set_buffer_uptodate(bh);
	briefs_mark_buffer_dirty(bh, sb);
	ref = TRIE_MAKE_REF(block, slot);

	if (trie_page_free_slots(page) == 0 ||
	    trie_page_free_name_off(page) >=
	    BRIEFS_BLOCK_SIZE - trie_page_data_end()) {
		/* Page became full; hot_block will be cleared below. */
		ref |= (1ULL << 63);
	}

	brelse(bh);
	return ref;
}

u64 briefs_trie_alloc_node(struct super_block *sb, size_t name_len)
{
	struct briefs_trie_sb_state *st;
	struct briefs_trie_pages *pages;
	struct briefs_trie_page_entry *entry, *tmp;
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	u64 ref = 0;
	u16 name_size;

	st = briefs_trie_ensure_state(sb);
	if (!st)
		return 0;
	pages = &st->pages;

	name_size = (name_len > 0) ? (name_len + 2) : 0;

	mutex_lock(&pages->lock);

	/*
	 * Fast path: try the cached hot page first.  Most allocations reuse
	 * the same page repeatedly, so this avoids a full list scan.
	 */
	if (pages->hot_block != 0) {
		ref = trie_alloc_from_block(sb, pages, pages->hot_block, name_size);
		if (ref != 0) {
			if (ref & (1ULL << 63)) {
				/* Hot page became full. */
				pages->hot_block = 0;
				ref &= ~(1ULL << 63);
			}
			mutex_unlock(&pages->lock);
			return ref;
		}
		pages->hot_block = 0;
	}

	/*
	 * Scan the partial page list for a page with both a free slot and
	 * enough name heap.  Bound the scan so a very long partial list does
	 * not stall every allocation; if no candidate is found within the
	 * limit, allocate a fresh page.
	 */
	{
		int scanned = 0;
		/*
		 * Bound the scan so a very long partial list does not stall every
		 * allocation.  During journal replay the pool is seeded from the
		 * on-disk trie and the scan must be UNBOUNDED: a large directory can
		 * have more than 64 partial pages, and a bounded scan could miss the
		 * page live reused (forcing a fresh page_init -> re-alloc -> -ENOSPC on
		 * a full fs, generic/475).  Replay is one-shot and single-threaded, so
		 * an unbounded scan is acceptable.
		 */
		struct briefs_sb_info *bsi = sb->s_fs_info;
		const int max_scan = (bsi && bsi->journal && bsi->journal->in_replay)
				     ? INT_MAX : 64;

		list_for_each_entry_safe(entry, tmp, &pages->partial, list) {
			if (scanned++ >= max_scan)
				break;

			ref = trie_alloc_from_block(sb, pages, entry->block, name_size);
			if (ref == 0)
				continue;

			if (ref & (1ULL << 63)) {
				/* Page became full. */
				list_del(&entry->list);
				kfree(entry);
				ref &= ~(1ULL << 63);
			} else {
				pages->hot_block = entry->block;
			}

			mutex_unlock(&pages->lock);
			return ref;
		}
	}

	mutex_unlock(&pages->lock);

	/* No partial page had room.  Allocate a fresh page where slot 0 is the
	 * requested new node.  Add it to the partial list for future reuse. */
	if (briefs_trie_page_init(sb, 0, 0, 0, &ref) != 0)
		return 0;

	if (name_size > 0) {
		bh = briefs_trie_get_page(sb, ref, &page, &node);
		if (IS_ERR(bh))
			return 0;
		/* Fresh page always has room for the name. */
		trie_page_set_free_name_off(page, trie_page_free_name_off(page) + name_size);
		trie_node_set_name_len(node, name_size);
		trie_node_set_name_offset(node, trie_page_free_name_off(page));
		briefs_mark_buffer_dirty(bh, sb);
		brelse(bh);
	}

	briefs_trie_page_add_partial(sb, TRIE_REF_BLOCK(ref));
	mutex_lock(&pages->lock);
	pages->hot_block = TRIE_REF_BLOCK(ref);
	mutex_unlock(&pages->lock);
	return ref;
}

/*
 * Store a name into a node's allocated name heap space.  Allocates space if
 * needed.  The caller must dirty the buffer head after calling.
 */
int briefs_trie_node_store_name(struct super_block *sb, u64 node_ref,
                                const char *name, size_t name_len)
{
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	char *dest;
	int ret;

	if (name_len > BRIEFS_NAME_LEN)
		return -ENAMETOOLONG;

	bh = briefs_trie_get_page(sb, node_ref, &page, &node);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	ret = trie_page_alloc_name(bh, sb, page, node, name_len);
	if (ret != 0) {
		brelse(bh);
		return ret;
	}

	if (name_len == 0) {
		trie_node_set_name_len(node, 0);
		trie_node_set_name_offset(node, 0);
		brelse(bh);
		return 0;
	}

	dest = TRIE_NODE_NAME_BASE(bh->b_data, node);
	put_unaligned_le16((u16)name_len, dest - 2);
	memcpy(dest, name, name_len);
	trie_node_set_name_len(node, 2 + name_len);

	briefs_mark_buffer_dirty(bh, sb);
	brelse(bh);
	return 0;
}

/*
 * Free a trie node.  If the page becomes empty, return the data block to the
 * allocator.  If it has free space, add it to the partial list.
 *
 * The per-superblock pages lock is held while updating the page header and
 * the partial list so that a page cannot be freed while another thread is
 * allocating from it.
 */
void briefs_trie_free_node(struct super_block *sb, u64 node_ref)
{
	struct briefs_trie_sb_state *st;
	struct buffer_head *bh;
	struct briefs_trie_page *page;
	struct briefs_trie_node *node;
	struct briefs_trie_pages *pages;
	struct briefs_trie_page_entry *entry, *tmp;
	u64 block, slot;
	bool page_empty;

	if (TRIE_REF_IS_NULL(node_ref))
		return;

	block = TRIE_REF_BLOCK(node_ref);
	slot = TRIE_REF_SLOT(node_ref);

	bh = briefs_trie_get_page(sb, node_ref, &page, &node);
	if (IS_ERR(bh))
		return;

	/*
	 * The per-superblock partial-page pool is built lazily by live trie
	 * mutations (create_root / alloc_node / free).  A directory whose trie
	 * was created on a previous mount and read back from disk by iget() is
	 * NOT in this mount's pool: nothing re-seeds the pool for it, and if the
	 * dir is only removed (not otherwise modified) on this mount the pool
	 * may not even exist yet.  Freeing a trie node must not depend on the
	 * pool -- the live_count and free-slots bitmap live in the on-disk page
	 * header, and the block must be returned to the allocator when the page
	 * empties or it leaks as an fsck orphan ("allocated but not referenced").
	 * The pool is consulted only for the allocation-cache bookkeeping below,
	 * which is skipped when no pool exists.  briefs_trie_free_all() already
	 * validated this page's magic via trie_read_node() before calling here,
	 * so a stale ref pointing at non-trie data never reaches this point.
	 */
	st = briefs_trie_get_state(sb);
	pages = st ? &st->pages : NULL;

	if (pages)
		mutex_lock(&pages->lock);

	if (!(trie_page_free_slots(page) & (1ULL << slot))) {
		trie_page_set_free_slots(page, trie_page_free_slots(page) | (1ULL << slot));
		trie_page_set_live_count(page, trie_page_live_count(page) - 1);
		memset(node, 0, sizeof(*node));
		briefs_mark_buffer_dirty(bh, sb);
	}

	page_empty = (trie_page_live_count(page) == 0);

	if (pages) {
		if (!page_empty && trie_page_free_slots(page) != 0)
			__trie_page_add_partial_locked(pages, block);

		if (page_empty) {
			list_for_each_entry_safe(entry, tmp, &pages->partial, list) {
				if (entry->block == block) {
					list_del(&entry->list);
					kfree(entry);
					break;
				}
			}
			if (pages->hot_block == block)
				pages->hot_block = 0;
		}

		mutex_unlock(&pages->lock);
	}

	if (page_empty) {
		struct briefs_sb_info *bsi = sb->s_fs_info;

		/*
		 * Sync the now-empty page header to disk before returning the
		 * block to the allocator.  This prevents a later allocation from
		 * seeing stale trie metadata in the on-disk block.  A failing
		 * device (dm-error/dm-thin) can fail this write; route it through
		 * briefs_sync_write_buffer() (mark+sync) so the now-empty page is
		 * written before the block is freed and the dirty+EIO flags are
		 * quiesced (the block is being freed, and its next allocation memset +
		 * re-dirties via briefs_get_zero_block(), so a stale on-disk
		 * header is overwritten before any reader sees it) and the error
		 * is handled per the errors= policy.  Warn rather than abort the
		 * directory op: discard the returned -EIO and continue.
		 */
		briefs_sync_write_buffer(bh, sb, "trie free");
		brelse(bh);

		/*
		 * Check if bsi is valid before journaling/freeing. During teardown
		 * or after certain errors, sb->s_fs_info may be NULL. The block has
		 * already been zeroed above, so skipping the journal/free is safe
		 * (the block will be cleared on next allocation anyway).
		 * This prevents NULL pointer dereference (generic/013 crash).
		 */
		if (!bsi || !bsi->sb) {
			pr_warn_ratelimited("briefs: trie free with no bsi (sb_info torn down)\n");
			return;
		}

		/* Journal the trie page free so recovery does not leave it allocated. */
		briefs_journal_trie_free(bsi->journal, block);
		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
	} else {
		brelse(bh);
	}
}
