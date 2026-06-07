BrieFS!
======

[The BrieFS logo - a pelican wearing briefs (aka The Pelican's Briefs)](https://github.com/ctdk/briefs/blob/master/images/briefs-logo-1.png)

_"If you want a picture of the future, imagine a pelican standing on a human head -- for ever."_

BrieFS: A Linux filesystem that solely uses [extents](https://en.wikipedia.org/wiki/Extent_(file_systems)) for file data, [tries](https://en.wikipedia.org/wiki/Trie) for storing directory contents, and [bitmap tries](https://en.wikipedia.org/wiki/Bitwise_trie_with_bitmap) for inode and block allocation.


**NB:** This filesystem is experimental and may totally flip out, make your machine hang and stop responding, corrupt your data, corrupt your children, annoy you greatly, or just up and crash. Caution is advised. I must state that I've warned you about potential issues and that I'm not responsible if anything bad happens because you were messing around with this. Also, no warranty as per the LICENSE files.

RELATED
-------

* [github.com/ctdk/briefs-utils](github.com/ctdk/briefs-utils): The briefs utilities, written in Golang, comprised of `mkfs.briefs` and `fsck.briefs`. These are unsurprisingly in progress, but `mkfs.briefs` creates BrieFS volumes while `fsck.briefs` doesn't do anything yet.
* [github.com/ctdk/modern-xiafs](github.com/ctdk/modern-xiafs): Computer filesystem archaeology. A port of an ancient Linux filesystem to modern kernels, updated as I get around to it.

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

For certain values of "support", anyway. As of this writing, all BrieFS development is being done using the Debian Linux kernel version 6.12.48. Once it gets far enough along, it will jump up to track the current `linux` git repo. Other kernel versions and specific distro kernels may come as time and interest permit and dictate.

BUGS
----

It's not even entirely finished, so yeah. It does stuff, but it's still at a point that a list of what *does* work would be more informative.

WORKS
-----

* Kernel module loading
* Kernel module unloading
* Mounting a filesystem created by `mkfs.briefs`.
* Unmounting a filesystem.
* Listing the root directory.
* touching files
* mkdir creates directories
* file lookup (the exact implementation is subject to change)
* reading and writing files works with both inline and external extents
* unlink and rename work
* chown and chmod work
* fsync implemented
* bitmap pyramids for data block and inode allocation
* statfs
* journal replay on mount
* truncate

DEFINITELY MISSING OR BROKEN
----------------------------

* Can't execute ELF binaries from a briefs filesystem right now.
* Normally creating files, writing files, making directories, that sort of thing work fine. However, a test run of untarring the linux kernel sources in a directory on a briefs filesystem had a very impressive kablooie. Be gentle for now until that's sorted.
* Creating things that aren't either regular files or directories (meaning, no symlinks, device files, named pipes, or so on).
* Reusing inodes
* FUSE implementation (requires less commitment than the kernel module)
* Proper tests
* Thorough annotations - Annotating the source code thoroughly will wait until tings settle down. Right now everything's still in constant flux, so there's no point thoroughly annotating something that may change unrecognizably or flat out disappear soon.

LICENSE
-------

BrieFS is dual licensed under the MIT license or the GNU GPL, version 2.0 only.
