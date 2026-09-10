# FUSE 63-HANG forensics — root cause of the dominant family (2026-09-08)

Evidence capture: `fuse-hang-forensics-20260908.txt` (repro logs, fsync totals,
per-test scope CPU, SIGQUIT goroutine dump).
Run being explained: `run-20260907-225019-fuse.txt` (188/110/430/2/63).

## TL;DR

The 63 HANGs are **not** one wedge family.  They split three ways:

1. **~48 tests: throughput blowout, not a deadlock.**  The bridge runs a full
   journal checkpoint on **every metadata op** — ~6 device fsyncs and ~31 ms
   per file create.  Metadata-heavy tests simply exceed the 300 s timeout
   with no test output yet.  Proven on generic/006 (below); CPU
   classification fits the whole group.
2. **4 tests: genuine livelock** — 091, 617, 751, 760 each burned a full core
   for the entire 300 s (scope CPU ≈ 4 min 57 s).  Separate root cause, not
   yet chased (test-side CPU burn; the daemon lives outside the test scope).
3. **11 tests unclassified** (systemd never reported scope CPU): 014 069 103
   108 133 249 449 511 524 563 747.

## The per-op checkpoint mechanism (family 1)

### Per-create cost, measured

Standalone repro on a fresh mkfs of /dev/vdb1 (mount via `mount -t
fuse.briefs`, daemon straced for fsync/fdatasync):

- `permname -c 4 -l 6 -p 1` — 4096 fresh creates in **125 s** wall,
  **24,406 device fsyncs** (≈6 fsyncs/create, avg 4.5 ms, max 33 ms),
  **~88 % of wall time inside fsync**.
- Raw `pwrite+fsync` on the same device: 9–10 ms — no individual fsync is
  pathological; it is the *count* per op.
- generic/006 = two such phases (8192 creates) + bridge check-startup
  (scratch mkfs/mount/unmount cycle + TEST_DEV mount) → comfortably over
  the 300 s kill, log frozen at the first "single thread permname" heading.
  Exactly the full-run signature (killed at 300 s, zero output past ~252 B).
- The daemon's writeback never stalls (io sampler: write_bytes grows
  steadily) and every fsync completes — nothing is wedged; the test is
  just ~60–100× slower than the kernel (kernel 006 ≈ 2 s).

### Why every op checkpoints: a per-op Sync colliding with the sync-path back-pressure

Every create calls `Journal.Sync(false)`
(`fuse/dir_ops.go:398`, `createNamedInode` → "commit before flush"),
then `flushCache()`.  `syncLocked` (`briefs/journal_write.go:278`) opens with:

```go
if !j.inCheckpoint && j.writePos == j.sb.JournalLogStart {
    // back-pressure: force a full checkpoint
```

A checkpoint sets `JournalLogStart = writePos` (`journal_write.go:439`), so on
the *next* op's Sync the condition `writePos == JournalLogStart` is **always
true** — the post-checkpoint *empty-ring* state.  Every op therefore takes
the full path: metadata fsync + allocator sync + checkpoint-block write +
checkpoint-block fsync + superblock write + superblock fsync (plus the
inner commit's superblock sync) ≈ 6 fsyncs.

**Correction of the first reading (which called this a misport):** the
condition itself is a faithful port — of the kernel's *sync-path*
back-pressure (`journal.c:2476`, checked before writing the pending block),
not of the write-path one (`journal.c:591-618`, which excludes the empty
state, comment verbatim):

> Having just advanced write_pos, write_pos == journal_log_start holds ONLY
> when the ring has wrapped completely full: in the empty state (right after
> a checkpoint, or at fresh mount) journal_log_start == write_pos but
> next_block(write_pos) != write_pos, so the condition above is never taken
> there.

The kernel's sync-path check fires in the empty state too — and that is
harmless there *by design*: the kernel only syncs the journal on explicit
fsync/sync_fs/DIRSYNC.  `dir.c:25` spells the constraint out — driving a
flush on every create/unlink "makes BrieFS far too slow for workloads like
generic/417" — so per-create syncs were rejected at the call-site level, and
the sync-path back-pressure stays a rare event.  The bridge ported the
journal condition faithfully but then *called it per metadata op* (~10
`Sync(false)` sites: dir_ops, file_ops, extent_ops, fileattr), promoting the
kernel's rare path into the common case.  The divergence is the call
frequency, not the condition.

There is also a real structural hazard the fix must respect: on disk, the
post-checkpoint empty ring and a fully wrapped full ring are
**indistinguishable** (`logStart == logEnd == writePos` in both); only
history — ring positions advanced since the last checkpoint — discriminates,
because a wrapped ring has advanced ≥ the usable ring size.  And when the
sync-path back-pressure fires on a genuinely full ring with a pending block,
the pending block sits at `writePos == logStart` = the oldest LIVE block:
retiring first and writing the pending block afterward (into the retired
position) is required, or the checkpoint's inner sync clobbers records whose
metadata has not been flushed yet.

### Corroboration

- Full-run scope CPU for every family-1 test is 1–33 s over a 300 s wall
  (I/O-bound), vs 4 min 57 s for the livelocks.
- During the repro-batch live capture of 006 (misread at the time as a
  wedge): the daemon's SIGQUIT dump shows a single CREATE handler in
  `fsync(/dev/vdb1)` inside `checkpointLocked`, all other handlers idle in
  `readRequest` — with 88 % of daemon time spent in fsync, a random dump
  *should* land mid-fsync.  No goroutine was stuck on a mutex.
- dmesg across the full run: zero hung-task reports — nothing ever sat
  blocked in one uninterruptible op for 120 s.

## Misdiagnosis notes (retraction of the "wedge" reading)

The 09-07 live capture of 006 was read as a daemon wedge.  Three traps:

- `ps` **etimes is process age, not time-in-state**; "permname workers D-state
  84 s" were 84-second-old processes seen *momentarily* in D.
- The watcher (D-state ≥ 120 s trigger) never fired during the whole
  episode — the workers were cycling through D on multi-second ops.
- A Go daemon that spends ~88 % of wall time inside fsync will almost
  always show its handler "stuck in fsync" in a one-shot dump.

Real wedge indicators remain: a handler goroutine blocked on a lock with no
holder, or a client op blocked continuously for minutes (hung-task in dmesg,
`wchan` stable).  The 013 whiteout shard deadlock was a real wedge of this
kind; 006 was not.

## Fix options for family 1

- **A. Gate the sync-path back-pressure on the empty ring (minimal,
  kernel-faithful):** keep the per-op Sync but stop the empty-state
  checkpoint.  Because the empty and full-ring states are on-disk identical
  (above), the gate needs history: a `blocksSinceCheckpoint` counter of ring
  positions advanced since the last checkpoint (reset wherever logStart is
  reset).  The full-ring firing must retire *before* writing the pending
  block (which lands in the retired position).  Two follow-on obligations
  the old every-op checkpoint silently covered: the per-op Sync must write
  the allocator bitmap pools (the kernel's sync_blockdev does this via dirty
  bitmap buffers; without it a crashed image has stale on-disk bitmaps only
  replay can reconcile), and `checkpointLocked` must not clear the pending
  state when retiring without flushing.  Cuts ~6 fsyncs/op to ~2.  006 drops
  to roughly half its current time — still likely over 300 s; the
  fsstress-scale HANGs need more.  CAUTION: this activates the bridge's
  ring-wrap path, which the current checkpoint-every-op behavior never
  exercises; it needs the same concurrency care the kernel path got
  (cf. journal.c:591 lock-order fix, generic/411).
- **B. Stop syncing the journal per metadata op (the real fix):** commit
  records lazily like the kernel — Sync only on fsync/O_SYNC/unmount, plus
  checkpoint at ring-full back-pressure and unmount.  Correctness is
  preserved (crash-consistency tests sync explicitly before simulating a
  crash; the unmount checkpoint is unconditional).  This is a semantic
  change to the bridge's durability contract (an unsynced create may vanish
  after a simulated crash — kernel-equivalent) and needs a full-suite
  re-run to validate.
- **C. Palliative:** raise per-test timeouts like 013's 1200 s — does not
  scale to 48 tests.

A+B together are the kernel's actual design (back-pressure exists precisely
because the kernel does *not* checkpoint per op).

### Fix A: landed and validated 2026-09-08

Implemented in briefs-utils (bu-refactor-1): `blocksSinceCheckpoint` gate,
retire-first full-ring handling (`checkpointLocked(flushPending=false)` —
the pending block sits at `writePos == logStart`, the oldest live block),
per-sync allocator bitmap writes (the kernel's sync_blockdev flushes dirty
bitmap buffers; the old code did this as a per-op-checkpoint side effect —
without it, crashed images left stale on-disk bitmaps, caught by
TestCrashRecovery), and `checkpointLocked` no longer clears `dirty`
unconditionally (with flushPending=false that DROPPED the pending records,
caught by the new TestSyncBackPressureRetiresFullRing).

Validation on the 64G VM image (/dev/vdb1, journal 64 blocks):

- **A/B, unstrace'd, same conditions, 200 shell creates through the
  bridge: OLD (747ef76) 12.44 s vs NEW 7.58 s — 39 % faster (62 → 38 ms
  per create).**  The original "125 s / 4096 creates ≈ 30 ms/op, 88 % in
  fsync" attribution was strace-inflated: unstrace'd, fsyncs average
  ~194 µs, and the dominant per-op cost is NOT fsync — the daemon's
  /dev/fuse reads show ~27 ms gaps waiting for the *client's* next
  request with daemon CPU near zero.  The residual 38 ms/op (kernel: ~0.5
  ms) is the FUSE round-trip / per-op-Sync cost — fix B territory.
- Ring-full back-pressure now fires once per genuine wrap: with ~2 journal
  blocks per create (two per-op Syncs), 200 creates = 6 wraps = 6
  back-pressure checkpoints + 1 unmount (checkpoint seq 8; fsck clean).
- **The unmount checkpoint works, but `umount` returns BEFORE the daemon's
  post-DESTROY checkpoint completes.**  fsck immediately after umount sees
  the un-retired tail (looked like a missing checkpoint; strace of the
  daemon's exit tail shows the checkpoint-block + superblock pwrites and a
  clean exit_group).  The fuse-briefs-umount wrapper waits on the pidfile
  for exactly this; plain `umount` does not — test scripts must wait for
  the daemon pid to exit before fsck.

## Fix B: landed and validated 2026-09-09

Implemented in briefs-utils (bu-refactor-1).  The bridge no longer syncs the
journal per metadata op — kernel parity (dir.c:25: the kernel syncs only on
fsync/sync_fs/DIRSYNC/O_SYNC, plus unmount and the ring-full back-pressure
checkpoint).

Architecture (the bridge's analog of kernel buffer heads):

- **Per-op block cache** (cacheBegin/loadBlock/saveBlock) gives one shared
  working buffer per block within an op — the kernel gets this for free
  from sb_bread/sb_getblk; without it two roles landing on the same packed
  trie page (parent + last sibling) clobbered each other.
- **Deferred-metadata map** (`BrieFS.dirtyBlocks`): a successful op ends
  with `mergeCache` — dirty blocks move into daemon memory with NO I/O.
  Readers stay coherent between merge and drain through a dirty-view hook
  on `BlockDevice.ReadBlock`.  This is the bridge's write-back cache: the
  kernel's pinned, not-yet-dirty buffer heads.
- **Drain at the commit point**: the journal's syncLocked calls the
  `MetaSyncer` hook AFTER persisting log_end (the commit point) and BEFORE
  the allocator bitmap persist — so the records justifying a block are
  always durable before the block reaches the device page cache
  (kernel parity: briefs_journal_flush_owned), and a completed sync leaves
  the on-disk bitmaps converged with the committed records.
- **Deferred frees** — the generic/040/041 free-vs-commit class under fix B:
  `trieReleasePage` and `freeInodeData` used to free blocks in memory
  BEFORE their freeing records (JRN_TRIE_ALLOC op=1 / JRN_EXTENT_FREE)
  committed; previously the per-op op-end sync retroactively gated this.
  A freed block could be reallocated for data whose fresh content the
  drain then overwrote (caught by TestCrashSlotReuseReplay: a reused trie
  page clobbered a new file's data — live readback was already corrupt
  BEFORE the crash).  Now `deferBlockFree` queues the free in `pendingFrees`
  and drops the block's deferred content; `SyncMeta` applies the frees
  after the commit point.  The frees apply in SyncMeta — NOT in
  SyncAllocators — because the pre-commit ring-full back-pressure path
  (`checkpointLocked(flushPending=false)` inside syncLocked) also calls
  SyncAllocators and would publish bitmap frees ahead of their uncommitted
  records, reintroducing the same corruption in the other direction.
  Freed-after-own-sync paths (commitExtentChange, xattr chain rewrite)
  use `freeBlockNow`, which also drops any stale deferred copy.
- **Crash model** (kill -9: process memory lost, OS page cache survives):
  an unsynced op simply never happened — the same semantics a buffered op
  has against the kernel.  A completed journal sync leaves the image fully
  converged (fsck clean on the raw crashed image, no replay needed).

Unit tests: full `go test ./...` green, `-race` green.  Three tests updated
for the new contract (crash test syncs before the kill; trie-cap test
flushes deferred metadata before corrupting on-disk state; fstrim/hardlink
tests sync before asserting free counts).

Validation on the 64G VM image (/dev/vdb1, journal 64 blocks), same
conditions as the fix A A/B:

- **200 shell creates through the bridge: 0.06 s (~0.3 ms/op), second
  batch 0.11 s** — fix A left 38 ms/op; the per-op Sync was the whole
  residual.  For scale, the kernel module does ~0.5 ms/op.
- **Crash after a synced workload** (kill -9 of the real daemon pid, no
  unmount): raw-image fsck CLEAN, 14 committed records converged; fresh
  mount reads every synced file back.
- **Crash with unsynced creates pending**: fsck exit 0; remount shows the
  synced files intact and the unsynced creates gone (kernel-equivalent
  buffered semantics).
- **generic/006: PASS in 6 s** (was ~311 s against the 300 s timeout —
  pure per-op-Sync blowout; a 50× cut, now far under the kernel-adjacent
  scale).

Gotchas found during validation:

- The mount helper's pidfile can hold a pid that is not the serving daemon
  (a race with the systemd-run unit's fork); crash tests must take the pid
  from `ps` (`[f]use.briefs -i <dev> -m <mnt>`), or the "crash" silently
  degrades to a clean unmount.
- systemd-run refuses to append the daemon's stdout to a log file owned by
  a non-root user ("Failed at step STDOUT"): a stale log from a user
  earlier mount attempt wedges every later root mount with a bare
  "timed out waiting for <mnt>".  rm the stale /tmp log to recover.
- **Expected fsck WARNING after a post-replay crash** (not a fix B bug):
  `MarkCleanAfterReplay` bumps checkpoint_seq in the superblock without
  writing a checkpoint block — kernel-faithful (journal.c:2177 does the
  same; the checkpoint path's block-before-superblock order at
  journal.c:838 only covers real checkpoints).  So an image crashed between
  a replay-bearing mount and the first checkpoint shows "checkpoint seq
  mismatch: payload=N, superblock=N+1" (fsck exit 0).  The window exists
  in the kernel too but was invisible in the bridge because the old
  per-op checkpoint closed it after one op.  A clean unmount checkpoint
  clears it (verified: final fsck fully clean, no warnings).

## The 63 re-run under fix B (2026-09-09/10): family 1 was NOT the whole story

All 63 re-run against the fix-B bridge (run-20260909-193211-fuse.txt,
fsck validation on):

|            | 2026-09-07 (pre fix A+B) | 2026-09-09 (fix B) |
|------------|------:|------:|
| HANG       | 63 | 44 |
| PASS       |  0 | 14 |
| FAIL       |  0 |  4 |
| NOT RUN    |  0 |  1 |
| FSCK WARN  |  — |  0 |

Cleared: 006 100 103 249 310 339 471 488 558 617 676 707 736 751 (PASS);
007 500 585 589 (now FAIL — triageable, see below); 108 (NOT RUN).

Still hanging (44): 014 027 069 074 077 091 095 102 113 114 127 129 132
133 224 226 247 273 274 275 311 320 347 366 371 410 416 418 449 460 465
476 511 524 563 590 610 627 642 747 748 750 760 761.

**The "~48 throughput blowouts" attribution is REFUTED as the dominant
family.**  Removing the per-op syncs cleared only 19 of 63.  What the old
scope-CPU analysis classified as one I/O-bound family splits into at
least: genuine per-op-Sync blowouts (cleared), and a still-hanging
majority whose blocker is something else.  Leading suspect for the
fsx/fsstress-heavy members: the WRITE path still syncs per extent change —
`commitExtentChange` runs `journal.Sync(false)` + two Fdatasyncs on every
extent-list mutation (kernel parity argument does not apply: the kernel
does NOT sync the journal on buffered writes; it only dirties pages and
commits records lazily).  A 1M-op buffered fsx (522-class) pays a full
journal commit + device flush per write.  Second suspect for the
mmap/fsx members (074 127 …): the known silent msync/flush deadlock
family (see the mmap-writeback-deadlock memory).  Next forensics step:
scope-CPU the 44 again — idle (deadlock/round-trip) vs full-core
(livelock) vs I/O-bound (per-write syncs) now separates cleanly.

The old sub-families did not survive contact either:

- **Livelock family (4)**: 617 and 751 now PASS — the "full-core burn"
  was throughput-coupled or flake, not an independent livelock.  091 and
  760 still hang.
- **Unclassified 11**: 100 103 249 108 cleared; 014 069 449 511 524 747
  still hang (with 133, 563 also hanging on re-check).
- 311 hangs on the kernel baseline too — not bridge-specific.

**The 4 new FAILs** (first time these tests ever completed under the
bridge — all new surfaces, none triaged yet):

- 007 (dirstress create/remove/lookup): `creat: No space left on device`
  after far more iterations than the reference — real-bug candidate:
  either frees deferred into `pendingFrees` never reclaim in the
  in-memory allocator without a sync (reclaim starvation → spurious
  ENOSPC) or unlinks failing so files accumulate.  Needs triage; if the
  former, the fix is to free in-memory at op time and keep the deferred
  side for the on-disk bitmap only.
- 500: `fstrim: the discard operation is not supported` — the bridge's
  FITRIM is not wired through the FUSE ioctl path (harness/feature gap;
  the unit-level fstrimOp works).
- 585: `rm: cannot remove ...: Directory not empty` — real-bug candidate
  (stale dirent left visible).
- 589: mount.fuse.briefs usage/propagation-flag failures — the mount
  helper does not handle the test's bind/propagation forms (harness gap).

0 FSCK WARN across every test that completed: fix B's deferred write-back
left no on-disk corruption fsck can detect under real workloads.

## Remaining follow-ups

1. Triage the two real-bug-candidate FAILs (007 ENOSPC / deferred-free
   reclaim starvation; 585 dir-not-empty) — 007 reproduces within the
   timeout now.
2. Scope-CPU the 44 still-hanging tests to split: per-write sync cost
   (fsx/fsstress members; next fix is deferring commitExtentChange's
   journal sync like fix B did for metadata ops) vs mmap-deadlock family
   (074 127 …) vs true livelock (091 760).
3. generic/475 dm-error (separate, pre-existing: fsstress D-state ~5 h).
4. After the fix lands: re-run the 63 and re-diff against kernel baseline.