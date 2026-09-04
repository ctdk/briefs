# BrieFS xfstests state

State of the `generic` xfstests group against the BrieFS kernel module, as
measured by fresh full-suite and targeted runs on the VM.

## Overview

**Current state (2026-09-04, master `e6d3d6d`):** the generic/112 btree
checksum mismatch is root-caused and fixed. A range delete frees emptied
leaves and drops only their parent idx entries, so a surviving
predecessor's `next_leaf` dangles at the freed block, whose cached
buffer keeps a corpse image (the delete path zeroes the leaf payload
during compaction; the freed branch never rewrites num_keys or the
checksum). `btree_lower_bound_block` was the one reader that followed
the chain, so a trust=false walk into the corpse failed the CRC — the
mismatch splat — and after block reuse the chain would have named a
valid leaf at the wrong key range for trust=true readers. The bound is
now derived from the idx structure (leaf miss bubbles -ENOENT to the
parent, which retries right-sibling children); `next_leaf` is
write-only in the kernel, on-disk images unchanged. Instrumented
evidence: 5/5 captured splats had the block free in the allocator
bitmap, all 126 slots zero, buffer frozen, disk coherent-or-reused.
Verification: generic/112 loop 5 splats → 0 over 12 runs; regression set
091 092 112 209 263 363 551 617 679 127 521 passes 11/11 with fsck
validation enabled (516/517 are environmental not-runs: dedupe
unsupported); full-suite bracket below.

**Latest full-suite run:** 2026-09-04, every generic test on the VM,
kernel `6.12.101-lockdep`, at the leaf-chain fix content (`e6d3d6d`;
the archive's commit field records the pre-commit tree state). All 793
tests ran fresh (RESUMED 0), 676 at the default 300s timeout.
Archive: `tests/xfstests/runs/run-20260904-063947-kernel.txt`.

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   793 | all generic tests                                        |
| Pass             |   456 | flaky tier 127 299 340 388 538 617 all PASS              |
| Fail             |     3 | 311 547 563 (all accepted/flaky, see below)              |
| Not run          |   332 | `_require_*` gate or unsupported feature                 |
| Skipped          |     2 | 475 492 (skip list honored)                              |
| Hang             |     0 |                                                           |
| Mount fail       |     0 | runner tears down DM targets before each test            |

vs the 2026-09-03 baseline (455/4/0): `538` (DIO unaligned-AIO flake)
passed this run (+1); `311`, `547` (475-family flake) and `563`
(cgroup writeback, disabled) fired as always. Zero btree checksum
mismatches and zero lockdep splats for the whole run+boot; deltas are
known flakes only, none related to the reader-side-only change.

**Prior state (2026-09-03, master `facf534` + `c1e5445`):** the quadratic
seekdir follow-up is closed. `briefs_readdir` now checkpoints the trie
iterator's settled post-emit state every 64 emitted entries (per-fd, in
`struct trie_iter`; generation-tagged, cleared on `trie_gen` mismatch)
and `seekdir` restores the nearest checkpoint below the target and skips
< stride entries, so a seek costs O(stride) instead of O(N). `generic/676`
(`t_readdir_3`, 4000 files) drops **~604s → ~58s** and PASSES under the
restored 300s default timeout — the 1200s harness override from `c356c9f`
is reverted (`c1e5445`). No on-disk format change (offsets and
checkpoints are session-local). The 30000-file thinning path (stride
ratchet 64→512 at the 128 KiB blob budget) passes, and the readdir
regression set `003 080 112 257 471 637 676 736` passes 8/8 with fsck
validation enabled. Full-suite 455/4(311 538 547 563)/0,
archive `tests/xfstests/runs/run-20260903-175757-kernel.txt`.

- **generic/112 btree checksum mismatch — CLOSED by `e6d3d6d`** (was the
  open item from the seekdir cycle): root-caused to the dangling
  `next_leaf` chain link followed by `btree_lower_bound_block`; see the
  2026-09-04 section above. Full investigation tables live in
  `~/src/briefs-notes/112-btree-checksum-mismatch-triage.md` (session
  notes) and the memory topic file.

**Prior state (2026-09-02, master `c356c9f`):** the deferred **problem-2**
lockless read-vs-writeback btree race is fixed (`5ccf40a` + `d03e776`:
per-inode `extent_lock` converted mutex→rwsem; every `trust_verified=false`
tree read — root-pointer snapshot included — now holds it shared; mutators
exclusive as before; `briefs_btree_drain` stays lock-free by design under
`j->write_lock`). The flaky tier collapsed: **zero btree checksum mismatches
across the whole cycle** (baseline was ~169/boot under 299 stress), zero
lockdep splats after the exchange follow-up below, and the previously-flaky
stress tests are now deterministic:

- **Stochastic verification (one boot):** `299` 10/10, `340` 8/8, `127` 5/5,
  `074` PASS, 1005b2e 18-test regression cluster **18/18**.
- **`generic/720` lockdep splat — pre-existing, fixed `aeca44e`:**
  `briefs_lock_two_inodes` took two plain `inode_lock()`s (same i_rwsem
  lockdep class, no nesting notation). Reproduces on the ddcb7ef baseline
  on a fresh lockdep boot (the splat pattern predates the rwsem work; it
  was never seen before because full-suite boots don't reset lockdep
  state, which self-disables after the first report). Fix: exchange
  guarantees S_ISREG on both inodes, so the open-coded pair became the
  exported `lock_two_nondirectories()`/`unlock_two_nondirectories()`
  (second lock taken with the I_MUTEX_NONDIR2 subclass), plus
  `down_write_nested` for the second `extent_lock` of
  `briefs_lock_two_extents` — the same-class pair lockdep would trip
  next. Verified: 720 PASS, zero dmesg findings, fresh boot.
- **`generic/676` HANG in the 09-01 full run — timeout marginality, not a
  regression:** `t_readdir_3` seeks to 4000 random positions per ops-mode;
  `briefs_readdir` implemented seekdir as iterator re-init + linear skip, so
  the test was quadratic (~604s measured, vs the 300s default). Reproduced
  on the ddcb7ef baseline module isolated; the binary terminates cleanly
  ("All tests passed"). Harness fix `c356c9f`: 676 timeout → 1200s.
  **Resolved 2026-09-03 by the checkpointed seekdir (`facf534`); the
  override is reverted (`c1e5445`) and 676 PASSES at the 300s default.**
- **Hard FAIL (accepted non-PASS):** `311` (dm-flakey + fsync timing,
  reproduces on known-good baselines), `563` (cgroup writeback disabled on
  6.12, iput CVE workaround). `475`/`492` stay on the skip list (see
  below); `538` (DIO unaligned-AIO flake) passed this run.

**Latest full-suite run:** 2026-09-01, every generic test on the VM, kernel
`6.12.101-lockdep`, at the rwsem fix content (`5ccf40a` + `d03e776`, plus
working-tree `aeca44e` for 720 — the archive's commit field records the
pre-commit tree state).
Archive: `tests/xfstests/runs/run-20260902-001517-kernel.txt`.

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   793 | all generic tests                                        |
| Pass             |   456 | flaky tier all PASS: 074 127 299 340 388 538 617 720     |
| Fail             |     2 | 311 563 (both accepted non-PASS)                          |
| Not run          |   332 | `_require_*` gate or unsupported feature                 |
| Skipped          |     2 | 475 492 (skip list honored this run)                     |
| Hang             |     1 | 676 — timeout marginality; fixed in harness (`c356c9f`)   |
| Mount fail       |     0 | runner tears down DM targets before each test            |

vs the `ddcb7ef` baseline (456/5/0 with 475+492 unhonored-skips): identical
pass count, `538` FAIL→PASS, `676` PASS→HANG resolved as harness
marginality, and the problem-2 flakes (`127`/`340`/`299` residuals) are
gone — 0 checksum mismatches for the entire run+boot.

**Historical baseline (2026-08-31, master `ddcb7ef`):** the Phase-2 journal-owned-pin
regression cycle is complete and the suite is at its best baseline yet —
**456 pass / 5 fail / 0 hang** (2026-08-30 full run). All 5 fails are accepted
non-PASS; there are **no open BrieFS regressions** from the Phase-2 work.

**Current state (2026-08-31, master `ddcb7ef`):** the Phase-2 journal-owned-pin
regression cycle is complete and the suite is at its best baseline yet —
**456 pass / 5 fail / 0 hang** (2026-08-30 full run). All 5 fails are accepted
non-PASS; there are **no open BrieFS regressions** from the Phase-2 work.

- **Hard FAIL (5, all accepted non-PASS):**
  - `311` — pre-existing baseline flake (dm-flakey + fsync timing); reproduces
    on a known-good baseline.
  - `475` — dm-error crash-replay, **accepted ~50%**: the residual is the C2
    block-layer-capped mode, not BrieFS-addressable. The Phase-2 pin design
    (`5524267`) fixed the pdflush-drift mode that motivated the old "needs a
    journal-format change" framing — that framing is superseded.
  - `492` — libblkid has no BrieFS probe; the kernel `FS_IOC_*FSLABEL` ioctls
    work — a `util-linux` task (skip-list entry since `991faac`).
  - `538` — DIO unaligned-AIO flake (0x5a data pattern, not metadata); was
    PASS at `ca4478b`, fired in the 08-30 run.
  - `563` — cgroup writeback intentionally disabled (`9385fc8`, 6.12 iput
    CVE-2026-31703 workaround; not a BrieFS defect).
- **Run-config note (08-30 run):** `SKIP_TESTS="475 492"` is recorded in the
  archive but was **not honored** by that run config — both ran and FAILed, so
  the run reports 0 skipped instead of 2. A category shift only; both are
  accepted non-PASS either way.
- **Flaky tier (not deterministic):** `127`/`340` (the deferred **problem-2**
  lockless read-vs-writeback btree race — the `299` residual; `340` confirmed
  flake via isolated x8 = 8/8 PASS, 2026-08-29; both passed the 08-30 full
  run), `388` (crash-recovery soak flake, isolated x8 = 8/8 PASS), `617`
  (~6% io_uring DIO soak flake), `547` (475-family), `521`
  (455/127-class `msync → blkdev_issue_flush → submit_bio_wait` deadlock,
  pre-existing, VM-reboot-only).
- **`299` — PARTIALLY FIXED, passes ~70-80%** (`1005b2e` + `0fd1448`; it
  passed the 08-30 full run). Residual = problem-2 above. See
  `briefs-dio-stress-cluster-250-252-299-triage`.
- **`720` — un-skipped and PASSING** since `3666a3e` (~58 s).

### The Phase-2 pin cycle (2026-08-27 → 08-30, closed)

Phase 2 of the journal-owned-bh lifetimes work (`5524267`) changed deferred
metadata writeback to **pin** buffers (`get_bh`, deliberately not `BH_Dirty`)
from dirty-time until commit, so pdflush can never write drifted metadata
between journal commits (the `generic/475` root cause). It introduced a
19-test regression, fixed in two steps:

1. **Pin-survives-free data aliasing** — the pin survived `briefs_free_block`'s
   `clean_bdev_aliases` (a no-op against a pinned, not-dirty buffer), so
   checkpoint's `flush_owned` wrote stale trie/btree content (TRNP/BTRE/ERTB
   magic + stored filenames) onto blocks already reused as data extents.
   **Fixed `ca4478b`**: `briefs_journal_untrack_bh` before
   `clean_bdev_aliases` in the allocator free paths (`alloc.c`). 16/19
   recovered in the next full suite; the aliasing magic is gone from every
   dmesg since.
2. **`generic/676` full-suite-only flush deadlock** — the pin was held across
   the `sync_dirty_buffer` wait, leaving the whole owned batch unreclaimable
   and pdflush-ineligible; under the full-suite loaded cache the sync write
   could never obtain memory (silent D-state freeze, both CPUs, lockdep-silent;
   passes isolated, so only a full suite can confirm a fix).
   **Fixed `ddcb7ef`**: two-pass `briefs_journal_flush_owned` — pass 1 drops
   ALL pins at commit (`mark_buffer_dirty` + `brelse`) before any sync wait;
   pass 2 re-resolves each block via `sb_bread` (the stored bh can dangle once
   unpinned) and syncs + quiesces per-buffer.

Cycle result: **17/19 recovered**; the remaining two (`127`/`340`) are the
pre-existing problem-2 flakes above, not Phase-2 residuals.

**Latest full-suite run:** 2026-08-30, `tests/xfstests/run-suite.sh` over every
generic test on the VM, kernel `6.12.101-lockdep`, branch `master`, at the
`ddcb7ef` fix content (the archive's `commit:` field records `f99b741` — the
runner captured HEAD before the fix commit landed; the code under test was the
two-pass flush in the working tree).
Archive: `tests/xfstests/runs/run-20260830-161504-kernel.txt`.

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   793 | all generic tests                                        |
| Pass             |   456 | per-test runner reported PASS                            |
| Fail             |     5 | 311 475 492 538 563 (all accepted non-PASS, see above)   |
| Not run          |   332 | `_require_*` gate or unsupported feature                 |
| Skipped          |     0 | skip list not honored this run (see run-config note)     |
| Hang             |     0 | 676 fixed (`ddcb7ef`)                                    |
| Mount fail       |     0 | runner tears down DM targets before each test            |

**Phase-2 regression bracket** (three clean-tree full suites, same kernel and
runner; archives committed in `f99b741` and `caa4196`):

| Baseline                          | Commit     | Pass | Fail | Hang | Skip | Archive |
|-----------------------------------|------------|-----:|------|-----:|-----:|---------|
| Phase-2 landing (the regression)  | `5524267`  |  437 | 21   | 1 (676) | 2 | `run-20260827-225818` |
| untrack-before-free fix           | `ca4478b`  |  452 | 6 (127 299 311 340 388 563) | 1 (676) | 2 (475 492) | `run-20260828-193729` |
| two-pass flush fix (current)      | `ddcb7ef`  |  456 | 5 (311 475 492 538 563)    | 0 | 0* | `run-20260830-161504` |

\* skip list not honored (475/492 ran); the skip entries themselves are
unchanged.

vs the pre-Phase-2 reference `c4082e7` (2026-08-18, 455/4/0): net **+1 pass**
with `299` now in PASS and `720` un-skipped, at the cost of `538` (known DIO
flake) firing this run. vs `ca4478b`: `676` HANG→PASS plus the flaky tier
(`127`/`299`/`340`/`388`) all landing PASS this run; `538` went the other way.

### Pre-Phase-2 reference run: 2026-08-18 (`c4082e7`)

The last full suite before the Phase-2 pin work (and the reference the Phase-2
regression was measured against): `tests/xfstests/run-suite.sh` over every
generic test on the VM, kernel `6.12.101-lockdep`, branch `master`, commit
`c4082e7`. Read the table and notes below as a historical point-in-time.
Archive: `tests/xfstests/runs/run-20260818-181855-kernel.txt`.

| Bucket           | Count | Notes                                                    |
|------------------|------:|----------------------------------------------------------|
| Selected         |   793 | all generic tests                                        |
| Pass             |   455 | per-test runner reported PASS                            |
| Fail             |     4 | 299 311 492 563 (see below)                              |
| Not run          |   332 | `_require_*` gate or unsupported feature                 |
| Skipped          |     2 | 475 720 (known hangs/deferred)                           |
| Hang             |     0 | former timeout hangs now skipped or passing              |
| Mount fail       |     0 | runner tears down DM targets before each test            |

**The 4 fails are all known / pre-existing / deferred — zero new regressions**
vs the 2026-08-13 full run (`3835ac5`: 378 pass / 10 fail / 397 not-run / 8
skipped):

- `generic/311` — pre-existing baseline flake (dm-flakey + fsync timing).
- `generic/563` — cgroup writeback accounting; expected after `SB_I_CGROUPWB`
  was disabled on 6.12 (`9385fc8`, iput CVE-2026-31703 workaround).
- `generic/299` — **partially fixed** (`1005b2e` + `0fd1448`, 2026-08-19): the
  fallocate O(blocks) HANG and the commit-under-writeback / reuse-torn-write
  btree checksum races are fixed; 299 now passes ~70-80% of runs (was 0% hard
  HANG/FAIL). Residual = the deferred **problem-2** lockless read-vs-writeback
  btree race (see "Current state" above). See
  `briefs-dio-stress-cluster-250-252-299-triage`.
- `generic/492` — harness gap: the kernel `FS_IOC_GET/SETFSLABEL` ioctls work
  (label set + read back); only the two `blkid` lines fail because libblkid
  has no BrieFS probe — a separate userspace (`briefs-utils`) task. **Skipped
  since `991faac`** (see below).

**Resolved since the 2026-08-13 run** (now PASS; were FAIL in 0813 or 0814):
`089` (`8780683`, trie name-store errors + lazy name-heap compaction),
`536` (`33e4019`, stale inode-snapshot replay on inode-slot reuse — the earlier
"data=ordered" framing was wrong), `250`/`252` (`19ba019`, defer
unwritten→written conversion to DIO end_io and convert only on `!error`),
`623`/`730` (`29f4572`, shutdown error propagation: fsync/read_iter early
`-EIO` + `.shutdown` super_op), and `050`/`274`/`346`/`599` (now pass). Net:
pass 378→455 (+77), not-run 397→332 (−65, more tests runnable). PASS now
includes the fallocate zero_range/collapse_range/insert_range suite
(`e2f023c`), the file-range exchange suite (`56d1aa7` + Tier-3 `7fa96e5`),
POSIX ACLs (`a113c76`), and the iomap DIO/bmap/swapfile tests.

**Provisioning note:** `fsgqa`/`fsgqa2` QA users are now created by
`tests/xfstests/setup-vm.sh` (`3144a46`), so the ~26 `_require_user`-gated
tests run on a fresh VM (previously they existed only because they had been
added by hand). The mount points need no special ownership — tests run
`./check` as root and `chown` a subdir/file to `fsgqa` before invoking it.

**`generic/127`/`521`/`522`** pass in this run but remain flaky mmap+fsx
flush-deadlock candidates (455/127-class; `521` confirmed via bisect
2026-08-13 to wedge on the pre-`#74` build too — a pre-existing
`msync → blkdev_issue_flush → submit_bio_wait` deadlock from the 455 fix
`82c9a61`, not a `#74` regression). Treat a 521/127 hang as a pre-existing
flake until the flush deadlock is fixed.

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
  388 FAILed once in the 2026-08-28 (`ca4478b`) full suite ("can't read
  superblock", empty dmesg — the RANDOM soak flake firing under contention),
  which was **confirmed a flake via isolated x8 = 8/8 PASS** (2026-08-29); it
  passed the 2026-08-30 full suite. Not a regression.
- **History:** was expunged via `/xfstests/tests/generic/.exclude` in the
  2026-07-06 run because it wedged the suite. Re-added and green in
  refactor-round-3.

---

## Failing tests (2026-08-30 full-suite run — current)

The 2026-08-30 run at the `ddcb7ef` fix content (archive
`run-20260830-161504`) reported **5 failures, 0 hangs**. All five are accepted
non-PASS — there are **no open BrieFS regressions**:

- `generic/311` — pre-existing baseline flake (dm-flakey + fsync timing);
  reproduces on a known-good baseline.
- `generic/475` — dm-error crash-replay, **accepted ~50%** (`ca4478b`-era
  triage): the residual is the C2 block-layer-capped mode. The Phase-2 pin
  design (`5524267`) fixed the pdflush-drift mode; the earlier "needs a
  journal-format change" framing is superseded. Ran this run only because the
  skip directive wasn't honored.
- `generic/492` — libblkid-probe gap (kernel `FS_IOC_*FSLABEL` ioctls work; a
  `util-linux` task). Skip-list entry since `991faac`; ran for the same
  config reason.
- `generic/538` — DIO unaligned-AIO flake (0x5a data pattern, not metadata);
  PASS at `ca4478b`, fired under this run's contention.
- `generic/563` — cgroup writeback accounting; expected after
  `SB_I_CGROUPWB` was disabled on 6.12 (`9385fc8`, iput CVE-2026-31703
  workaround).

**Watched but not failing this run (the flaky tier):** `127`/`299`/`340`/`388`
all passed — their race windows don't reliably fire even under full-suite
contention (`340` and `388` confirmed flakes via isolated x8 = 8/8 PASS,
2026-08-29). See the Overview flaky-tier notes.

---

## Failing tests (2026-08-18 per-test run — pre-Phase-2 reference)

The 2026-08-18 full-suite run (commit `c4082e7`, archive
`run-20260818-181855`) reported **4 failures**, **0 hangs**. All four are
pre-existing / deferred — there are **no new regressions** vs the 2026-08-13
run.

**4 FAIL tests:**
- `generic/299` — **partially fixed** (`1005b2e` + `0fd1448`, 2026-08-19): the
  fallocate O(blocks) HANG and the commit-under-writeback / reuse-torn-write
  btree checksum races are fixed (the ERTB/block-reuse angle is addressed at
  the `briefs_get_zero_block` reuse chokepoint); 299 now passes ~70-80% of runs
  (was 0% hard HANG/FAIL). Residual = the deferred **problem-2** lockless
  read-vs-writeback btree race (`trust_verified=false` reader racing
  writeback's mid-edit btree insert, which takes `extent_lock` but not
  `inode_lock`; ~169 `btree: node N checksum mismatch`/boot, transient `-EIO`,
  occasional timeout). Needs read-path serialization / CoW btree nodes /
  per-buffer seqlock. See `briefs-dio-stress-cluster-250-252-299-triage`.
- `generic/311` — pre-existing baseline flake (dm-flakey + fsync timing);
  reproduces on a known-good baseline.
- `generic/492` — harness gap: kernel `FS_IOC_GET/SETFSLABEL` works (label
  set + read back); only the two `blkid` lines fail (libblkid has no BrieFS
  probe — a `briefs-utils` task, not a kernel bug). **Skipped since `991faac`.**
- `generic/563` — cgroup writeback accounting mismatch; expected after
  `SB_I_CGROUPWB` was disabled on 6.12 (`9385fc8`, iput CVE-2026-31703
  workaround).

**Skipped (2, in the skip list — not counted as fail):** `475 492`. `475` is
the dm-error crash-replay residual, **accepted ~50%** since the `ca4478b`-era
triage (C2 block-layer-capped; the Phase-2 pin design `5524267` fixed the
pdflush-drift mode that the old "needs a journal-format change" framing
targeted); `492` is the libblkid-probe gap (kernel label ioctls work; a
`util-linux` task, skipped `991faac`). `720` was in this list at the time of
the 0818 run but is **no longer skipped** — see the post-run updates below.

> **Post-run updates (2026-08-18):**
> - `88de097` — `068`/`074`/`464`/`476` re-verified at full-suite scale (4/4
>   PASS, 0 hang, `TIMEOUT_SECS=900`; archive `run-20260818-162028`) and
>   **un-skipped**. Fixes: 068 (FIFREEZE/FITHAW `c274292`), 074 (mmap
>   writeback leak `fb649e8` + truncate_setsize AB-BA), 464 (trie_iter_grow
>   double-free `4ef6ccb`), 476 (all-writes fsstress).
> - `c4082e7` — `051`/`461`/`753` re-verified (3× each, 9/9 PASS, 0 hang) and
>   **un-skipped**. BrieFS now has shutdown support (`.shutdown` super_op from
>   `29f4572`), which is why 051/461 pass; 753's dm-error WARN was fixed
>   `73a0d1d`. Default skip list dropped to `475 720`.
> - `991faac` — `492` **skipped** (libblkid has no BrieFS probe; kernel label
>   ioctls work). Default skip list became `475 492 720`.
> - `3666a3e` (2026-08-19) — `720` **un-skipped and PASSING** (~58 s): the
>   O(E²) punch-alternating setup cost was fixed by threading the exact
>   blocks-freed count out of `briefs_btree_delete_range` (O(1) `i_blocks`
>   update), and the btree internal-split separator read-after-memset bug that
>   then surfaced was fixed in the same commit. **Default skip list is now
>   `generic/475 generic/492`** (`run-suite.sh`).
> - `1005b2e` + `0fd1448` (2026-08-19) — `299` **partially fixed** (see the
>   `299` entry above and "Current state" at the top): btree modify-path
>   `wait_on_buffer` + `briefs_get_zero_block` reuse wait + fallocate
>   O(extents)/halving. 299: 0% → ~70-80% pass; residual problem-2 deferred.

**Moved into PASS since 2026-08-13** (key ones): `089` (`8780683`), `536`
(`33e4019`), `250`/`252` (`19ba019`), `050`, `274` (was the split regression,
fixed `141be8c`), `346`, `599`, `623`/`730` (`29f4572`); plus the fallocate
zero_range/collapse/insert_range suite (`e2f023c`), the file-range exchange
suite (`56d1aa7` + Tier-3 `7fa96e5`), and `050`/`274`/`346`/`599`.
`250`/`252`/`274`/`346`/`599`/`623`/`730` moved out of the FAIL set into PASS;
net FAIL count dropped 10→4.

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

## Not-run tests (332 in the 2026-08-18 run; table below from 2026-07-06)

The 2026-08-18 per-test run reported **332 not-run** (down from 397 in
2026-08-13, as the fallocate / exchange-range / ACL / iomap-DIO suites moved
into PASS). The detailed reason-grouped table below was built from the
2026-07-06 `./check -g auto` run's `.notrun` artifacts (the per-test runner
archives do not record the reason text), so the table's per-row counts are
from that older run and are representative of the reason *taxonomy* rather
than the exact 2026-08-18 counts. Shifts since that table was built: the 14
**ACL** tests moved to PASS (`a113c76` — that row is now historical), the
iomap migration moved ~23 DIO/bmap/swapfile tests into PASS, and the
fallocate + exchange-range work moved ~50 more into PASS. Use the 2026-08-18
PASS / NOT RUN lists above for exact membership; use this table for "why a
test is not run".

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
| xfs_io exchangerange not supported ~~(now supported `56d1aa7`)~~ | 16 | 709 710 712 714 716 717 718 719 ~~720~~ 722 723 724 725 726 727 752 — exchange-range landed; 720 now PASSING (`3666a3e`, un-skipped), row kept for history |
| ACLs not supported ~~(now PASSING via `a113c76`)~~          | ~~14~~ | ~~026 053 077 099 105 237 307 318 319 375 444 449 529 697~~ — all 14 now PASS in 2026-08-13 (POSIX ACLs landed); row kept for history |
| xfs_io fcollapse failed ~~(COLLAPSE_RANGE landed `e2f023c`)~~ | 12 | 012 016 017 021 022 031 072 497 499 503 641 687 — collapse_range now implemented; most now PASS, row kept for history |
| fsverity utility required (no fsverity)                   | 11 | 572 573 574 575 576 577 579 624 625 692 788 |
| xfs_io fzero failed ~~(ZERO_RANGE landed `e2f023c`)~~ | 11 | 008 009 033 042 096 456 469 511 610 685 758 — zero_range now implemented; most now PASS, row kept for history |
| Dedupe not supported (test)                               | 9 | 121 122 136 158 160 182 304 408 516 |
| xfs_io finsert failed ~~(INSERT_RANGE landed `e2f023c`)~~ | 9 | 058 060 061 063 064 404 485 686 735 — insert_range now implemented; most now PASS, row kept for history |
| Dedupe not supported (scratch)                            | 7 | 162 163 374 493 517 630 674 |
| idmapped mounts not supported                             | 6 | 644 645 656 689 698 699 |
| FITRIM not supported ~~(now PASS)~~ | 5 | 038 251 260 288 500 — all five now PASS, row kept for history |
| O_TMPFILE not supported                                   | 4 | 004 389 509 531 |
| DAX not supported                                         | 3 | 413 462 605 606 608 |
| log state probing not supported ~~(052 now PASS)~~ | 3 | ~~052~~ 054 055 — 052 now PASS (shutdown support), row kept for history |
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
| xfs_io label failed ~~(label ioctl landed)~~ | 1 | ~~492~~ — 492 now runs and fails on blkid (libblkid no BrieFS probe); SKIPPED since `991faac`, row kept for history |
| cross-device copy_file_range not supported                  | 1 | 565 |
| requires delayed allocation buffered writes                | 1 | 614 |
| xfs_io swapext not supported ~~(now PASS)~~ | 1 | ~~711~~ — 711 now PASS, row kept for history |
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

## Passing tests (456)

From the 2026-08-30 run archive (`run-20260830-161504-kernel.txt`, the
`ddcb7ef` fix content, master). Delta vs the 2026-08-18 list: `299` (partial
fix holding) and `720` (un-skipped) moved in; `538` (DIO flake) moved out.

```
001 002 003 004 005 006 007 008
009 011 012 013 014 015 016 017
020 021 022 023 024 025 026 027
028 029 030 031 032 033 034 035
036 037 038 039 040 041 042 043
044 045 046 047 048 049 050 051
052 053 056 057 058 059 060 061
062 063 064 065 066 067 068 069
070 071 072 073 074 075 076 077
078 079 080 081 083 084 085 086
087 088 089 090 091 092 093 094
095 096 097 098 099 100 101 102
103 104 105 106 107 108 109 112
113 114 117 120 123 124 125 126
127 128 129 130 131 132 133 135
141 169 177 184 192 193 198 204
207 208 209 210 211 212 213 214
215 221 224 225 226 228 236 237
239 240 245 246 247 248 249 250
251 252 255 256 257 258 260 263
269 273 274 275 277 285 286 288
294 299 300 306 307 308 309 310
312 313 314 315 316 317 318 319
320 321 322 323 325 335 336 337
338 339 340 341 342 343 344 345
346 347 348 349 350 351 354 355
360 361 362 363 364 366 371 375
376 377 378 388 389 390 391 392
393 394 401 402 403 404 405 406
409 410 411 412 416 417 418 420
422 423 424 426 427 428 430 431
432 433 434 436 437 438 439 441
442 443 444 445 446 448 449 450
451 452 453 454 456 459 460 461
464 465 466 467 468 469 471 472
473 474 476 477 478 479 480 481
483 484 485 486 488 489 490 491
494 495 496 497 498 499 500 502
503 504 505 507 508 509 510 511
512 519 520 521 522 523 524 525
526 527 528 529 530 531 532 533
534 535 536 537 539 545 547 551
552 553 554 555 557 558 564 567
568 569 571 585 586 589 590 591
597 598 599 604 609 610 611 615
616 617 618 619 620 622 623 626
627 629 631 632 633 634 635 636
637 638 639 640 642 643 646 647
650 676 677 678 679 680 683 684
685 686 687 688 690 694 695 696
697 701 703 704 705 706 707 708
711 712 713 715 718 719 720 722
723 724 725 728 729 730 731 732
735 736 737 738 740 741 742 743
747 748 749 750 751 752 753 754
755 756 758 759 760 761 763 764
771 779 782 784 785 789 790 792
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
| 257 637 676                   | —        | readdir seek/resume (simple_offset) — 676 later regressed under Phase 2 (full-suite-only flush HANG) and was re-fixed by `ddcb7ef` |
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
| 475                           | —        | dm-error crash-replay: passed this run; since triaged to **accepted ~50%** (C2 block-layer-capped; drift mode fixed by `5524267`) |
| 048                           | 62167fa  | sync+shutdown file size bug (inode dirty on i_size growth + inode-block RMW lock) |
| 737                           | 8f4a27b  | O_DIRECT+shutdown file lost (directory sync durability + journal ring back-pressure) |
| 475 drift mode                | 5524267  | Phase-2 pin: deferred metadata held get_bh + not-BH_Dirty until checkpoint (pdflush can't write drifted metadata) |
| 19-test aliasing cluster      | ca4478b  | untrack owned bh before clean_bdev_aliases on free (pin-survives-free wrote stale TRNP/BTRE/ERTB onto reused data blocks) |
| 753 unkillable wedge          | ca4478b  | trie sibling-walk + xattr-chain caps (BRIEFS_TRIE_SIBLING_MAX / BRIEFS_XATTR_MAX_CHAIN = 1024) |
| 676                           | ddcb7ef  | two-pass flush_owned: drop all pins at commit before the sync wait + sb_bread re-resolve (full-suite-only deadlock) |

> Open BrieFS code bugs as of the 2026-07-06 run (status updated to 2026-08-18):
> - `299` — btree checksum mismatch under stress; **partially fixed**
>   (`1005b2e` + `0fd1448`, 2026-08-19): the fallocate O(blocks) HANG and the
>   commit-under-writeback / reuse-torn-write races are fixed; 299 passes
>   ~70-80% of runs. Residual = deferred **problem-2** lockless read-vs-
>   writeback btree race; see
>   `briefs-dio-stress-cluster-250-252-299-triage`.
> - `341`, `510`, `771` — replay duplicate directory entries: **now PASS** in
>   2026-08-13 (idempotency/replay work resolved them).
> - `547` — fsstress metadata mismatch: **now PASS** in 2026-08-13 (crash-replay
>   durability work; still watched as flaky).
> - `599` — VFS `cleanup_mnt` WARN after shutdown: **now PASS** (`bce12e6`).
> - `623` — fsync after shutdown does not return `EIO`: **now PASS** (`29f4572`).
> - `730` — read after device delete missing `EIO`: **now PASS** (`29f4572`).
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
