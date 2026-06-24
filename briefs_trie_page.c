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
	if (block >= (bdev_nr_bytes(sb->s_bdev) >> sb->s_blocksize_bits))
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

	if (block >= (bdev_nr_bytes(sb->s_bdev) >> sb->s_blocksize_bits))
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
	 */
	sync_dirty_buffer(bh);

	/*
	 * The underlying device can fail this synchronous write (dm-thin pool
	 * exhaustion after its no_space_timeout, a failing loop backing store,
	 * etc.).  Without checking, BrieFS would journal + reference a block
	 * whose on-disk content is still zero, so every later traversal that
	 * reads it logs "trie page N has bad magic 0x0" and the dir op fails;
	 * the caller (briefs_trie_alloc_node) would also re-mark this buffer
	 * dirty, tripping the kernel's mark_buffer_dirty() write-error WARN.
	 * Detect the error (clearing dirty+EIO via the shared helper so pdflush
	 * does not later write stale trie content to the freed block and a future
	 * re-allocation via briefs_get_zero_block() -- which memsets +
	 * set_buffer_uptodate + re-dirties -- does not WARN), free the block, and
	 * propagate -EIO so the caller unwinds (does not insert a dir entry
	 * pointing at the zeroed page).
	 */
	if (briefs_check_meta_write_error(bh)) {
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
 * Allocate name-heap space for a node.  The slot must already be allocated.
 * If the node already has a name allocation that is large enough, reuse it;
 * otherwise grow the heap.  Returns 0 on success, negative on error.
 */
static int trie_page_alloc_name(struct briefs_trie_page *page,
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
	if (name_base - name_size < data_end)
		return -ENOSPC;

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
	mark_buffer_dirty(bh);
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
		mark_buffer_dirty(bh);
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

	ret = trie_page_alloc_name(page, node, name_len);
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

	mark_buffer_dirty(bh);
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

	st = briefs_trie_get_state(sb);
	if (!st)
		return;
	pages = &st->pages;

	block = TRIE_REF_BLOCK(node_ref);
	slot = TRIE_REF_SLOT(node_ref);

	bh = briefs_trie_get_page(sb, node_ref, &page, &node);
	if (IS_ERR(bh))
		return;

	mutex_lock(&pages->lock);

	if (!(trie_page_free_slots(page) & (1ULL << slot))) {
		trie_page_set_free_slots(page, trie_page_free_slots(page) | (1ULL << slot));
		trie_page_set_live_count(page, trie_page_live_count(page) - 1);
		memset(node, 0, sizeof(*node));
		mark_buffer_dirty(bh);
	}

	page_empty = (trie_page_live_count(page) == 0);

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

	if (page_empty) {
		struct briefs_sb_info *bsi = sb->s_fs_info;

		/*
		 * Sync the now-empty page header to disk before returning the
		 * block to the allocator.  This prevents a later allocation from
		 * seeing stale trie metadata in the on-disk block.  A failing
		 * device (dm-error/dm-thin) can fail this write; clear the
		 * dirty+EIO flags (the block is being freed, and its next
		 * allocation memset+re-dirties via briefs_get_zero_block(), so a
		 * stale on-disk header is overwritten before any reader sees it)
		 * and warn rather than abort the directory op.
		 */
		sync_dirty_buffer(bh);
		if (briefs_check_meta_write_error(bh))
			pr_warn("briefs: trie free: page %llu header write failed\n",
				block);
		brelse(bh);
		/* Journal the trie page free so recovery does not leave it allocated. */
		briefs_journal_trie_free(bsi->journal, block);
		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
	} else {
		brelse(bh);
	}
}
