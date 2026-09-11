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

- 007 (dirstress create/remove/lookup): `creat: No space left on device` —
  FIXED 2026-09-09 (briefs-utils, see below).  Starvation hypothesis
  REFUTED by measurement: the failing VM run consumed only 130 of 16.5M
  blocks (df sampling) while emitting 1768 ENOSPCs — the frees were not
  starved; the ENOSPC was spurious.
- 500: `fstrim: the discard operation is not supported` — the bridge's
  FITRIM is not wired through the FUSE ioctl path (harness/feature gap;
  the unit-level fstrimOp works).
- 585: `rm: cannot remove ...: Directory not empty` — FIXED 2026-09-09
  (briefs-utils, see the 585 section below; FUSE presentation-layer bug,
  not a dirent leak).
- 589: mount.fuse.briefs usage/propagation-flag failures — the mount
  helper does not handle the test's bind/propagation forms (harness gap).

0 FSCK WARN across every test that completed: fix B's deferred write-back
left no on-disk corruption fsck can detect under real workloads.

## generic/007 triage (2026-09-09) — spurious ENOSPC from name-heap dead space

007 was HANG before fix B, so this surface had never been reached.  The
incoming starvation hypothesis (deferred frees in `pendingFrees` never
reclaiming in the in-memory allocator without a sync) was **refuted by
measurement**: a VM repro (nametest -l namelist -s 1 -i 100000 -z on a
16.5M-block image, df sampled every 5s) hit 1768 ENOSPCs in 15s while
consuming only 130 blocks — the allocator was nowhere near exhausted;
the ENOSPC was spurious.

**Root cause: the bridge is a pre-generic/089 port.**  It was missing
both halves of kernel commit 8780683:

1. **Lazy name-heap compaction** (kernel trie_page.c:427).  The per-trie-
   page name heap is a bump allocator whose `FreeNameOff` high-water mark
   never shrinks; freeing a node orphans its name bytes as dead space
   (capacity 4096-2324 = 1772 bytes).  A nameless INTERM node (created by
   a middle-byte `trieFindOrCreateChild` with nameLen 0) can land on a
   page already full of dead space — the pool scan's has-name-heap check
   passes trivially for size 0.  When a shorter prefix name is later
   inserted ("nametest.1" over "nametest.12"), the re-leafing
   `trieStoreName` needs fresh heap space on that page → spurious ENOSPC
   with the heap mostly dead.  Intermittent because it needs the longer
   name created before its prefix.
2. **Store-name-before-LEAF-commit ordering** at all 3 insert sites
   (kernel trie.c:587/:662/:720): a failed `trieStoreName` after the slot
   is already marked LEAF leaves a nameless leaf (unfindable by lookup →
   orphaned inode).

**Fix (briefs-utils commit 5028653, bu-refactor-1):** ported both halves
into `fuse/trie_mutate.go` — `triePageCompactNames` (kernel-faithful
layout compaction, aborts untouched on corrupt-heap anomalies) called
from `triePageAllocName`'s would-ENOSPC path, with the compacted layout
persisted even when the retry still fails; and the 3-site reordering
(store name first, then mark LEAF).  Bridge-only hazard handled at each
site: `trieStoreName` mutates the shared op-cache page buffer, so a
`*briefs.TrieSlot` parsed before the store is stale — every reordered
site re-parses the slot before setting LEAF bits (the kernel edits page
memory in place and has no such staleness).

**Proof:** new `fuse/trie_compact_test.go` — `TestTriePageCompactNames`
(heap math + live-name readability + reclaimed-space allocation) and
`TestTrieNameHeapChurnNoSpuriousENOSPC`, a 50k-op create/remove churn
over prefix-family names with randomized (fixed-seed) pick order that
fails on the old code at iteration 776 with exactly the spurious ENOSPC
and passes on the fix.  Full `go test ./...` green, `-race` green.

**VM repro A/B:** identical script — old daemon: 1768 ENOSPCs, 130
blocks consumed; fixed daemon: **0 ENOSPC in 17s, counts match the
007.out reference exactly** (creates 18736 OK / 18802 EEXIST, removes
18675 OK, lookups 12000 OK, cleanup 61 removes).  Harness re-run with
FSCK_ENABLED=1: **generic/007 PASS in 18s, fsck clean** (archive
run-20260910-013335-fuse.txt).

Secondary finding while triaging (follow-up, NOT 007's cause): ring-full
back-pressure in `writeRecordLocked` sets `j.dirty = false` before
`checkpointLocked(true)`, so the checkpoint's flushPending-inner-sync
never runs → **SyncMeta never drains `pendingFrees` on ring wrap** —
deferred frees only reclaim on explicit fsync/sync/unmount.  Bounded by
trie pages (~130 blocks here) but a latent starvation class for
delete-then-write-without-sync workloads.  Fix shape: free in the
in-memory allocator at op time and defer only the on-disk bitmap publish
— with frees kept OUT of SyncAllocators (pre-commit back-pressure calls
it; publishing ahead of uncommitted records is the 040/041 corruption
class).

## generic/585 triage (2026-09-09) — whiteouts presented as directories

The `rm: cannot remove '...tmp.MVVxkOTAsB': Directory not empty` was NOT
fsstress (fsstress had already passed) — it was `_require_renameat2`'s
cleanup (common/renameat2): `renameat2 -w foo bar` then `rm -rf $dir`.
The whiteout left at `foo` could never be removed, so the rmdir failed
ENOTEMPTY forever.  The incoming hypothesis (stale dirent left visible)
was **refuted**: the leftover is a real, fsck-consistent on-disk whiteout
entry; the bridge-side state after `renameWhiteout` is correct (proven by
unit test: trie ftype 2, on-disk inode mode 0020000, nlink 1, slot dump
via dd confirmed).  The failure is entirely in the FUSE presentation
layer, by two compounding bugs:

1. **Lookup ftype type collision** (fuse.go).  The trie's dirent ftype is
   S_IFMT>>12: 4=dir, 8=reg, 10=symlink, **2=chardev** — and RENAME_WHITEOUT
   creates chardevs.  But the Lookup switch tested ftype against
   `briefs.NodeTypeDir` (0x02), which is a *trie node-type bit*, not a
   dirent type — so every ftype-2 entry got StableAttr.Mode = S_IFDIR.
   go-fuse's `setEntryOut` then builds the reply mode as
   `(out.Attr.Mode & 07777) | n.stableAttr.Mode` → 0040000.
2. **Zero-perm whiteout + go-fuse's patcher**.  The whiteout was created
   with bare `S_IFCHR` (no permission bits).  go-fuse's `rawBridge.setAttr`
   (bridge.go:265) rewrites any zero-perm mode in Getattr/Lookup replies
   to `|= 0644` plus `|= 0111` when S_IFDIR — net reply
   **0040755 = `drwxr-xr-x`**, matching the observed `4 directory 1 755`.
   With the whiteout looking like a non-empty directory, `rm -rf` recursed
   into it, unlink failed, rmdir failed ENOTEMPTY — un-removable entry.

**Fix (briefs-utils commit 099bf08, bu-refactor-1)** — three kernel-parity
changes plus
one latent-bug fix, all in the FUSE layer:

1. Lookup: StableAttr.Mode from `childInode.Filemode` unconditionally (the
   on-disk inode is authoritative, like the kernel's lookup which igets and
   derives everything from the inode; go-fuse masks StableAttr.Mode to the
   S_IFMT type bits itself).
2. Readdir: `mode := uint32(ftype) << 12` — the kernel's exact formula
   (dir.c:139 `file_type = (entry_type << 12) & S_IFMT`) — instead of a
   4/8/10 switch that defaulted chardev/blockdev/fifo/socket d_types to
   DT_REG.
3. `renameWhiteout`: create with `S_IFCHR | 0600` (kernel dir.c:1036
   `S_IFCHR | WHITEOUT_MODE` parity), so the mode is never zero even if
   the patcher runs.
4. `NullPermissions: true` in fs.Options — go-fuse's zero-perm patcher is
   off; modes are reported exactly as stored.  This was a latent bug
   beyond 585: any `chmod 000` file stat'ed as 0644 (and 000 dirs as
   0755).

**Proof:** new `fuse/rename_whiteout_test.go` — `TestRenameWhiteoutChardev`
asserts the whiteout's trie ftype is 2, inode mode 0020600, nlink 1, and
that a plain `unlinkInDir` removes it (the exact 585 failure mode).  Full
`go test ./...` green, `-race` green.  Host mount repro with the fixed
daemon: `foo` stats as `character special file ... 600` (`crw-------`,
rdev 0,0 — kernel parity), `bar` as the moved regular file, `rm -rf`
succeeds, unmount + fsck **completely clean** (no leaked blocks, no
leftover entries — also clears the earlier "3 allocated / 2 referenced"
suspicion for this path; that observation came from the fsstress-worked
585 image, not the whiteout path).  Same repro on the VM (/dev/vdb1,
/go/bin/fuse.briefs): identical results, fsck clean.  Harness re-run
with FSCK_ENABLED=1: **generic/585 PASS, fsck clean** (archive
run-20260910-032321-fuse.txt; use a fresh LOG_DIR for re-runs — the
runner skips any test with an existing check-*.log in LOG_DIR).

Note for the kernel-backed side: the 6.12 FUSE client forwards
RENAME_WHITEOUT to the daemon (FUSE_RENAME2, flags; no kernel-side
vfs_whiteout), so this is bridge-only surface — the kernel module creates
whiteouts itself in briefs_rename_whiteout and is unaffected.

## The 44-hang re-run under fix C (2026-09-10): throughput family confirmed

Fix C (utils 42c9b58, no per-write journal sync — see the fix C notes in
`briefs-fuse-hang-forensics-63hangs` memory; fsx A/B 23,832→2,694 device
syncs, 187s→22s per 10K ops) was validated by re-running the 44 fix-B
hangs plus the NOTRUN 108 (45 tests, FSCK_ENABLED=1, fresh LOG_DIR,
`/go/bin/fuse.briefs` built at 42c9b58; archive
`run-20260910-203043-fuse.txt`, 86 minutes):

    27 PASS / 8 FAIL / 1 NOTRUN / 9 HANG / 0 fsck-warn
    (fix B on the same 45: 0 PASS / 4 FAIL / 1 NOTRUN / 44 HANG here,
     counting only these 45 of its 63)

- **35 of the 44 hangs cleared — the per-write sync cost was the dominant
  family**, and the fix-B split of the 44 into "throughput vs livelock vs
  mmap" is settled: 091 and 760 (the fix-B "genuine livelock" pair) both
  PASS under fix C, so that label is refuted — they were throughput-
  coupled like the rest.  311 (which hangs on the kernel module too)
  also passes.
- **Remaining 9 hangs: 069 074 113 410 476 642 747 748 750.**  The
  mmap/flush family is in here (074 and 113; 127 is its sibling but now
  FAILS fast instead of hanging — see below).  The rest need a fresh
  scope-CPU round.
- **New FAIL surfaces unmasked** (they hung under fix B, so the failure
  was never visible):
  - `274/275/371/511` — an ENOSPC cluster.  Leading suspect: fix C
    removed the per-write sync, and with it the per-write SyncMeta that
    applied `pendingFrees` — freed blocks now return to the allocator
    only at the next fsync/sync/umount, so delete-then-write-without-sync
    tests (exactly 275's shape) starve.  This generalizes the known
    ring-wrap finding: the proper fix is the same (free in-memory at op
    time, defer only the on-disk bitmap publish).
  - `127` — fsx with `-R -W` (mapped reads AND writes; the flags enable
    mmap ops, not disable them — easy to misread): after ~5155 ops fsx
    reports `Size error: expected 0x3775c stat 0x14274`.  The file size
    diverged from fsx's model mid-run — a real correctness surface for
    the mmap path, previously masked by the hang.
  - `102/226` — 800MB single write / 16×64MB buffered writes on a 256M
    filesystem; output diffs not yet triaged (226's failure output
    truncates at the write loop, 102's diff needs the .full file).
  - `563` — the known accepted cgroup-writeback FAIL, unchanged.

**Misdiagnosis trap (new):** a daemon that burned CPU *during* a test
looks "stuck" in `ps` — `%CPU` and TIME are cumulative over process
lifetime, not instantaneous.  After interrupting this run's generic/069,
its scratch daemon showed "35.4% CPU, 3:32" with zero clients and was
called a livelock; a SIGQUIT goroutine dump showed every goroutine idle
in `read()` on /dev/fuse, and the CPU total had not grown between two
samples taken 10 minutes apart.  The ~210s CPU (8 threads × ~26s) was
legitimately consumed serving 069's fsstress load before the client
died.  Same class as the `ps etimes` trap above: sample twice, compare
deltas, before calling anything wedged.

**Run-mechanics trap:** the first launch of this re-run failed two ways
at once — the `vagrant ssh -- sudo bash -c '...'` quoting collapsed
("bash: -c: option requires an argument") AND the suite raced the fsx
benchmark that still held /dev/vdb1 at /mnt/testfuse (run-suite's
cleanup only unmounts the xfstests mountpoints), so all 45 tests
instant-MKFS-failed and self-archived as run-20260910-194329 (45×
mkfs_fail; that garbage archive is deleted).  Launch long suites
detached (`sudo setsid nohup ... &` via the `vagrant ssh -- 'bash -s'`
heredoc pattern) and never alongside anything else that holds the test
devices.

## The ENOSPC cluster closed: deferred-free reclaim (2026-09-10)

Triage confirmed the pendingFrees starvation, and widened the cluster:
reading the test bodies (before any re-run) showed **102 and 226 are the
same mechanism, not separate large-write surfaces** — 102 is
`pwrite 800M` + `rm` ×10 on a 1GB fs, 226 is `pwrite 64M` + `rm` ×16 on
a 256M fs (tagged `enospc`); both print xfs_io errors through the output
filter, so a spurious ENOSPC in loop 2+ lands in the diff.  371 (pwrite
80M + rm, 100 iterations ×2 workers, no fsync anywhere) and 511 (256M
pwrite fills the fs, rm, then fsx's writes need the freed blocks) are
likewise delete-then-write.  Only 274 has no delete-then-write at all
(its fills stay on disk and the writes go into preallocated unwritten
space), so its fix C failure was a different, unattributed mechanism
(its .full was purged before triage — see the purge lesson).

Root cause chain, fully verified: deferred frees apply to the in-memory
allocator bitmap only when their journal records commit (the 040/041
reuse-before-commit guard, fix B); fix C removed the per-write sync that
retroactively applied them; and `sync(2)`/`syncfs(2)` cannot reach a
regular FUSE mount — the 6.12 client registers `.sync_fs = fuse_sync_fs`
(inode.c:1205) but sets `fc->sync_fs = 1` only under `if (ctx->is_bdev)`
(inode.c:1738-1742, fuseblk only), and go-fuse has no SYNCFS handler
either, so the kernel would disable it permanently anyway (ENOSYS at
inode.c:755).  generic/275's captured evidence is the mechanism in one
frame: "Post rm space: 0 available, 100% capacity" after `rm` + sync.

**Fix: utils 0e4fc16 — reclaim on demand, NOT free-at-op-time.**  Freeing
at op time was rejected: it would let a freed block be reused before its
freeing record commits (the ca4478b regression class) AND the ring-full
back-pressure path calls SyncAllocators pre-commit, which would publish
frees to the on-disk bitmap ahead of uncommitted records.  Instead the
Allocator gets a `reclaim` hook fired once after a failed
allocBlock/AllocBlocks scan; BrieFS wires it to `reclaimPendingFrees`
(cache.go), which `journal.Sync(false)`s when frees are pending and
reports whether to retry.  The sync commits the freeing records first
and SyncMeta applies the frees after the commit point — a block never
becomes reusable before its previous freeing is durable (the ordering
the kernel gets from its commit thread + ordered data).  Statfs now
reports the pending frees as available (`FreeCountDataPlus`), so df
matches what a write can actually obtain; the kernel gets the same
visibility from kjournald, the divergence is documented at both sites.
This also heals the ring-wrap starvation (writeRecordLocked's
back-pressure path drains pendingFrees only at the next sync) by the
same mechanism.  Regression test
`fuse/reclaim_test.go:TestDeferredFreeReclaimOnENOSPC` (fill, truncate
without sync, second file's write must succeed and crash-replay clean);
with the hook unwired it reproduces generic/275's exact "no space left
on device".

**Validation: all six tests PASS** (FSCK_ENABLED=1, fresh LOG_DIR,
`/go/bin/fuse.briefs` built at 0e4fc16 = fix C + 736a395 + reclaim;
archive `run-20260910-222942-fuse.txt`, ~8 min): 274 275 371 511 102
226 — 6 PASS / 0 FAIL / 0 HANG / 0 fsck-warn.  274's attribution
between the reclaim hook and the 736a395 lazy-drain commit is ambiguous
(both are in the binary; the old failure output no longer exists to
distinguish), but the cluster is closed either way.

## generic/127 closed: the SyncMeta drain window (2026-09-11, utils ffdc719)

Root cause.  SyncMeta swapped the deferred-metadata map empty under
`dirtyMu` and then wrote the blocks to the device page cache with NO shard
lock.  Between the swap-out and a block's write landing, the dirty view no
longer served that block, so a concurrent op on a SIBLING inode in the same
4K inode-table block (file writes serialize on the inode-block shard lock,
which the drain does not hold) based its whole-block read-modify-write on
the stale page-cache copy and regressed every sibling slot.  Two fsx files
created consecutively share one inode-table block, and the flusher
variant's `-f` (constant fsync) drives the drains — the writer/flusher pair
reproduces, single instances do not.

Evidence chain:
- Marker-gated bridge instrumentation (env/file-gated `debugf`, since
  `systemd-run` strips the environment) logged every size-touching op,
  every drain, and diffed every whole-block `setDirtyBlock` store landing
  for a block whose drain was still in flight.
- With the drain window artificially widened 20 ms (debug-gated sleep),
  the pair failed in <1 min and the detector showed the sibling slot
  regressing with a BACKWARDS mtime — an op's own slot always carries a
  fresh timestamp, so a backwards mtime is unambiguously a stale base.
  The regressed numbers matched the fsx error exactly:
  "Size error: expected 0x28342 stat 0x26a1d" = 164674 -> 158237.
- The same widened window with the fix: 200K ops, both fsx A-OK.
- Deterministic pin: `TestSyncMetaDrainKeepsSiblingSlots` parks a drain
  via the `syncMetaPause` test seam between snapshot and writes, then
  writes to the sibling inode; on the old swap-out shape it fails with
  "sibling write during drain regressed a: size 100, want 50".

Fix (utils ffdc719): the drain copies the map, writes the snapshot, and
only afterwards deletes entries no concurrent op re-stored meanwhile
(CAS-delete).  The dirty view covers every block for the whole window; a
concurrently-stored newer copy stays in the map for the next drain.

Result: generic/127 PASS in the harness (run-20260911-005329-fuse.txt) —
the first time it has passed on EITHER implementation (the kernel module
wedges on the 127-class msync/flush deadlock, a different mechanism).

Gotchas collected on the way:
- The pair repro's one-shot `umount` races a still-running previous pair:
  `pkill -9` orphans the FUSE mount, the next `mkfs` refuses ("is
  mounted"), and the script exits WITHOUT writing its status marker while
  the previous script's `wait` completes and writes its own — a later
  launch then reads a stale status/log set and "confirms" a run that never
  happened.  The repro now loop-umounts to completion, guards against
  concurrent instances with `flock` (pgrep self-matches the setsid
  launcher chain), and writes distinct failure markers.
- fsx's `-f` variants use `_flush` filenames: the six instances do NOT
  share files.  The failure needed CONCURRENCY between two files sharing
  one inode-table block, not a shared-file race.
- `setsid` forks when the child is a process-group leader; the transient
  parent's command line matches `pgrep -f repro127`, tripping single-
  instance guards.

## Remaining follow-ups

1. ~~Triage 007~~ DONE.  ~~Triage 585~~ DONE.  ~~ENOSPC cluster 102
   226 274 275 371 511~~ DONE (reclaim, utils 0e4fc16, above).
   ~~Triage 127~~ DONE (drain window, utils ffdc719, above).
2. Scope-CPU the 9 still-hanging tests (069 074 113 410 476 642 747 748
   750) the same way (systemd scope Consumed lines + daemon SIGQUIT
   dumps at the hang point; mind the cumulative-CPU trap).
3. 563 remains the known accepted cgroup-writeback FAIL.
4. generic/475 dm-error (separate, pre-existing: fsstress D-state ~5 h).
5. Benchmark 736a395 (fsx A/B for the residual 1,052 fdatasyncs/10K ops;
   the 45-test re-run archive measured 42c9b58 only).
6. Re-run the full 793-test suite under fix C + 736a395 + 0e4fc16 +
   ffdc719 to get the post-fix totals and re-diff against the kernel
   baseline.