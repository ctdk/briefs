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
#include "briefs.h"
#include "briefs_alloc.h"

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

	bh = sb_bread(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);

	*page = (struct briefs_trie_page *)bh->b_data;
	*node = trie_slot_at(bh->b_data, slot);

	if ((*page)->magic != BRIEFS_TRIE_PAGE_MAGIC) {
		pr_err("briefs: trie page %llu has bad magic 0x%08x\n",
		       block, (*page)->magic);
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

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);
	if (!buffer_mapped(bh)) {
		bh->b_blocknr = block;
		set_buffer_mapped(bh);
	}

	*page = (struct briefs_trie_page *)bh->b_data;
	*node = trie_slot_at(bh->b_data, slot);

	if ((*page)->magic != BRIEFS_TRIE_PAGE_MAGIC) {
		pr_err("briefs: trie page %llu has bad magic 0x%08x\n",
		       block, (*page)->magic);
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

	rel = briefs_alloc_block(&bsi->alloc);
	if (rel == 0)
		return -ENOSPC;
	block = data_to_abs(bsi->sb, rel);

	bh = briefs_get_zero_block(sb, block);
	if (!bh) {
		briefs_free_block(&bsi->alloc, rel);
		return -EIO;
	}

	page = (struct briefs_trie_page *)bh->b_data;
	page->magic = BRIEFS_TRIE_PAGE_MAGIC;
	page->version = 1;
	page->live_count = 1;
	page->free_name_off = 0;
	page->free_slots = ~1ULL;       /* slot 0 allocated; rest free */

	node = trie_slot_at(bh->b_data, 0);
	node->depth = depth;
	node->byte_val = byte_val;
	node->node_type = node_type;

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
 * Find a free slot in a page.  Does not touch the name heap.
 */
static int trie_page_alloc_slot(struct briefs_trie_page *page, u64 *out_slot)
{
	u64 slot;

	for (slot = 0; slot < TRIE_SLOTS_PER_BLOCK; slot++) {
		if (page->free_slots & (1ULL << slot))
			break;
	}
	if (slot >= TRIE_SLOTS_PER_BLOCK)
		return -ENOSPC;

	page->free_slots &= ~(1ULL << slot);
	page->live_count++;
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
	if (node->name_offset > 0 && node->name_len >= 2 + name_len)
		return 0;

	name_base = BRIEFS_BLOCK_SIZE - page->free_name_off;
	if (name_base - name_size < data_end)
		return -ENOSPC;

	page->free_name_off += name_size;
	node->name_offset = page->free_name_off;
	node->name_len = 2 + name_len;
	return 0;
}

/*
 * Return true if the page has enough free name heap for a name of size name_size.
 */
static inline bool trie_page_has_name_heap(struct briefs_trie_page *page, u16 name_size)
{
	if (name_size == 0)
		return true;
	return page->free_name_off + name_size <=
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
	if (page->magic != BRIEFS_TRIE_PAGE_MAGIC) {
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
		page->free_name_off += name_size;
		node->name_len = name_size;
		node->name_offset = page->free_name_off;
	}
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	ref = TRIE_MAKE_REF(block, slot);

	if (page->free_slots == 0 ||
	    page->free_name_off >=
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
		const int max_scan = 64;

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
		page->free_name_off += name_size;
		node->name_len = name_size;
		node->name_offset = page->free_name_off;
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
		node->name_len = 0;
		node->name_offset = 0;
		brelse(bh);
		return 0;
	}

	dest = TRIE_NODE_NAME_BASE(bh->b_data, node);
	*(__u16 *)(dest - 2) = (__u16)name_len;
	memcpy(dest, name, name_len);
	node->name_len = 2 + name_len;

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

	if (!(page->free_slots & (1ULL << slot))) {
		page->free_slots |= (1ULL << slot);
		page->live_count--;
		memset(node, 0, sizeof(*node));
		mark_buffer_dirty(bh);
	}

	page_empty = (page->live_count == 0);

	if (!page_empty && page->free_slots != 0)
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
		 * seeing stale trie metadata in the on-disk block.
		 */
		sync_dirty_buffer(bh);
		brelse(bh);
		briefs_free_block(&bsi->alloc, abs_to_data(bsi->sb, block));
	} else {
		brelse(bh);
	}
}
