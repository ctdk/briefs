/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_IOMAP_H
#define _BRIEFS_COMPAT_IOMAP_H

/*
 * compat/compat-iomap.h - stable wrappers around the iomap entry
 * points whose prototypes changed between v6.12 and current upstream.
 *
 * v6.16 threaded a "private" parameter through zero_range,
 * truncate_page and page_mkwrite (buffered_write already carried one
 * from v6.10); v6.17 then threaded a "write_ops" parameter through the
 * buffered write and zeroing helpers (page_mkwrite was left at the
 * v6.16 three-argument shape).  BrieFS passes NULL for all of them, as
 * in-tree filesystems without write_begin hooks do.  v6.19 moved the
 * read path behind the void-returning iomap_bio_read_* compat inlines;
 * v6.17 also collapsed iomap_writepages() to a single writepage-context
 * argument that now carries the mapping, wbc and writeback ops.
 *
 * Every wrapper takes the v6.12 argument list (plus the ops pointer
 * where the old core reached it indirectly) and routes to the right
 * prototype per kernel, so the BrieFS call sites never change shape
 * again.
 */

#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/uio.h>

#if BRIEFS_HAS_IOMAP_WRITE_OPS
static inline ssize_t
briefs_compat_file_buffered_write(struct kiocb *iocb, struct iov_iter *from,
				   const struct iomap_ops *ops)
{
	return iomap_file_buffered_write(iocb, from, ops, NULL, NULL);
}
#else
static inline ssize_t
briefs_compat_file_buffered_write(struct kiocb *iocb, struct iov_iter *from,
				   const struct iomap_ops *ops)
{
	return iomap_file_buffered_write(iocb, from, ops, NULL);
}
#endif

#if BRIEFS_HAS_IOMAP_WRITE_OPS
static inline int
briefs_compat_zero_range(struct inode *inode, loff_t pos, loff_t len,
			 bool *did_zero, const struct iomap_ops *ops)
{
	return iomap_zero_range(inode, pos, len, did_zero, ops, NULL, NULL);
}

static inline int
briefs_compat_truncate_page(struct inode *inode, loff_t pos, bool *did_zero,
			    const struct iomap_ops *ops)
{
	return iomap_truncate_page(inode, pos, did_zero, ops, NULL, NULL);
}
#elif BRIEFS_HAS_IOMAP_PRIVATE
static inline int
briefs_compat_zero_range(struct inode *inode, loff_t pos, loff_t len,
			 bool *did_zero, const struct iomap_ops *ops)
{
	return iomap_zero_range(inode, pos, len, did_zero, ops, NULL);
}

static inline int
briefs_compat_truncate_page(struct inode *inode, loff_t pos, bool *did_zero,
			    const struct iomap_ops *ops)
{
	return iomap_truncate_page(inode, pos, did_zero, ops, NULL);
}
#else
static inline int
briefs_compat_zero_range(struct inode *inode, loff_t pos, loff_t len,
			 bool *did_zero, const struct iomap_ops *ops)
{
	return iomap_zero_range(inode, pos, len, did_zero, ops);
}

static inline int
briefs_compat_truncate_page(struct inode *inode, loff_t pos, bool *did_zero,
			    const struct iomap_ops *ops)
{
	return iomap_truncate_page(inode, pos, did_zero, ops);
}
#endif

#if BRIEFS_HAS_IOMAP_PRIVATE
static inline vm_fault_t
briefs_compat_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops)
{
	return iomap_page_mkwrite(vmf, ops, NULL);
}
#else
static inline vm_fault_t
briefs_compat_page_mkwrite(struct vm_fault *vmf, const struct iomap_ops *ops)
{
	return iomap_page_mkwrite(vmf, ops);
}
#endif

/*
 * v6.19 reworked the iomap read path behind new prototypes
 * (iomap_read_folio now takes an iomap_read_folio_ctx and returns
 * void); the kernel provides void iomap_bio_read_folio(folio, ops) and
 * iomap_bio_readahead(rac, ops) compat inlines for exactly the old
 * call shape.  Keep BrieFS's aops wrappers' return types stable.
 */
#if BRIEFS_HAS_IOMAP_BIO_READ
static inline int
briefs_compat_read_folio(struct folio *folio, const struct iomap_ops *ops)
{
	iomap_bio_read_folio(folio, ops);
	return 0;
}

static inline void
briefs_compat_readahead(struct readahead_control *rac,
			const struct iomap_ops *ops)
{
	iomap_bio_readahead(rac, ops);
}
#else
static inline int
briefs_compat_read_folio(struct folio *folio, const struct iomap_ops *ops)
{
	return iomap_read_folio(folio, ops);
}

static inline void
briefs_compat_readahead(struct readahead_control *rac,
			const struct iomap_ops *ops)
{
	iomap_readahead(rac, ops);
}
#endif /* BRIEFS_HAS_IOMAP_BIO_READ */

/*
 * v6.17 collapsed iomap_writepages() to a single wpc argument that
 * carries the inode, wbc and writeback ops (the old 4-argument form
 * took the mapping and wbc separately).  Both sides build the
 * writepage context the same way their respective cores expect; the
 * >= 6.17 initializer mirrors fs/gfs2/aops.c gfs2_writepages().
 */
static inline int
briefs_compat_writepages(struct address_space *mapping,
			 struct writeback_control *wbc,
			 const struct iomap_writeback_ops *ops)
{
#if BRIEFS_HAS_IOMAP_WRITEBACK_RANGE
	struct iomap_writepage_ctx wpc = {
		.inode = mapping->host,
		.wbc = wbc,
		.ops = ops,
	};

	return iomap_writepages(&wpc);
#else
	struct iomap_writepage_ctx wpc = { };

	return iomap_writepages(mapping, wbc, &wpc, ops);
#endif
}

#endif /* _BRIEFS_COMPAT_IOMAP_H */