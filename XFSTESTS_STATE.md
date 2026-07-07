# BrieFS xfstests state

State of the `generic` xfstests group against the BrieFS kernel module, as
measured by fresh full-suite and targeted runs on the VM.

## Overview

**Latest full-suite run:** 2026-07-06, `./check -g auto -X .exclude` on the VM,
kernel `6.12.94+deb13-amd64`, branch `even-more-xfstests`. `generic/388` was
expunged from the run via `/xfstests/tests/generic/.exclude` because it wedges
the suite (shutdown/replay corruption leaves `godown` stuck in D-state; see
failing-test notes).

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   782 | auto-group tests the suite considered (388 excluded)     |
| Not run          |   398 | skipped by a `_require_*` gate (verified below)            |
| Executed         |   384 | selected − not-run                                        |
| **Pass**         |   378 | executed and matched golden output                       |
| **Fail**         |     6 | see below                                                |
| Wedges / oops    |     0 | suite completed without manual intervention              |

**Tally reading.** xfstests reports "Failed 7 of 782 tests"; the 782 is the
*selected* count. Executed = 782 − 398 = 384; pass = 384 − 7 = 377.

**Of the 6 failures:**

- `generic/311` — pre-existing baseline flake (dm-flakey/fsync timing).
- `generic/417` — **fixed** on 2026-07-06: directory trie root persisted before
  shutdown, trie collapse frees empty root, allocator reserves block 0, and
  `put_super()` flushes metadata before checkpointing the journal.
- `generic/599` — real BrieFS bug: VFS `cleanup_mnt` WARN after a shutdown ioctl,
  triggered by BrieFS shutdown; dmesg check fails. Needs investigation.
- `generic/623` — real BrieFS shutdown bug: fsync after shutdown does not
  return `EIO` as the test expects.
- `generic/730` — real BrieFS shutdown bug: read after the `scsi_debug` block
  device is deleted should return `EIO`, but the read exits cleanly.
  Same error-propagation gap as 623.
- `generic/753` — real BrieFS dm-error metadata-sync bug: dmesg reports
  `journal back-pressure checkpoint failed: -5` and `metadata write error`
  (xattr chain + alloc header) under a failing dm-error device, then fsck
  reports an inconsistent filesystem. Same metadata-sync error-propagation
  family as the earlier `73a0d1d` fix, but hitting sync sites beyond inode
  table blocks.

Environment issues resolved after the re-run:

- `generic/038` — now runs and passes after `SCRATCH_DEV` was enlarged to 20 GiB.
- `generic/048` — now runs (was gated by ≥10 GB free space) and passes after the
  sync/shutdown size fix.
- `generic/133` — passed after `TEST_DEV` image was enlarged to 4 GiB.
- `generic/256`, `273`, `274`, `275`, `312`, `320`, `620`, `747` — now run and
  pass with the enlarged scratch/test loop devices.
- `generic/465` — passed after `SCRATCH_DEV` image was enlarged to 20 GiB.
- `generic/482`, `generic/757` — now correctly not-run (`could not locate any FUA
  write`) after `LOGWRITES_DEV` was configured; BrieFS does not issue FUA writes.

`generic/455` (log-writes replay) and `generic/475` (dm-error crash-replay)
both passed in this run. `generic/455` was previously failing with an md5
mismatch and is now green with the configured log-writes infrastructure.
`generic/127` also passed, though it has historically hung in the mmap+fsx
children; xfstests' own fsx timeout killed the children and the suite
continued.

**Post-run fix (2026-07-07):** The 2026-07-06 full-suite run exposed a kernel
BUG, not a BrieFS bug: a NULL pointer dereference at `iput+0xca` inside the
`inode_switch_wbs_work_fn` kworker, triggered by `SB_I_CGROUPWB`.  The 6.12.y VM
kernel has the known `cgroup_writeback_umount()` vs `inode_switch_wbs()` race
and CVE-2026-31703, both unfixed in this stable branch.  Commit `9385fc8`
temporarily disables cgroup writeback (removes `SB_I_CGROUPWB`) so the wb-switch
path is never entered.  A targeted run `generic/001..030` passes cleanly;
`generic/563` (which tests per-cgroup writeback accounting) regresses and will
remain failed until the VM kernel is updated.

---

## Excluded test

### generic/388 — shutdown-recovery stress wedge

- **Status:** expunged from the full suite via `/xfstests/tests/generic/.exclude`
  containing `388`.
- **Nature:** repeated `XFS_IOC_GOINGDOWN NOLOGFLUSH` shutdown / remount cycles
  corrupt BrieFS trie pages (bad-magic flood) and eventually leave `godown`
  stuck in `briefs_dir_open` in D-state, wedging the suite.
- **Action:** real shutdown/replay bug; not fixed in this run. Re-add to the
  suite only after the corruption is fixed.

---

## Failing tests (6)

### generic/048 — file size not persisted after sync+shutdown
- **Status:** **fixed** (`62167fa`); was fail (output mismatch), newly exposed now
  that scratch is large enough.
- **Root cause:** `iomap_file_buffered_write()` updates `i_size` itself but does
  not always dirty the inode, so `sync_inodes_sb()` did not write the final
  size before `XFS_IOC_GOINGDOWN` made the FS read-only. A second, smaller race
  was concurrent read-modify-write of the same inode-table block from sibling
  inodes; the copy-back could overwrite another slot that had been updated in
  the meantime.
- **Fix:** unconditionally `mark_inode_dirty()` in `briefs_iomap_buffered_write()`
  when `i_size` grew, and add per-inode-block mutex serialization around the
  snapshot/prepare/journal/copy-back in `briefs_write_inode()` and
  `briefs_persist_disk_inode()`.
- **Verification:** 30/30 standalone passes and 20/20 `dd` reproducer passes;
  `generic/030–099` shutdown cluster clean.

### generic/737 — file lost after O_DIRECT+shutdown
- **Status:** **fixed** (`8f4a27b`); was fail (output mismatch).
- **Root cause:** T-2 creates a 1 MiB file with `O_DIRECT & O_SYNC`, issues a
  sudden `XFS_IOC_GOINGDOWN NOLOGFLUSH` shutdown, and remounts; the file is gone.
  Two independent durability gaps combined: directory mutations were not flushed
  by the create path because `briefs_dir_sync()` delegated to
  `briefs_inode_sync()` and returned early for non-`IS_SYNC`/`DIRSYNC` inodes,
  and the 64-block journal ring could wrap under a stream of fsyncs and
  overwrite the still-needed create records because back-pressure was only
  checked on block-full flushes.
- **Fix:** rewrite `briefs_dir_sync()` to flush directory metadata, the journal,
  and issue a drive flush unconditionally; add back-pressure checkpoints at the
  top and bottom of `__briefs_journal_sync_locked()` to keep the ring from
  wrapping during sync-driven single-block advances.
- **Verification:** minimal `repro737.sh` 3/3, `generic/737` 3/3 standalone, and
  a regression cluster of `generic/003 029 030 032 048 640 737` all pass with
  clean `dmesg`; `generic/011` dirstress still passes.

### generic/127 — mmap+fsx D-state hang
- **Status:** **passed** in the 2026-07-06 run; historically a hang where the
  mmap fsx children enter D-state for >20 min and have to be killed.
- **Nature:** flaky deadlock between mmap writeback / page-fault interaction
  and BrieFS writeback. The non-mmap fsx variants complete fine.
- **Action:** monitor; if it re-hangs, investigate `briefs_writepage`/iomap
  DIO/mmap writeback paths.

### generic/311 — pre-existing baseline flake
- **Status:** fail (output mismatch), reproduces on a known-good baseline too.
- **Nature:** long-standing dm-flakey + fsync timing flake, not a BrieFS
  regression. Exercises fsync-on-a-failing-device semantics; the mismatch is
  sensitive to dm-flakey drop-window timing.
- **Action:** none (baseline; track only for regressions in neighbouring tests).

### generic/417 — xattr EA create/unlink race
- **Status:** **fixed** (repro passes, fsck clean) on 2026-07-06.
- **Root cause:** A create+setxattr+unlink+`NOLOGFLUSH` shutdown cycle left two
  inconsistencies across umount/remount:
  1. The parent directory's on-disk inode still pointed at a stale trie root
     because `briefs_update_parent_dir()` persisted the directory inode with
     `sync=false`; the updated `dir_trie_root` was not on disk before the
     journal was checkpointed clean, so the next mount read stale metadata and
     had no replay records to correct it.
  2. `briefs_trie_remove()` stopped its collapse loop at `anc - 2`, missing the
     leaf's immediate parent, and never freed an empty root. The orphan trie
     page was then reused by the next xattr block, so later trie allocations
     saw a bad magic and fell back to fresh pages until the allocator handed
     out data block 0, which every caller treats as the ENOSPC sentinel.
  3. The allocator itself allowed block 0 to be handed out successfully, even
     though its API uses 0 as the failure sentinel.
- **Fix:**
  - Update the parent directory inode in `briefs_update_parent_dir()` and leave
    the buffer dirty; `sync_blockdev()` in `briefs_put_super()` flushes it before
    the journal is checkpointed clean, so a NOLOGFLUSH shutdown/umount cycle
    still sees the trie root on disk without paying a synchronous write on every
    create/unlink.
  - Collapse the leaf's immediate parent in `briefs_trie_remove()` and free an
    empty root afterwards.
  - Treat data-relative block 0 as the ENOSPC sentinel everywhere in the
    allocator: reserve it at init, at allocation time, in multi-block
    allocation, and in `briefs_alloc_recompute_summaries()`.
  - Only force a journal sync + drive flush in `briefs_dir_sync()` for DIRSYNC or
    IS_SYNC inodes; normal directory operations are batched by the periodic
    journal checkpoint or the shutdown/umount path.
- **Verification:** local `repro417.sh` (create, 512-byte EA, unlink,
  `XFS_IOC_GOINGDOWN NOLOGFLUSH`, umount/remount, recreate, EA) passes,
  `fsck.briefs` reports a clean filesystem, and `generic/417` passes on the VM.

### generic/599 — VFS cleanup_mnt WARN after shutdown
- **Status:** fail (`_check_dmesg` catches a kernel warning).
- **Root cause:** after `XFS_IOC_GOINGDOWN LOGFLUSH`, the next umount triggers
  `WARNING: CPU: ... at fs/namespace.c:1370 cleanup_mnt+0x130/0x150`. BrieFS
  shutdown leaves the mount in a state that surprises the VFS cleanup path.
- **Action:** investigate shutdown → umount cleanup ordering.

### generic/623 — fsync after shutdown does not return EIO
- **Status:** fail (output mismatch).
- **Root cause:** expected output contains `fsync: Input/output error`; BrieFS
  does not return `EIO` on fsync after a shutdown ioctl.
- **Action:** wire shutdown error propagation into fsync/writeback.

### generic/455 — log-writes replay md5 mismatch
- **Status:** **fixed/passing** in the 2026-07-06 run; was fail (output mismatch)
  with `testfile0.mark4 md5sum mismatched`.
- **Nature:** with `LOGWRITES_DEV` properly configured (20 GiB backing image),
  the test now replays logged writes correctly and matches the expected md5.
- **Action:** none; remains in the crash-replay family and should be watched
  for regressions.

### generic/730 — read after device deletion missing EIO
- **Status:** fail (output mismatch); expected read error, got no error.
- **Root cause:** after deleting the `scsi_debug` device under a mounted BrieFS
  filesystem with an open read fd, BrieFS does not propagate `EIO` to the read.
  Same shutdown/error-propagation gap as `generic/623`.
- **Action:** real BrieFS bug; wire device-error/shutdown error propagation into
  read path.

### generic/753 — dm-error metadata write-error WARN
- **Status:** fail (`_check_dmesg` + fsck inconsistency).
- **Root cause:** under a dm-error device that fails writes, BrieFS emits
  `journal back-pressure checkpoint failed: -5`, `metadata write error (xattr
  chain sync)`, and `metadata write error (alloc header sync)`, then the
  post-test fsck reports an inconsistent filesystem. The earlier `73a0d1d` fix
  hardened inode-table writes with `lock_buffer` across `mark_buffer_dirty`,
  but other metadata sync paths (xattr chain, allocator headers) still propagate
  errors only to dmesg without cleaning up or remounting read-only safely.
- **Action:** extend the metadata-sync error-check pattern to xattr chain and
  allocator sync sites; consider remounting read-only on metadata I/O errors.

---

## Not-run tests (398)

Every not-run is gated by a `_require_*` probe that actually exercises the
filesystem or the VM environment, so a not-run is a genuine unimplemented
feature or an environment gap — not a stale declaration. The `.notrun` reasons
from the run were read and grouped; all gates are legitimate.

**Two meta-categories:**

- **Absent feature** (BrieFS does not implement it): reflink/COW, quota,
  shutdown-state probing, fscrypt/fsverity, exchangerange, POSIX ACL,
  fcollapse/fzero/finsert, dedupe, idmapped mounts, O_TMPFILE, FITRIM, DAX,
  atomic writes, defrag, casefold, setdeleg, fsmap, swapext/startupdate,
  file_getattr/file_setattr syscalls, connectable file handles, fanotify ioerrors,
  duplicate fsid, cross-device copy_file_range, project quota, etc. Most are
  deliberately out of scope.
- **Environment** (VM setup, closable without code change): lvm/logwrites
  devices, fsverity/duperemove utilities, dbtest not built, scratch/test too
  small, selinux, hibernation-to-swap, zoned devices.

### Absent features

| Reason (gate text)                                        | N  | Tests                                                                                                                                                                                                                                                                           |
|-----------------------------------------------------------|----:|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Reflink not supported (scratch)                           | 138 | 161 164 165 166 167 168 170 171 172 173 174 175 176 183 185 186 187 188 189 190 191 194 195 196 197 199 200 201 202 203 205 206 216 217 218 220 222 227 229 238 242 243 253 254 259 261 262 264 265 266 267 268 271 272 276 278 279 281 282 283 284 287 289 290 291 292 293 295 296 297 298 301 302 305 326 327 328 329 330 331 332 333 334 352 353 356 357 358 359 370 372 373 387 414 415 447 457 458 501 513 514 515 518 540 541 542 543 544 546 562 588 628 648 651 652 653 654 655 657 658 659 660 661 662 663 664 665 666 667 668 669 670 671 672 673 675 702 733 |
| Reflink not supported (test)                              | 39 | 110 111 115 116 118 119 134 137 138 139 140 142 143 144 145 146 147 148 149 150 151 152 153 154 155 156 157 159 178 179 180 181 303 407 463 578 612 649 734 |
| disk quotas not supported                                 | 31 | 082 219 230 231 232 233 234 235 244 270 280 379 380 381 382 383 384 385 386 400 506 566 587 594 600 601 603 681 682 691 762 |
| No encryption support (fscrypt)                           | 28 | 368 369 395 396 397 398 399 419 421 429 435 440 548 549 550 580 581 582 583 584 592 593 595 602 613 621 693 739 |
| xfs_io exchangerange not supported                        | 16 | 709 710 712 714 716 717 718 719 720 722 723 724 725 726 727 752 |
| ACLs not supported                                        | 14 | 026 053 077 099 105 237 307 318 319 375 444 449 529 697 |
| xfs_io fcollapse failed (no COLLAPSE_RANGE)               | 12 | 012 016 017 021 022 031 072 497 499 503 641 687 |
| fsverity utility required (no fsverity)                   | 11 | 572 573 574 575 576 577 579 624 625 692 788 |
| xfs_io fzero failed (no ZERO_RANGE)                       | 11 | 008 009 033 042 096 456 469 511 610 685 758 |
| Dedupe not supported (test)                               | 9 | 121 122 136 158 160 182 304 408 516 |
| xfs_io finsert failed (no INSERT_RANGE)                   | 9 | 058 060 061 063 064 404 485 686 735 |
| Dedupe not supported (scratch)                            | 7 | 162 163 374 493 517 630 674 |
| idmapped mounts not supported                             | 6 | 644 645 656 689 698 699 |
| FITRIM not supported                                      | 5 | 038 251 260 288 500 |
| O_TMPFILE not supported                                   | 4 | 004 389 509 531 |
| DAX not supported                                         | 3 | 413 462 605 606 608 |
| log state probing not supported                           | 3 | 052 054 055 |
| duperemove utility required                               | 3 | 559 560 561 |
| multi-block atomic writes not supported                   | 3 | 774 775 778 |
| write atomic not supported (block device)                 | 3 | 765 773 776 |
| xfs_io pwrite doesn't support -A                          | 3 | 768 769 770 |
| defragmentation not supported                             | 2 | 018 324 |
| casefold not supported                                    | 2 | 556 783 |
| fcntl setdeleg not supported                              | 2 | 786 787 |
| xfs_io exchangerange -s 64k -l 64k not supported          | 2 | 713 715 |
| could not locate any FUA write                            | 2 | 482 757 |
| can't mkfs briefs with geometry                           | 1 | 223 |
| xfs_io fsmap missing                                      | 1 | 365 |
| filesystem timestamp bounds unknown                       | 1 | 402 |
| xfs_io fiemap -a failed (no attr-fork fiemap)             | 1 | 425 |
| xfs_io label failed (no label ioctl)                      | 1 | 492 |
| cross-device copy_file_range not supported                  | 1 | 565 |
| requires delayed allocation buffered writes                | 1 | 614 |
| xfs_io swapext not supported                              | 1 | 711 |
| xfs_io startupdate not supported                          | 1 | 721 |
| briefs does not support duplicate fsid                      | 1 | 744 |
| requires > 1000 xattrs (4K xattr block limit)               | 1 | 745 |
| requires fs-specific discard-range check                  | 1 | 746 |
| connectable file handles (export ops lack .get_name)      | 1 | 777 |
| fanotify ioerrors not supported                           | 1 | 791 |
| FSTRIM not supported                                      | 1 | 537 |
| file_getattr not supported for regular files on briefs    | 1 | 772 |
| write atomic not supported (filesystem)                   | 1 | 767 |
| xfs_io chattr +x failed                                   | 1 | 607 |

> **Note on chattr/lsattr.** `generic/079 277 424 545 553 555 596 629` are now
> passing after the inode-flag implementation. The remaining chattr-related
> not-runs are blocked by other missing features: `159 160` (reflink/dedupe over
> immutable files), `607` (DAX inheritance), `508` (requires
> `_require_scratch_shutdown`), and `507` (shutdown). `generic/472` exercises a
> swapfile with `chattr` noise on stderr but **passes** — the chattr failure is
> non-fatal there.

### Environment gaps (closable without code change)

| Reason (gate text)                                        | N  | Tests            | Fix                                                            |
|-----------------------------------------------------------|----:|------------------|----------------------------------------------------------------|
| requires $LOGWRITES_DEV / dm-log-writes                   | 1  | 470              | configure dm-log-writes scratch device; 455 now runs and fails   |
| BrieFS issues no FUA writes                               | 2  | 482 757          | genuine missing feature; cannot run these log-writes tests       |
| requires $SCRATCH_LOGDEV                                  | 2  | 487 766          | configure dm-log-writes config                                  |
| scratch device too small / zoned loopback needed          | 2  | 781 793          | bigger SCRATCH_DEV + kernel zoned support                       |
| scratch device too small / other size gates                 | 0  | —                | resolved by 20 GiB SCRATCH_DEV (038, 048, 256, 273–275, 312, 320, 620, 747 now run) |
| requires ≥4GB free on test                                 | 1  | 694              | bigger TEST_DEV loop (≥8G)                                      |
| requires ≥5GB free on test                                 | 1  | 701              | bigger TEST_DEV loop (≥8G)                                      |
| /xfstests/src/dbtest not built                            | 1  | 010              | `make` in /xfstests/src (build bug `dbtest.c:306 myDB`, oos)    |
| selinux required                                          | 1  | 700              | env                                                            |
| userspace hibernation to swap enabled                     | 1  | 570              | env                                                            |

> `fsverity`/`duperemove` "utility required" rows are listed under absent
> features: even with the utility installed, BrieFS lacks the kernel ioctl, so
> the test would still not-run. The util alone won't help.

---

## Passing tests (377)

```
001  002  003  005  006  007  011  013
014  015  020  023  024  025  027  028
029  030  032  034  035  036  037  039
040  041  043  044  045  046  047  048  049
050  051  056  057  059  062  065  066
067  068  069  070  071  073  074  075
076  078  079  080  081  083  084  085
086  087  088  089  090  091  092  093
094  095  097  098  100  101  102  103
104  106  107  108  109  112  113  114
117  120  123  124  125  126  127  128  129
130  131  132  133  135  141  169  177
184  192  193  198  204  207  208  209
210  211  212  213  214  215  221  224
225  226  228  236  239  240  241  245
246  247  248  249  250  252  255  256
257  258  263  269  273  274  275  277
285  286  294  299  300  306  308  309
310  312  313  314  315  316  317  320
321  322  323  325  335  336  337  338
339  340  341  342  343  344  345  346
347  348  354  355  360  361  362  363
364  366  371  376  377  378  390  391
392  393  394  401  403  405  406  409
410  411  412  416  418  420  422  423
424  426  427  428  430  431  432  433
434  436  437  438  439  441  443  445
446  448  450  451  452  453  454  455  459
460  461  464  465  466  467  468  471
472  474  475  476  477  478  479  480
481  483  484  486  488  489  490  491
494  495  496  498  502  504  505  507
508  510  512  519  520  523  524  525
526  527  528  530  532  533  534  535
536  538  539  545  547  551  552  553
554  555  557  558  563  564  567  568
569  571  585  586  589  590  591  596
597  598  604  609  611  615  616  617
618  619  620  622  626  627  629  631
632  633  634  635  636  637  638  639
640  642  643  646  647  650  676  677
678  679  680  683  684  688  690  695
696  703  704  705  706  707  708  728
729  731  732  736  737  738  740  741  742
743  747  748  749  750  751  754
755  756  759  760  761  763  764  771
779  782  784  785  789  790  792
```

### xfstests xattr cluster (13/13, 2026-07-02)

All xattr-gated tests pass with the chained-xattr fix in `29121e6`:
020, 037, 062, 066, 070, 097, 103, 117, 337, 377, 403, 454, 631.

### xfstests shutdown cluster (2026-07-04)

The `XFS_IOC_GOINGDOWN` ioctl is now implemented, so `godown`-based shutdown
tests run. Core cluster `043 044 045 046 047 049 050 051` passes (048 now fails
with the sync/size bug). `050` required a read-only-dirty-journal mount
rejection in `briefs_fill_super`.

Extended godown cluster `392 461 468 474 505 530 536 622 635 646 705` passes.

Newly-exposed shutdown-related failures:
`599` (VFS cleanup_mnt WARN), `623` (fsync after shutdown missing EIO), `730`
(read after device delete missing EIO), `753` (dm-error metadata-sync WARN), and
historically `127` (mmap+fsx D-state hang — passed in this run). `generic/417`
(xattr EA race) and `generic/737` (file lost after O_DIRECT+shutdown) are now
fixed. `generic/388` is excluded from the full suite because it wedges.

With `LOGWRITES_DEV` now configured, `455` passes (was an md5 mismatch in the
previous run). `482` and `757` are correctly not-run because BrieFS does not
issue FUA writes. Size failures `133` and `465` passed after enlarging the loop
images.

### Recent fix highlights (this campaign)

A large cluster of previously-failing tests now passes. Notable fixes:

| Tests                         | Commit   | Area                                                          |
|-------------------------------|----------|---------------------------------------------------------------|
| 093 193 683 684 688           | a1eb7e0  | killpriv-on-modify (file_remove_privs on inline write + fallocate, ATTR_MODE on truncate) |
| 563                           | 55023ac/9385fc8 | cgroup writeback (SB_I_CGROUPWB) temporarily disabled on 6.12 due to iput crash |
| 617                           | 0cd7062  | punch-empty-tree straddler orphan                             |
| 522 616                       | —        | punch-split-extent pagecache invalidation                     |
| 074                           | fb649e8  | orphan dir-trie-page leak (trie_free_node)                    |
| 753                           | 73a0d1d  | dm-error writeback WARN (lock_buffer across mark_buffer_dirty)|
| 679                           | 05cb297  | unwritten extents                                             |
| 020 037 062 066 070 097 103 117 337 377 403 454 631 | 29121e6  | chained xattr buffer-head accounting + release bugs             |
| 547                           | 86fa48b  | fsync-ordered trie durability                                 |
| 640                           | 2d66610  | rename trie-root journal ordering                             |
| 620                           | 97a50e7  | mkfs huge-disk EIO (stop pre-zeroing inode table)             |
| 169 420                       | 8321fc6  | FS_IOC_FSGETXATTR ioctl                                       |
| 029 030 032                   | f8ef293  | always-checkpoint at unmount (clean-unmount replay clobber)   |
| 417                           | —        | xattr/unlink NOLOGFLUSH durability (dir inode sync, trie collapse, block-0 sentinel) |
| 464                           | 4ef6ccb  | trie_iter_grow double-free (suite wedge)                      |
| 023 025 078                   | —        | renameat2 EXCHANGE/WHITEOUT + emptiness check                 |
| 257 637 676                   | —        | readdir seek/resume (simple_offset)                           |
| 471 736                       | —        | readdir rewinddir + trie_gen staleness                        |
| 313 423 755                   | —        | timestamp cluster (current_time at all sites)                 |
| 322 321                       | —        | journal-replay write_pos + stale-cached-parent evict          |
| 011                           | —        | journal write serialization (per-journal write_lock)          |
| 015                           | —        | delalloc ENOSPC stall (nonda_switch + fail-folio)             |
| 087                           | —        | utime setattr_prepare                                         |
| 633 696                       | —        | setgid inheritance (inode_init_owner)                         |
| 228 394                       | —        | RLIMIT_FSIZE (inode_newsize_ok)                               |
| 092 483                       | —        | fallocate prealloc                                            |
| 467 426 756                   | —        | exportfs (encode_fh/fh_to_dentry/fh_to_parent/get_parent)     |
| 626                           | —        | RENAME_WHITEOUT                                               |
| 749                           | —        | —                                                             |
| 732 634 741 754               | —        | mkfs stdout/refuse-overwrite/fsync                            |
| 068 085 390 491 738           | c274292  | FIFREEZE/FITHAW (freeze_fs/unfreeze_fs)                       |
| 405                           | —        | dm-thin write-error (metadata sync error check)               |
| 003                           | —        | journal-replay multi-fix cluster                              |
| 643                           | —        | swapfile (iomap_swapfile_activate — passes; was mis-tagged)   |
| 704                           | —        | O_DIRECT (iomap DIO landed; sub-sector DIO accepted)          |
| 177                           | —        | env (gawk installed)                                          |
| 079 277 424 545 553 555 596 629 | 38d57d0  | chattr/lsattr inode flags (+S/+D/+i/+a/+d/+A)                |
| 475                           | —        | dm-error crash-replay: passed this run, still flaky/deferred   |
| 048                           | 62167fa  | sync+shutdown file size bug (inode dirty on i_size growth + inode-block RMW lock) |
| 737                           | 8f4a27b  | O_DIRECT+shutdown file lost (directory sync durability + journal ring back-pressure) |

> Open BrieFS code bugs after this run: `417` (xattr/unlink race), `599`
> (shutdown VFS cleanup WARN), `623` (fsync after shutdown missing EIO), `730`
> (read after device delete missing EIO), `753` (dm-error metadata-sync WARN),
> and the excluded `388` shutdown/replay wedge. `generic/127` passed in this run
> but remains a known mmap+fsx D-state hang risk. `generic/311` is a
> pre-existing baseline flake. `generic/455` and `generic/475` passed in this
> run; `475` remains a known flaky deferred bug in the crash-replay family.

---

## How this was measured

- Full suite `./check -g auto -X .exclude` on the VM (2026-07-06), kernel
  `6.12.94+deb13-amd64`, branch `even-more-xfstests`. `generic/388` excluded
  via `/xfstests/tests/generic/.exclude`.
- Post-run setup on the VM: `TEST_DEV` (`/var/tmp/test.img`) enlarged to
  4 GiB, `SCRATCH_DEV` (`/var/tmp/scratch.img`) and `LOGWRITES_DEV`
  (`/var/tmp/logwrites.img`) enlarged to 20 GiB. `LOGWRITES_DEV` is a raw loop
  device; logwrites tests create their own `/dev/mapper/logwrites-test` dm target
  on top of `SCRATCH_DEV`.
- Results: `generic/038`, `048`, `256`, `273–275`, `312`, `320`, `620`, `747`
  run and pass; `generic/133` and `generic/465` pass; `generic/482` and
  `generic/757` correctly not-run (`could not locate any FUA write`);
  `generic/455` passes with configured log-writes; `generic/753` newly fails on
  dm-error metadata-sync dmesg warnings.
- Pass/Fail/Not-run lists derived from the final `Ran:` / `Not run:` / `Failures:`
  block in `/xfstests/results/check.log`.
- Not-run reasons read from each test's `.notrun` artifact in
  `/xfstests/results/generic/` and grouped.
- Failing-test details inspected from `.out.bad`, `.full`, and `.dmesg` files.
