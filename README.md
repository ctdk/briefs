BrieFS!
======

_"If you want a picture of the future, imagine a pelican standing on a human head -- for ever."_

TODO: put something here

RELATED
-------

* github.com/ctdk/briefs-utils: The briefs utilities, written in Golang, comprised of `mkfs.briefs` and `fsck.briefs`. These are unsurprisingly in progress, but `mkfs.briefs` creates very basic BrieFS volumes while `fsck.briefs` doesn't do anything yet.
* github.com/ctdk/modern-xiafs: Computer filesystem archaeology. A port of an ancient Linux filesystem to modern kernels, updated as I get around to it.

RATIONALE
---------

Another Linux filesystem. Why, exactly?

There are multiple reasons that may nor may not be particularly good ones. First, I've found filesystems to be interesting for a long time as shown by my work with the Xiafs port to modern kernels. It's informative and illuminating, but not very useful for modern systems. Second, for a long time I've wondered about using trie data structures in a filesystem. (TrieFS doesn't make much sense, though, so the name is BrieFS.) Third, I believe a relatively simple but modern filesystem with well annotated source code could be a useful teaching tool; filesystems are something that in my experience you only understand if you already understand filesystems. Fourth, once I finally buckled to pressure and decided to try out AI assisted coding I thought making a relatively simple extents only filesystem would be a good and fun learning project.

Finally, and most importantly of all, the mental image of a pelican wearing briefs is funny.

AI Usage
--------

Since trying out AI assisted coding was part of the reason for this in the first place, it follows that an AI was used. It was not allowed to write code willy-nilly, however; I would review its proposals or changes and adjust them as needed rather than blindly trusting what it produced.

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
* reading and writing files works within the inline extents (external extents aren't implemented yet)
* unlink and rename work
* chown and chmod work
* fsync implemented
* bitmap pyramids for data block and inode allocation
* statfs
* journal replay on mount

DEFINITELY MISSING
------------------

* Creating things that aren't either regular files or directories (meaning, no symlinks, device files, named pipes, or so on).
* External extents aren't implemented yet, so reading and writing to files is brittle to say the least.
* Reusing inodes
* FUSE implementation (requires less commitment than the kernel module)

LICENSE
-------

BrieFS is dual licensed under the MIT license or the GNU GPL, version 2.0 only.
