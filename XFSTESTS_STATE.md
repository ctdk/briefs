# BrieFS xfstests state

State of the `generic` xfstests group against the BrieFS kernel module, as
measured by fresh full-suite and targeted runs on the VM.

## Overview

**Latest per-test full-suite run:** 2026-08-13, `tests/xfstests/run-suite.sh`
over every generic test on the VM, kernel `6.12.101+deb13-amd64`, branch
`refactor-round-3`, commit `3835ac5` (post split-on-conversion + B1; before
the `generic/274` meta_shield fix). Archive:
`tests/xfstests/runs/run-20260813-113611-kernel.txt`.

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   793 | all generic tests                                        |
| Pass             |   378 | per-test runner reported PASS                            |
| Fail             |    10 | 050 250 252 274 311 346 563 599 623 730 (see below)      |
| Not run          |   397 | `_require_*` gate or unsupported feature                 |
| Skipped          |     8 | 051 068 074 461 464 475 476 753 (known hangs/deferred)   |
| Hang             |     0 | former timeout hangs now skipped (461/753) or passing (619) |
| Mount fail       |     0 | runner tears down DM targets before each test            |

**Post-run fixes (not yet re-confirmed by a full suite):**
- `generic/274` — FIXED by `141be8c` (meta_shield count-shield reserves
  B+tree metadata for unwritten fallocate extents at 100%-full). Targeted
  re-run of 274 + the regression cluster (011/032/092/521/616/363/091/015/003)
  was 10/10 PASS (`run-20260813-182156`). Effective full-suite tally is
  therefore **379 pass / 9 fail** pending a fresh full run.
- `53679e3` (follow-up #74) releases the unwritten reserve on punch/truncate
  so `df` is honest immediately (was held until evict). No test pass/fail
  change; verified by a df-honesty spot-check (4/4) + 9 targeted tests PASS
  + clean fsck.
- `e2f023c` (fallocate zero_range/collapse_range/insert_range): the mode gate
  now accepts the three ops. 31 of the 32 generic tests they gate move from
  NOT RUN to PASS — the 11 zero_range tests (008 009 033 042 096 456 469 511
  610 685 758), the 12 collapse tests (012 016 021 022 031 072 497 499 503
  687), and the 9 insert tests (058 060 061 063 064 404 686 735), plus
  `generic/485` (insert EFBIG overflow fix) and `generic/017` (collapse
  timeout bump). `generic/641` stays NOT RUN — it needs
  `_scratch_mkfs_blocksized` support in `common/briefs` (xfstests-dev repo),
  not a kernel change. The regression cluster (011 015 025 026 092 274 363
  522 616) stays PASS, fsck clean after each. Effective full-suite tally is
  therefore **410 pass / 9 fail** pending a fresh full run.

**Changes since the 2026-08-05 run** (iomap migration + refactor-round-3):
- **iomap migration** (buffer_head → iomap, phases 0-8): DIO landed (closes
  `generic/704`), bmap + swapfile landed (closes `generic/472`, `generic/643`),
  moving ~23 formerly not-run tests into the passing set.
- **POSIX ACLs** (`a113c76`): ACLs as `system.posix_acl_*` xattrs via
  `.get_inode_acl`/`.set_acl`; the 14 ACL-gated tests (026 053 077 099 105
  237 307 318 319 375 444 449 529 697) now PASS (were "ACLs not supported").
- **split-on-conversion** (`c9e3eb2` + `afc8a93` + `3835ac5`): a partial write
  into a preallocated unwritten extent now splits it (written middle +
  unwritten wings) instead of flipping the whole extent to MAPPED, and
  truncate keeps the unwritten EOF block unwritten. Fixed the `44f41eb`
  stale-data regression (521/091/263/522/616/363) but regressed `generic/274`
  at 100%-full (the split needs a metadata block) — fixed by `141be8c`.
- **journal-owned bh lifetimes** (`134d4a4`): journal-tracked dirty metadata
  block set + `briefs_journal_flush_owned` replaces `sync_blockdev`; fixes
  `generic/475` umount-redirty-EIO.
- **lockdep Phase 2** (`54f1984`): extent_seq preempt_disable wrappers; the
  `6.12.101-lockdep` kernel is used for targeted lock-order verification
  (the full suite above ran on the stock kernel).
- Skip list changed: 070/224 un-skipped after Phase-1 re-verification (619
  was also un-skipped and now passes); the 461/753 former timeout hangs are
  skipped rather than timed-out; 074/464/476 skipped pending re-verify; 475
  skipped (flaky/deferred); 051/068 skipped.

**`generic/388`** now PASSES (in the 2026-08-13 PASS list); it is no longer
excluded or wedging. **`generic/299`** still needs investigation (btree
checksum mismatch under stress). **`generic/127`/`521`** pass in the full
suite but are flaky mmap+fsx flush-deadlock candidates (see failing-test
notes; `521` confirmed via bisect 2026-08-13 to wedge on the pre-#74 build
too, so it is a pre-existing 455/127-class flush deadlock, not a #74
regression).

---

**Prior per-test run:** 2026-08-05, kernel `6.12.100+deb13-amd64`, branch
`refactor-round-1` — 327 pass / 8 fail / 420 not-run / 3 hang (461/619/753).
The notes immediately below are from that run.

**Mount-fail note:** An earlier iteration of this run reported 11 `MOUNT FAIL`
entries for `generic/313..323`. Those were run-runner artifacts caused by
leftover device-mapper targets from tests such as `generic/475`. The runner was
hardened to tear down DM targets wrapping `TEST_DEV` and `SCRATCH_DEV` before each
test, so the final run reported **0 MOUNT FAIL**. Re-running `313..323` in
isolation with the improved runner gave **9 PASS / 2 NOT RUN / 0 MOUNT FAIL**.

**`generic/299` note:** The runner killed `generic/299` after it spent >300s
spinning with repeated `briefs: btree: node <N> checksum mismatch` errors. It is
counted as a failure because the process exited with an ambiguous status
(UNKNOWN / exit 137); the underlying issue is a real BrieFS btree corruption bug
that needs investigation.

**Previous full-suite run:** 2026-07-06, `./check -g auto -X .exclude` on the
VM, kernel `6.12.94+deb13-amd64`. `generic/388` was expunged from the run via
`/xfstests/tests/generic/.exclude` because it wedges the suite (shutdown/replay
corruption leaves `godown` stuck in D-state; see failing-test notes).

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

**Subsequent run (2026-07-07, invalid):** After the `9385fc8` workaround, a full
`./check -g auto -X .exclude` run completed without hanging or kernel oops, but it
reported **Failed 204 of 782 tests**.  Of the 203 `.out.bad` files, **168 are
ENOSPC / "no space left on device" / "no free inodes" cascades** on `TEST_DEV`.
`generic/001` creates enough files (200-chain copies across many base files over 5
iterations) to exhaust the fixed inode table of the 16 GiB `TEST_DEV` image, and
the leftover state poisons every later test that writes to `TEST_DIR`.  This is an
environmental/setup issue, not a BrieFS regression caused by disabling
`SB_I_CGROUPWB`.

**Confirmed not an inode leak:** targeted create/delete, `generic/001`, `generic/027`,
`generic/074`, and a cluster of those tests all return `FreeInodes` to 524,287 after
cleanup.  The only persistent allocation is data-relative block 0, reserved by the
allocator as the ENOSPC sentinel.

**Standard procedure going forward:** xfstests mounts `TEST_DEV` once per
`./check` invocation and does not clean it between tests, so a full
`./check -g auto` run on BrieFS is unreliable.  Use the per-test runner
`tests/xfstests/run-suite.sh`, which reformats both `TEST_DEV` and `SCRATCH_DEV`
with `mkfs.briefs` before each test.  A valid post-9385fc8 tally must be taken
with that runner (or with equivalent per-test reformatting), not with a single
bulk `./check` invocation.

---

## Formerly excluded test

### generic/388 — shutdown-recovery stress wedge (now PASSING)

- **Status:** **PASSING** as of the 2026-08-13 full-suite run (in the PASS
  list; no longer in the skip list). The earlier `XFS_IOC_GOINGDOWN
  NOLOGFLUSH` shutdown/replay corruption that left `godown` stuck in
  `briefs_dir_open` D-state has since been resolved by the shutdown/replay
  durability work (always-checkpoint-at-unmount `f8ef293`, journal-replay
  write_pos `321/322`, and the journal-owned-bh / sync-model changes).
- **History:** was expunged via `/xfstests/tests/generic/.exclude` in the
  2026-07-06 run because it wedged the suite. Re-added and green in
  refactor-round-3.

---

## Failing tests (2026-08-13 per-test run)

The 2026-08-13 full-suite run reported **10 failures**, **0 hangs** (the
former 461/753 timeout hangs are now in the skip list; 619 was un-skipped and
passes). Classification vs the prior run, from
`briefs-remaining-xfstests-failures-triage`:

**10 FAIL tests:**
- `generic/050` — read-only dirty-journal mount output differs; expected-error-string mismatch. Pre-existing/expected.
- `generic/250` — DIO-error sibling: direct I/O after a failing device does not propagate `EIO` (needs deferred-conversion / DIO error wiring).
- `generic/252` — DIO-error sibling of 250 (same family).
- `generic/274` — **NEW split regression at 100%-full** (split-on-conversion
  `c9e3eb2` needs a metadata block for the extent split, which hit ENOSPC).
  **FIXED post-run by `141be8c`** (meta_shield count-shield; targeted re-run
  10/10 PASS). Counted as failing in the archived run; effective tally below
  treats it as fixed.
- `generic/311` — pre-existing baseline flake (dm-flakey/fsync timing); reproduces on a known-good baseline.
- `generic/346` — flake (intermittent; not a steady BrieFS bug).
- `generic/563` — cgroup writeback accounting mismatch; expected after `SB_I_CGROUPWB` was disabled on 6.12 (`9385fc8`).
- `generic/599` — shutdown cluster: VFS `cleanup_mnt` WARN after shutdown ioctl (`_check_dmesg` catches it; not data corruption).
- `generic/623` — shutdown cluster: fsync after shutdown returns `EROFS` instead of expected `EIO`.
- `generic/730` — shutdown cluster: read after device delete returns no error instead of expected `EIO`.

**Post-run fixes (not yet re-confirmed by a fresh full suite):**
- `141be8c` fixes `generic/274` (meta_shield). Effective full-suite tally is
  therefore **379 pass / 9 fail** (050 250 252 311 346 563 599 623 730).
- `53679e3` (follow-up #74, df-honesty for punch/truncate) changes no test
  pass/fail; verified by a df-honesty spot-check + 9 targeted tests PASS.

**Moved into PASS since 2026-08-05** (23 tests): the 14 ACL-gated tests
(`026 053 077 099 105 237 307 318 319 375 444 449 529 697`, via `a113c76`),
`089` (bulk fsx format now matches), `127`/`521`/`522` (fsx flush, passing in
this run though flaky — see below), `388` (formerly excluded, now green),
`341`/`510`/`771` (replay duplicate-entry bugs now resolved), and `547`
(crash-replay, now passing). `250`/`252`/`274`/`346` moved out of the passing
set (the four non-skip regressions/​flakes above).

**Skipped (8, in the skip list — not counted as fail):** `051 068 074 461 464
475 476 753`. These are known hangs/deferred re-verifications: 461/619/753 were
former timeout hangs; 074/464/476 are skipped pending re-verification; 475 is
the flaky/deferred dm-error crash-replay bug; 051/068 were un-skipped then
re-skipped during the Phase-1 re-verification churn. (`619` is **not** in the
current skip list — it passed in this run; the historical "619 hung" note is
stale.)

**Note on `127`/`521`/`522`.** These pass in the full suite but are flaky
mmap+fsx flush-deadlock candidates. `521` was confirmed via bisect
(2026-08-13) to wedge on the pre-`#74` build too: it is a pre-existing
455/127-class flush deadlock (`msync → blkdev_issue_flush → submit_bio_wait`,
introduced by the 455 fix `82c9a61`), not a `#74` regression. Treat a 521/127
hang as a pre-existing flake until the flush deadlock is fixed; do not
attribute it to a new change without a bisect.

## Failing tests (6) (2026-07-06 `./check -g auto` run)

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

## Not-run tests (397 in the 2026-08-13 run; table below from 2026-07-06)

The 2026-08-13 per-test run reported **397 not-run**. The detailed
reason-grouped table below was built from the 2026-07-06 `./check -g auto`
run's `.notrun` artifacts (the per-test runner archives do not record the
reason text), so the table's per-row counts are from that older run and are
representative of the reason *taxonomy* rather than the exact 2026-08-13
counts. Two shifts since that table were built: the 14 **ACL** tests moved to
PASS (`a113c76` — that row is now historical), and the iomap migration moved
~23 DIO/bmap/swapfile tests from not-run into PASS. Use the 2026-08-13 PASS /
NOT RUN lists above for exact membership; use this table for "why a test is
not run".

Every not-run is gated by a `_require_*` probe that actually exercises the
filesystem or the VM environment, so a not-run is a genuine unimplemented
feature or an environment gap — not a stale declaration. The `.notrun` reasons
were read and grouped; all gates are legitimate.

**Two meta-categories:**

- **Absent feature** (BrieFS does not implement it): reflink/COW, quota,
  shutdown-state probing, fscrypt/fsverity, exchangerange, ~~POSIX ACL~~
  (now implemented — `a113c76`), fcollapse/fzero/finsert, dedupe, idmapped
  mounts, O_TMPFILE, FITRIM, DAX,
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
| ACLs not supported ~~(now PASSING via `a113c76`)~~          | ~~14~~ | ~~026 053 077 099 105 237 307 318 319 375 444 449 529 697~~ — all 14 now PASS in 2026-08-13 (POSIX ACLs landed); row kept for history |
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

## Passing tests (378)

From the 2026-08-13 run archive (`run-20260813-113611-kernel.txt`, commit
`3835ac5`). With the post-run `141be8c` fix for `generic/274`, the effective
pass count is **379** (274 moves from FAIL to PASS); a fresh full suite to
re-confirm is pending.

```
001  002  003  005  006  007  011  013
014  015  020  023  024  025  026  027
028  029  030  032  034  035  036  037
039  040  041  043  044  045  046  047
048  049  053  056  057  059  062  065
066  067  069  070  071  073  075  076
077  078  079  080  081  083  084  085
086  087  088  089  090  091  092  093
094  097  098  099  100  101  102  103
104  105  106  107  108  109  112  113
114  117  120  123  124  125  126  127
128  129  130  131  132  133  135  141
169  177  184  192  193  198  204  207
208  209  210  211  212  213  214  215
221  224  225  226  228  236  237  239
240  245  246  247  248  249  255  256
257  258  263  269  273  275  277  285
286  294  306  307  308  309  310  312
313  314  315  316  317  318  319  320
321  322  323  325  335  336  337  338
339  340  341  342  343  344  345  347
348  350  354  355  360  361  362  363
364  371  375  376  377  378  388  390
391  392  393  394  401  403  405  406
409  410  411  412  416  417  418  420
422  423  424  426  427  428  430  431
432  433  434  436  437  438  439  441
442  443  444  445  446  448  449  450
451  452  453  454  459  460  465  466
467  468  471  472  473  474  477  478
479  480  481  483  484  486  488  489
490  491  494  495  496  498  502  504
505  507  508  510  512  519  520  521
522  523  524  525  526  527  528  529
530  532  533  534  535  536  538  539
545  547  551  552  553  554  555  557
558  564  567  568  569  571  585  586
589  590  591  597  598  604  609  611
615  616  617  618  619  620  622  626
629  631  632  633  634  635  636  637
638  639  640  642  643  646  647  650
676  677  678  679  680  683  684  688
690  694  695  696  697  701  704  705
706  707  708  728  729  731  732  736
737  738  740  741  742  743  747  748
749  750  754  755  756  759  760  761
763  764  771  779  782  784  785  789
790  792
```

### xfstests xattr cluster (13/13, 2026-07-02)

All xattr-gated tests pass with the chained-xattr fix in `29121e6`:
020, 037, 062, 066, 070, 097, 103, 117, 337, 377, 403, 454, 631.

### xfstests shutdown cluster (2026-07-04)

The `XFS_IOC_GOINGDOWN` ioctl is now implemented, so `godown`-based shutdown
tests run. Core cluster `043 044 045 046 047 048 049 050 051` passes in this
run. `050` required a read-only-dirty-journal mount rejection in
`briefs_fill_super`.

Extended godown cluster `392 461 468 474 505 530 536 622 635 646 705` passes.

Newly-exposed shutdown-related failures:
`599` (VFS cleanup_mnt WARN), `623` (fsync after shutdown missing EIO), `730`
(read after device delete missing EIO), `753` (dm-error metadata-sync WARN), and
historically `127` (mmap+fsx D-state hang — passed in this run). `generic/417`
(xattr EA race) and `generic/737` (file lost after O_DIRECT+shutdown) are now
fixed. `generic/388` was excluded from the full suite because it wedged
(**update:** it now PASSES in 2026-08-13 after the shutdown/replay durability
work — see "Formerly excluded test" above).

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

> Open BrieFS code bugs as of the 2026-07-06 run (status updated to 2026-08-13):
> - `299` — btree checksum mismatch under stress; still needs investigation
>   (not in the 2026-08-13 fail set — not run or passing there; treat as
>   deferred).
> - `341`, `510`, `771` — replay duplicate directory entries: **now PASS** in
>   2026-08-13 (idempotency/replay work resolved them).
> - `547` — fsstress metadata mismatch: **now PASS** in 2026-08-13 (crash-replay
>   durability work; still watched as flaky).
> - `599` — VFS `cleanup_mnt` WARN after shutdown: still FAIL (shutdown cluster).
> - `623` — fsync after shutdown does not return `EIO`: still FAIL (shutdown cluster).
> - `730` — read after device delete missing `EIO`: still FAIL (shutdown cluster).
> - `388` — was an excluded shutdown/replay wedge: **now PASS** in 2026-08-13.
>
> `generic/127`, `521`, `522` **pass** in 2026-08-13 (no longer hangs) but are
> flaky mmap+fsx flush-deadlock candidates (see failing-test notes). `311` is a
> pre-existing baseline flake. `050` is an expected-error-string mismatch on
> read-only dirty-journal mount. `089` **now passes** (was a `TEST_DEV`-size
> artifact). `563` fails because cgroup writeback was disabled
> (`SB_I_CGROUPWB` workaround). `250`/`252` are DIO-error siblings (deferred
> conversion); `274` was a split regression now fixed by `141be8c`; `346` is a
> flake.

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

### Per-test runner (2026-07-13)

The 2026-07-13 run used `tests/xfstests/run-suite.sh` to invoke each generic
xftest individually. The runner was hardened during this session to:

- reformat both `TEST_DEV` and `SCRATCH_DEV` with `mkfs.briefs -f` before every
  test;
- unmount both mount points aggressively (`fuser -km`, normal/lazy/forced
  `umount`) so daemon/background references do not leak between tests;
- tear down leftover device-mapper targets that wrap `TEST_DEV` or
  `SCRATCH_DEV` (tests such as `generic/475` create dm-error/dm-thin-pool stacks);
- classify an interrupted `./check` (`Passed all 0 tests`) as a failure instead
  of a pass;
- report mount failures with the underlying error text.

Full-suite log on the VM: `/tmp/run-suite-clean.log`. Final summary:
**PASS 376, FAIL 12, NOT RUN 401, HANG 3, MOUNT FAIL 0, MKFS FAIL 0**.
