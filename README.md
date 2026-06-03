BrieFS!
======

TODO: put something here

RELATED
-------

* github.com/ctdk/briefs-utils: The briefs utilities, written in Golang, comprised of `mkfs.briefs` and `fsck.briefs`. These are unsurprisingly in progress, but `mkfs.briefs` creates very basic BrieFS volumes while `fsck.briefs` doesn't do anything yet.

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

It's not even entirely finished, so yeah. It doesn't actually do much yet. A list of what *does* work would be more informative.

WORKS
-----

* Kernel module loading
* Mounting (read-only) a filesystem created by `mkfs.briefs`.
* Listing the root directory.
* inode map created
* data map created
* extent allocation table created
* journal should be being created

LICENSE
-------

BrieFS is dual licensed under the MIT license or the GNU GPL, version 2.0 only.
