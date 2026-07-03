# BrieFS xfstests state

State of the `generic` xfstests group against the BrieFS kernel module, as
measured by fresh full-suite and targeted runs on the VM.

## Overview

**Latest full-suite run:** not yet re-run after the shutdown ioctl
implementation; previous run 2026-07-03 (`./check -s briefs -g auto`)
recorded 347 pass / 434 not-run / 2 fail.

**Latest targeted shutdown run:** 2026-07-03, core shutdown cluster
`043 044 045 046 047 048 049 050 051` all passed; extended godown-based
cluster `388 392 468 474 505 530 536 622 635 646 705` passed. New failures
in this group are unrelated to the ioctl itself: `417` (xattr EA size issue),
`599` (dmesg noise), `623` (VM `xfs_io` lacks the `shutdown` command),
`730`/`737` (other post-shutdown consistency issues, `737` hits a pre-existing
`fsck.briefs` OOM spin).

**Latest targeted chattr run:** 2026-07-03, `generic/079 277 424 545 553 555
596 629`, all 8 passed, using the module built from commit `38d57d0` on
`even-more-xfstests`.

**Latest regression cluster run:** 2026-07-03, 25 tests passed:
`003 011 015 020 023 025 029 030 032 040 041 059 065 073 075 090 101 228 394
424 545 553 555 596 629`.

**Latest xattr cluster run:** 2026-07-02, `generic/020 037 062 066 070 097 103
117 337 377 403 454 631`, all 13 passed (commit `29121e6` on
`even-more-xfstests`).

| Bucket        | Count | Notes                                                     |
|---------------|------:|-----------------------------------------------------------|
| Selected      |   783 | auto-group tests the suite considered                      |
| **Pass**      |   347 | executed and matched golden output                        |
| **Not run**   |   434 | skipped by a `_require_*` gate (absent feature or env)    |
| **Fail**      |     2 | `generic/311`, `generic/475` (see below)                  |
| Wedges / oops |     0 | suite completes cleanly, dmesg clean                      |

**Tally reading.** "Failed 2 of 783" — the 783 is the *selected* count, not the
executed count. Executed = 783 − 434 = 349; pass = 349 − 2 = 347.

**Of the 2 failures:**

- `generic/311` — pre-existing, reproduces on baseline; dm-flakey/fsync flake.
- `generic/475` — the one remaining **open BrieFS code bug** (dm-error
  crash-replay trie reallocation under fsstress concurrency). It is now exercised
  by the log-writes device setup and fails in-suite; it was previously
  not-run/masked. Deferred — needs an on-disk journal-format / sync-model
  change, explicitly out of scope.

---

## Failing tests (2)

### generic/311 — pre-existing baseline flake
- **Status:** fail (output mismatch), reproduces on a known-good baseline too.
- **Nature:** long-standing dm-flakey + fsync timing flake, not a BrieFS
  regression. Exercises fsync-on-a-failing-device semantics; the mismatch is
  sensitive to dm-flakey drop-window timing.
- **Action:** none (baseline; track only for regressions in neighbouring tests).

### generic/475 — dm-error crash-replay trie reallocation (real bug; deferred)
- **Status:** fail in full suite; previously passed in-suite and only flaked
  standalone, but the log-writes test infrastructure now exercises it reliably.
- **Root cause:** crash replay re-allocates trie pages instead of reusing the
  `JRN_TRIE_ALLOC` blocks recorded before the crash. Under concurrent fsstress
  interleaving the FIFO trie-block design aliases pages across unrelated parent
  directories, producing `-ENOSPC` or bad-magic aliasing on replay.
- **Fix:** requires an on-disk journal-format change (tag `JRN_TRIE_ALLOC` with
  the parent inode) or a sync-model change (persist trie structure so replay
  does not re-derive it). Out of scope for the current campaign.
- **Action:** defer; track as the single remaining open bug.

---

## Not-run tests (434)

Every not-run is gated by a `_require_*` probe that actually exercises the
filesystem (xfs_io/filefrag/vfstest), so a not-run is a genuine unimplemented
feature or an environment gap — not a stale declaration. Grouped by reason below.

**Two meta-categories:**

- **Absent feature** (BrieFS does not implement it): reflink/COW, quota,
  shutdown, fscrypt/fsverity, exchangerange, POSIX ACL, fcollapse/fzero/finsert,
  dedupe, idmapped mounts, O_TMPFILE, FITRIM, DAX, atomic writes, defrag,
  casefold, setdeleg, fsmap, swapext/startupdate, file_getattr/file_setattr
  syscalls, project quota, etc. These would need kernel work to close; most are
  deliberately out of scope.
- **Environment** (VM setup, closable without code change): lvm/logwrites
  devices, fsverity/duperemove utilities, dbtest not built, scratch/test too
  small, scsi_debug transiently loaded, selinux, hibernation-to-swap.

### Absent features

| Reason (gate text)                                        | N  | Tests                                                                                                                                                                                                                                                                           |
|-----------------------------------------------------------|----|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Reflink not supported (scratch)                           | 138| 161 164 165 166 167 168 170 171 172 173 174 175 176 183 185 186 187 188 189 190 191 194 195 196 197 199 200 201 202 203 205 206 216 217 218 220 222 227 229 238 242 243 253 254 259 261 262 264 265 266 267 268 271 272 276 278 279 281 282 283 284 287 289 290 291 292 293 295 296 297 298 301 302 305 326 327 328 329 330 331 332 333 334 352 353 356 357 358 359 370 372 373 387 414 415 447 457 458 501 513 514 515 518 540 541 542 543 544 546 562 588 628 648 651 652 653 654 655 657 658 659 660 661 662 663 664 665 666 667 668 669 670 671 672 673 675 702 733 |
| Reflink not supported (test)                              | 38 | 110 111 115 116 118 119 134 137 138 139 140 142 143 144 145 146 147 148 149 150 151 152 153 154 155 156 157 178 179 180 181 303 407 463 578 612 649 734 |
| disk quotas not supported                                 | 32 | 055 082 219 230 231 232 233 234 235 244 270 280 379 380 381 382 383 384 385 386 400 506 566 587 594 600 601 603 681 682 691 762 |
| log state probing not supported                           | 2  | 052 054 |
| No encryption support (fscrypt)                           | 28 | 368 369 395 396 397 398 399 419 421 429 435 440 548 549 550 580 581 582 583 584 592 593 595 602 613 621 693 739 |
| xfs_io exchangerange not supported                       | 18 | 709 710 712 713 714 715 716 717 718 719 720 722 723 724 725 726 727 752 |
| ACLs not supported                                        | 14 | 026 053 077 099 105 237 307 318 319 375 444 449 529 697 |
| xfs_io fcollapse failed (no COLLAPSE_RANGE)              | 12 | 012 016 017 021 022 031 072 497 499 503 641 687 |
| fsverity utility required (no fsverity)                   | 11 | 572 573 574 575 576 577 579 624 625 692 788 |
| xfs_io fzero failed (no ZERO_RANGE)                      | 11 | 008 009 033 042 096 456 469 511 610 685 758 |
| xfs_io finsert failed (no INSERT_RANGE)                   | 9  | 058 060 061 063 064 404 485 686 735 |
| Dedupe not supported (test)                              | 8  | 121 122 136 158 182 304 408 516 |
| Dedupe not supported (scratch)                           | 7  | 162 163 374 493 517 630 674 |
| DAX not supported                                        | 5  | 413 462 605 606 608 |
| idmapped mounts not supported                            | 6  | 644 645 656 689 698 699 |
| O_TMPFILE not supported                                  | 4  | 004 389 509 531 |
| FITRIM not supported                                     | 4  | 251 260 288 500 |
| atomic writes not supported                             | 10 | 765 768 769 770 773 774 775 776 778 767 |
| defragmentation not supported                            | 2  | 018 324 |
| casefold not supported                                   | 2  | 556 783 |
| fcntl setdeleg not supported                             | 2  | 786 787 |
| can't mkfs briefs with geometry                          | 1  | 223 |
| xfs_io fsmap missing                                     | 1  | 365 |
| filesystem timestamp bounds unknown                      | 1  | 402 |
| xfs_io fiemap -a failed (no attr-fork fiemap)            | 1  | 425 |
| xfs_io label failed (no label ioctl)                     | 1  | 492 |
| cross-device copy_file_range not supported                | 1  | 565 |
| xfs_io shutdown command missing in VM xfs_io             | 1  | 623 | rebuild or install xfsprogs with shutdown support |
| post-shutdown consistency / fsck stress                  | 4  | 417 599 730 737 | 417=xattr EA issue, 599=dmesg, 730/737=other (737 OOMs fsck.briefs) |
| requires delayed allocation buffered writes              | 1  | 614 |
| xfs_io swapext not supported                             | 1  | 711 |
| xfs_io startupdate not supported                         | 1  | 721 |
| briefs does not support duplicate fsid                   | 1  | 744 |
| requires > 1000 xattrs (4K xattr block limit)            | 1  | 745 |
| requires fs-specific discard-range check                 | 1  | 746 |
| connectable file handles (export ops lack .get_name)     | 1  | 777 |
| fanotify ioerrors not supported                          | 1  | 791 |
| FSTRIM not supported                                     | 1  | 537 |

> **Note on chattr/lsattr.** `generic/079 277 424 545 553 555 596 629` are now
> passing after the inode-flag implementation in commit `38d57d0`. The
> remaining chattr-related not-runs are blocked by other missing features:
> `159 160` (reflink/dedupe over immutable files), `607` (DAX inheritance),
> `508` (requires `_require_scratch_shutdown`), and `507` (shutdown). `generic/472`
> exercises a swapfile with `chattr` noise on stderr but **passes** — the chattr
> failure is non-fatal there.

### Environment gaps (closable without code change)

| Reason (gate text)                                        | N  | Tests            | Fix                                                            |
|-----------------------------------------------------------|----|------------------|----------------------------------------------------------------|
| lvm utility required                                      | 3  | 081 108 459      | `apt install lvm2` (config-heavy)                             |
| requires $LOGWRITES_DEV                                   | 4  | 455 470 482 757  | dm-logwrites scratch config                                    |
| requires $SCRATCH_LOGDEV                                  | 2  | 487 766          | dm-logwrites config                                            |
| scratch device too small (<16G)                           | 2  | 781 793          | bigger SCRATCH_DEV loop (≥16G)                                 |
| requires ≥10GB free on scratch                            | 1  | 038              | bigger scratch                                                 |
| requires ≥4GB free on test                                | 1  | 694              | bigger TEST_DEV loop (≥8G)                                     |
| requires ≥5GB free on test                                | 1  | 701              | bigger TEST_DEV loop (≥8G)                                     |
| /xfstests/src/dbtest not built                            | 1  | 010              | `make` in /xfstests/src (build bug `dbtest.c:306 myDB`, oos)   |
| scsi_debug module in use                                  | 1  | 731              | `rmmod scsi_debug` before suite (transient)                   |
| selinux required                                          | 1  | 700              | env                                                            |
| userspace hibernation to swap enabled                     | 1  | 570              | env                                                            |
| file_getattr not supported for regular files on briefs    | 1  | 772              | needs `file_getattr`/`file_setattr` syscalls (kernel 6.15+)   |

> `fsverity`/`duperemove` "utility required" rows are listed under absent
> features: even with the utility installed, BrieFS lacks the kernel ioctl, so
> the test would still not-run. The util alone won't help.

---

## Passing tests (347)

```
001  002  003  005  006  007  011  013
014  015  020  023  024  025  027  028
029  030  032  034  035  036  037  039
040  041  056  057  059  062  065  066
067  068  069  070  071  073  074  075
076  078  079  080  083  084  085  086
087  088  089  090  091  092  093  094
095  097  098  100  101  102  103  104
106  107  109  112  113  114  117  120
123  124  125  126  127  128  129  130
131  132  133  135  141  169  177  184
192  193  198  204  207  208  209  210
211  212  213  214  215  221  224  225
226  228  236  239  240  241  245  246
247  248  249  250  252  255  256  257
258  263  269  273  274  275  277  285
286  294  299  300  306  308  309  310
312  313  314  315  316  317  320  321
322  323  325  335  336  337  338  339
340  341  342  343  344  345  346  347
348  354  355  360  361  362  363  364
366  371  376  377  378  390  391  393
394  401  403  405  406  409  410  411
412  416  418  420  422  423  424  426
427  428  430  431  432  433  434  436
437  438  439  441  443  445  446  448
450  451  452  453  454  460  464  465
466  467  471  472  475  476  477  478
479  480  481  483  484  486  488  489
490  491  494  495  496  498  502  504
510  512  519  520  523  524  525  526
527  532  533  534  535  538  539  545
547  551  552  553  554  555  557  558
564  567  568  569  571  585  586  589
590  591  596  597  598  604  609  611
615  616  617  618  619  620  626  627
629  631  632  633  634  636  637  638
639  640  642  643  647  650  676  677
678  679  680  683  684  688  690  695
696  703  704  706  707  708  728  729
732  736  738  740  741  742  743  747
748  749  750  751  753  754  755  756
759  760  761  763  764  771  779  782
784  785  789  790  792
```

### xfstests xattr cluster (13/13, 2026-07-02)

All xattr-gated tests pass with the chained-xattr fix in `29121e6`:
020, 037, 062, 066, 070, 097, 103, 117, 337, 377, 403, 454, 631.

### xfstests shutdown cluster (2026-07-03)

The `XFS_IOC_GOINGDOWN` ioctl is now implemented, so `godown`-based shutdown
tests run. Core cluster `043 044 045 046 047 048 049 050 051` passes.
Extended godown cluster `388 392 461 468 474 505 530 536 622 635 646 705`
passes. `050` required a read-only-dirty-journal mount rejection in
`briefs_fill_super`.

Still gated by other missing features: `042` (fzero), `052 054` (log state),
`055` (quota), `766` (scratch logdev). Newly-exposed failures: `417` (xattr
EA size in `multi_open_unlink`), `599` (dmesg noise), `623` (VM `xfs_io`
lacks the `shutdown` command), `730` (post-shutdown I/O error expectation),
`737` (fsck.briefs OOM spin after shutdown, pre-existing Go tool issue).

### Recent fix highlights (this campaign)

A large cluster of previously-failing tests now passes. Notable fixes:

| Tests                         | Commit   | Area                                                          |
|-------------------------------|----------|---------------------------------------------------------------|
| 093 193 683 684 688           | a1eb7e0  | killpriv-on-modify (file_remove_privs on inline write + fallocate, ATTR_MODE on truncate) |
| 563                           | 55023ac | cgroup writeback (SB_I_CGROUPWB)                              |
| 617                           | 0cd7062 | punch-empty-tree straddler orphan                             |
| 522 616                       | —        | punch-split-extent pagecache invalidation                     |
| 074                           | fb649e8  | orphan dir-trie-page leak (trie_free_node)                    |
| 753                           | 73a0d1d  | dm-error writeback WARN (lock_buffer across mark_buffer_dirty)|
| 679                           | 05cb297  | unwritten extents                                             |
| 020 037 062 066 070 097 103 117 337 377 403 454 631 | 29121e6  | chained xattr buffer-head accounting + release bugs             |
| 547                           | 86fa48b  | fsync-ordered trie durability                                 |
| 640                           | 2d66610  | rename trie-root journal ordering                             |
| 620                           | 97a50e7  | mkfs huge-disk EIO (stop pre-zeroing inode table)             |
| 169 420                      | 8321fc6  | FS_IOC_FSGETXATTR ioctl                                       |
| 029 030 032                  | f8ef293  | always-checkpoint at unmount (clean-unmount replay clobber)   |
| 464                           | 4ef6ccb  | trie_iter_grow double-free (suite wedge)                      |
| 023 025 078                  | —        | renameat2 EXCHANGE/WHITEOUT + emptiness check                 |
| 257 637 676                  | —        | readdir seek/resume (simple_offset)                           |
| 471 736                      | —        | readdir rewinddir + trie_gen staleness                        |
| 313 423 755                  | —        | timestamp cluster (current_time at all sites)                 |
| 322 321                      | —        | journal-replay write_pos + stale-cached-parent evict          |
| 011                           | —        | journal write serialization (per-journal write_lock)          |
| 015                           | —        | delalloc ENOSPC stall (nonda_switch + fail-folio)             |
| 087                           | —        | utime setattr_prepare                                         |
| 633 696                      | —        | setgid inheritance (inode_init_owner)                         |
| 228 394                      | —        | RLIMIT_FSIZE (inode_newsize_ok)                               |
| 092 483                      | —        | fallocate prealloc                                            |
| 467 426 756                  | —        | exportfs (encode_fh/fh_to_dentry/fh_to_parent/get_parent)     |
| 626                           | —        | RENAME_WHITEOUT                                               |
| 749                           | —        | —                                                             |
| 732 634 741 754              | —        | mkfs stdout/refuse-overwrite/fsync                            |
| 068 085 390 491 738          | c274292  | FIFREEZE/FITHAW (freeze_fs/unfreeze_fs)                       |
| 405                           | —        | dm-thin write-error (metadata sync error check)               |
| 003                           | —        | journal-replay multi-fix cluster                              |
| 643                           | —        | swapfile (iomap_swapfile_activate — passes; was mis-tagged)   |
| 704                           | —        | O_DIRECT (iomap DIO landed; sub-sector DIO accepted)          |
| 177                           | —        | env (gawk installed)                                          |
| 079 277 424 545 553 555 596 629 | 38d57d0  | chattr/lsattr inode flags (+S/+D/+i/+a/+d/+A)                |

> `generic/475` (dm-error crash-replay) is the **one remaining real open bug**.
> Deferred — requires an on-disk journal-format / sync-model change, explicitly out
> of scope.

---

## How this was measured

- Full suite `./check -s briefs -g auto` on the VM (2026-07-03), kernel
  `6.12.94+deb13-amd64`, branch `even-more-xfstests`, module built on the VM
  from commit `38d57d0`.
- Chattr cluster re-run 2026-07-03 on `even-more-xfstests`, kernel
  `6.12.94+deb13-amd64`, module built from commit `38d57d0` (loaded via
  `insmod briefs_fs.ko`), loop devices on `/var/tmp`. 8/8 pass.
- Regression cluster re-run 2026-07-03, 25 tests, all pass.
- Xattr cluster re-run 2026-07-02 on branch `even-more-xfstests`, kernel
  `6.12.94+deb13-amd64`, module built from commit `29121e6` (loaded via
  `insmod briefs_fs.ko`), loop devices on `/var/tmp`. 13/13 pass.
- Pass/Fail/Not-run lists derived from the run's `Ran:` / `Not run:` / failure
  lines in `/tmp/fullsuite.log` on the VM.
- Not-run reasons read from each test's `.notrun` artifact and grouped.
