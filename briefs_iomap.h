/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * briefs_iomap.h - iomap-based file data path for BrieFS.
 *
 * Translates the BrieFS extent index into struct iomap, the buffer_head-free
 * file data path.  See briefs_iomap.c for the translation; metadata (inode
 * table, directory tries, btree index nodes, allocator bitmaps, journal) stays
 * on buffer_head and is untouched.
 */
#ifndef BRIEFS_IOMAP_H
#define BRIEFS_IOMAP_H

#include <linux/iomap.h>

/*
 * briefs_iomap_ops - report / read mapping.  Never allocates a block and never
 * converts an unwritten extent: a read or fiemap sees unwritten extents as
 * IOMAP_UNWRITTEN (-> FIEMAP_EXTENT_UNWRITTEN) and gaps as IOMAP_HOLE.
 */
extern const struct iomap_ops briefs_iomap_ops;

/*
 * briefs_write_iomap_ops - write mapping.  Allocates a block on a write miss
 * and converts an unwritten extent to written in place (no split, no free),
 * mirroring briefs_get_block.  Wired in Phase 3.
 */
extern const struct iomap_ops briefs_write_iomap_ops;

#endif /* BRIEFS_IOMAP_H */
