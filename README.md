BrieFS!
======

![The BrieFS logo - a pelican wearing briefs (aka The Pelican's Briefs)](images/briefs-logo-1.png)

_"If you want a picture of the future, imagine a pelican standing on a human head -- for ever."_

BrieFS: A Linux filesystem that solely uses [extents](https://en.wikipedia.org/wiki/Extent_(file_systems)) for file data, [tries](https://en.wikipedia.org/wiki/Trie) for storing directory contents, and bitmap pyramids for inode and block allocation.

**NB:** This filesystem is experimental and may totally flip out, make your machine hang and stop responding, corrupt your data, corrupt your children, annoy you greatly, or just up and crash. Caution is advised. I must state that I've warned you about potential issues and that I'm not responsible if anything bad happens because you were messing around with this. Also, no warranty as per the LICENSE files.

INSTALLATION
------------

First and foremost, clone, untar, or what have you this repository on your machine somewhere convenient. Then, make sure all of the kernel development packages you'll need are installed. You'll also need to set up the kernel build directory one way or another. If you're using the distro kernel, simply installing the `linux-headers` (or equivalent) package should be enough. If you're building your own, you'll need to set the KDIR environment variable for `make`.

Once you've either installed the kernel header package or built your custom kernel, building the BrieFS module should be straightforward. Go into the BrieFS source directory and run

```
$ make # or possibly KDIR=/path/to/the/kernel/build make
```

and you should have the `briefs_fs.ko` module ready and waiting.

USAGE
-----

Load the kernel module, most likely with `sudo insmod /path/to/briefs_fs.ko` unless you put it in your kernel's module tree, in which case you can run `sudo insmod briefs_fs`.

The BrieFS kernel module isn't very useful without a filesystem. If you haven't already, install the [briefs-utils](https://github.com/ctdk/briefs-utils) and create a filesystem:

```
$ sudo mkfs.briefs /dev/xvdb1 # or whatever
```

Obviously you can skip that step on the off chance you already have a BrieFS image laying around.

Once you have a BrieFS formatted volume, just mount it the usual way with

```
$ sudo mount -t briefs /dev/xvdb1 /mnt # or whatever
```

and you're on your way. If your BrieFS volume gets messed up, `fsck.briefs` is there to help you. It works the way you would expect an fsck program to work, but refer to the `briefs-utils` documentation for specific options.

### Mount options

BrieFS recognizes the following filesystem-specific mount options:

* `debug` / `nodebug` — create (or remove) the per-superblock debugfs tree and
  enable runtime stat counters. `nodebug` is the default.
* `norecovery` — skip journal replay on mount. This is useful for read-only
  inspection or repair of a dirty volume, but it requires a read-only mount
  (`-o ro,norecovery`). A read-write mount with `norecovery` is rejected.
* `errors=continue|remount-ro|panic` — behaviour on a metadata write error.
  `remount-ro` (the default) remounts the filesystem read-only. `continue`
  logs the error and keeps running read-write. `panic` triggers a kernel
  panic. The active policy is visible in `/proc/mounts` and debugfs.

RELATED
-------

* [github.com/ctdk/briefs-utils](https://github.com/ctdk/briefs-utils): The briefs utilities, written in Golang, composed of `mkfs.briefs`, `fsck.briefs`, and `fuse.briefs`. `mkfs.briefs` creates BrieFS volumes, `fsck.briefs` checks and repairs BrieFS volumes, and `fuse.briefs` provides a read-only FUSE bridge for those same BrieFS volumes.
* [github.com/ctdk/modern-xiafs](https://github.com/ctdk/modern-xiafs): Computer filesystem archaeology. A port of an ancient Linux filesystem to modern kernels, updated as I get around to it.

RATIONALE
---------

Another Linux filesystem. Why, exactly?

There are multiple reasons that may nor may not be particularly good ones. First, I've found filesystems to be interesting for a long time as shown by my work with the Xiafs port to modern kernels. It's informative and illuminating, but not very useful for modern systems. Second, for a long time I've wondered about using trie data structures in a filesystem. (TrieFS doesn't make much sense, though, so the name is BrieFS.) Third, I believe a relatively simple but modern filesystem with well annotated source code could be a useful teaching tool; filesystems are something that in my experience you only understand if you already understand filesystems. Fourth, once I finally buckled to pressure and decided to try out AI assisted coding I thought making a relatively simple extents only filesystem would be a good and fun learning project.

Finally, and most importantly of all, the mental image of a pelican wearing briefs is funny.

AI Usage
--------

Since trying out AI assisted coding was part of the reason for this in the first place, it follows that an AI was used. It was not allowed to write code willy-nilly, however; I would review its proposals or changes and adjust them as needed rather than blindly trusting what it produced.

SUPPORTED KERNEL VERSIONS
-------------------------

For certain values of "support", anyway. As of this writing, all BrieFS development is being done using the Debian Linux kernel version 6.12.48 or 6.12.90 (in other words, the default on trixie when I set things up). Once it gets far enough along, it will jump up to track the current `linux` git repo. Other kernel versions and specific distro kernels may come as time and interest permit and dictate.

BUGS
----

It's not even entirely finished, so yeah. It does stuff, but it's still at a point that a list of what *does* work would be more informative.

WORKS
-----

* Kernel module loading
* Kernel module unloading
* Mounting a filesystem created by `mkfs.briefs`.
* Unmounting a filesystem.
* Scanning a filesystem with fsck, reporting errors, and repairing them.
* Listing the root directory.
* touching files
* mkdir creates directories
* file lookup (the exact implementation is subject to change)
* reading and writing files works with both inline and external extents
* B+ tree extent index for files with more than eight inline extents (BrieFS 0.9.0+)
* unlink and rename work
* chown and chmod work
* fsync implemented
* bitmap pyramids for data block and inode allocation
* statfs
* journal replay on mount
* truncate
* fallocate pre-allocation and punch-hole (FALLOC_FL_PUNCH_HOLE)
* cp -r works properly
* symlinks and other special files
* a set of tests
* rm -r works
* Directory trie synchronization
* Stale iterator detection
* Packed directory trie pages (BrieFS 0.7.0+), storing up to 64 nodes per 4 KiB block
* inode reuse
* CRC32C checksum verification for journal records and B+ tree extent index nodes
* Files larger than 2 GiB
* Nanosecond-precision timestamps
* Extended attributes (user, trusted, and security namespaces; no POSIX ACLs)
* Direct I/O (O_DIRECT)

DEFINITELY MISSING OR BROKEN
----------------------------

* The journal is a **logical/metadata journal**, not a block-image one. It records and replays allocator changes, inode updates, directory changes, trie-page allocations, and symlink data, but it does **not** journal B+ tree extent-index *node structure*: a torn extent-index split is not reconstructed by replay. Instead it relies on ordered durable writes — every B+ tree index block the inode snapshot references is drained (synced) before the `JRN_INODE_FULL` record is written, so the journaled snapshot never points at a not-yet-on-disk tree block. If that ordering is violated by a crash the journal can't cover, `fsck.briefs --repair-only=btree-rebuild` can repair the damage offline.
* Data journaling is not implemented. Metadata (allocations, inode updates, directory changes, etc.) is journaled and replayed on mount, but ordinary file writeback goes through the page cache and is only durable after `sync`/`fsync`/flush. A crash after a buffered write but before flush may lose data. This is the same trade-off most filesystems make by default; full data journaling is not currently planned.
* No POSIX ACLs, quotas, reflink/COW, fscrypt, fsverity, or online resize. Extended attributes (user, trusted, and security namespaces) and direct I/O are supported.
* FUSE implementation (requires less commitment than the kernel module). A read-only FUSE bridge exists via briefs-utils; read-write FUSE is not yet implemented.
* Thorough annotations - Annotating the source code thoroughly will wait until things settle down. Right now everything's still in constant flux, so there's no point thoroughly annotating something that may change unrecognizably or flat out disappear soon.
* Refactoring. Since BrieFS is partly a project to learn about using AI assistance while coding, even though I've been reviewing what it's doing there's definitely some weirdness and clunkiness that needs to be gussied up and organized so it's easier to understand. This will go nicely hand in hand with the annotation project above.

LIMITS
------

Where the on-disk fields are wide enough that they aren't the binding constraint, the practical limits come from the VFS ceilings or from allocator/encoding widths:

* **Block size** is effectively fixed at 4096 bytes. `mkfs.briefs -b` accepts any power of two, but the kernel module hardcodes `sb_set_blocksize(sb, 4096)` and the on-disk layout assumes 4096 throughout, so only 4096 actually mounts.
* **File size** is bounded by the VFS ceiling `MAX_LFS_FILESIZE` (2^63−1 bytes on 64-bit, ~8 EiB), set as `s_maxbytes`. The allocator returns ENOSPC long before this binds on any real volume. Inline data is at most 256 bytes, and a file uses up to 8 inline extents before spilling into the B+ tree extent index.
* **Volume size** is limited by directory-trie node references, which encode the block number in the top 58 bits of a 64-bit word (`block << 6 | slot`). That gives a practical ceiling of 2^58 blocks, i.e. 2^58 × 4096 = 2^70 bytes (~1 ZiB). The on-disk `total_blocks` is a full 64-bit field (2^64 blocks theoretical), and `mkfs.briefs` imposes no upper cap, only a minimum.
* **Directories** have no per-directory entry cap (no ext2-style 32000 limit). A trie page holds 64 nodes per 4 KiB and chains on demand, bounded only by free space. Entry names are limited to `NAME_MAX` (255 bytes).
* **Hard links**: the on-disk link count is a 32-bit field (~4.29×10^9), well beyond the VFS `LINK_MAX` (65000). A directory's link count is 2 plus its subdirectory count.
* **Symbolic links**: targets up to 256 bytes are stored inline in the inode; longer targets use ordinary data extents, up to the file-size limit.
* **Inodes**: the inode number is a 64-bit field. `mkfs.briefs` provisions one inode per `inode-ratio` blocks (default 8, minimum 100), so a freshly made filesystem has `totalBlocks / inode-ratio` inodes available.
* **Timestamps**: on disk, 64-bit seconds plus 64-bit nanoseconds, at 1 ns granularity (`s_time_gran = 1`). BrieFS does not set `s_time_min`/`s_time_max`, so the VFS defaults apply (`TIME64_MIN` … `TIME64_MAX`, ±(2^63−1) seconds from the epoch, ~±292 billion years). BrieFS never clamps timestamps, so values outside the representable range are rejected by the VFS rather than silently wrapped.
* **Journal**: a fixed-size metadata-only ring, 64 blocks by default (`mkfs.briefs -j`), with a separate checkpoint block. There is no data journaling.

LICENSE
-------

BrieFS is dual licensed under the MIT license or the GNU GPL, version 2.0 only. See both LICENSE files for details.

AUTHOR
------

Jeremy Bingham <jbingham@gmail.com>

COPYRIGHT
---------

Copyright (c) 2026, Jeremy Bingham
