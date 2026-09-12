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

## The full-suite re-run under fix C + 736a395 + 0e4fc16 + ffdc719 (2026-09-11)

run-20260911-011508-fuse.txt (commit 063e746, VM 01:15-08:15): **277 PASS
/ 72 FAIL / 431 NOT RUN / 2 SKIP / 11 HANG / 0 fsck-warn** (793 processed),
against the pre-fix full run's 188/110/63 (run-20260907-225019-fuse.txt)
and the kernel baseline's 456 PASS / 3 accepted FAIL (311 547 563).

FAIL diff (pre-fix -> post-fix):
- 49 fixed: 008 011 013 029 033 059 062 070 071 075 078 086 089 101 112
  130 198 210 214 240 251 263 286 299 322 340 344 345 346 354 361 363
  401 412 428 437 439 446 469 479 567 619 626 647 650 729 749 758 759 —
  the fix-B/C/007/585/127 clusters plus the DIO stress cluster.
- 11 newly failing, of which only 7 are genuine PASS->FAIL regressions:
  **073 300 335 336 343 520 534** — and 6 of those 7 (all but 300, fio
  aio-dio) are fsync/log-replay/power-failure durability tests
  (write+fsync+replay, dm content-after-power-failure, link+fsync).  That
  points at fix B's deferred write-back/durability semantics, not the
  007/585/127 fixes.  Corroborating: 521/522, the 1M-op fsx soaks that
  PASSED with the per-op-sync bridge, now HANG under the deferred
  write-back cache.
- 4 are HANG->FAIL promotions (progress, still broken): 108 500 563 589
  (500 FSTRIM unwired, 589 mount-helper gap, 563 the accepted cgwb FAIL).

The 11 HANGs: 069 074 103 113 410 476 521 522 642 748 750 (747 now
passes).  Scope-CPU from the systemd Consumed lines: every one burned
<48 s CPU over their windows (069 10.7 s, 074 6.4 s, 113 4.0 s, 642 2.8 s,
410 22.8 s, 476 38.6 s, 521 43.8 s, 522 48.0 s, 748 33.8 s, 750 39.7 s;
103's scope emitted no Consumed line at all — started 02:06:37,
"Deactivated" 02:11:34, the full ~297 s to the KILL).  All wedge-shaped
blocked waits, NOT the old throughput family (which burned hundreds of
CPU-seconds).  Forensics re-run of the 11 with the fixed watcher is in
flight; the first watcher's 642 capture was lost to a filename bug
(`generic/642` contains a slash -> unwritable subdirectory path).

### The 7 FAIL regressions: .out.bad triage (2026-09-11)

Three distinct signatures, none of them the 007/585/127 fixes:

- **A. flakey remount fails fast (073 335 336 343).**  All four fail
  with `mount.fuse.briefs: timed out waiting for /mnt/briefs-scratch`
  remounting the dm-flakey device after the power-cut switch —
  generic/073's check log shows failure at 2 s, so the daemon is not
  slowly replaying; it either errors out or liveloops at mount almost
  immediately.  The daemon log is a shared filename and was overwritten,
  so the concrete error is unconfirmed — needs a solo repro that saves
  the log at the failure instant.  Note the same remount SUCCEEDS in
  520's cases, so it is state-dependent (journal content at the cut?),
  not "flakey device" per se.  The initial mount on the flakey device
  also succeeds in every one of these tests.
  **UPDATE 2026-09-11 (fix-D validation run): the surviving daemon log
  finally shows the concrete error — `journal replay: replay pass 2
  (apply): replay record type 7 (JRN_DIR_UPDATE) at block 26213825: no
  space left on device`, then the daemon exits and the mount helper
  times out.  So the daemon ERRORS OUT at mount: replay-apply hits
  ENOSPC (allocator exhaustion or a device-write error surfaced as
  ENOSPC on the flakey device) applying a directory-update record whose
  trie/slot re-derivation needs an allocation.  Log preserved at
  ~/src/briefs-notes/fuse-fixd-logs/073-daemon-mount.log.  Solo repro
  should reproduce the cut, then inspect which allocation in the
  DIR_UPDATE replay path returns ENOSPC.**

  **ROOT-CAUSED + FIXED 09-11 (utils 01734ab).  Two prongs of the
  kernel's generic/475 replay fix were missing from the bridge: (1)
  trieSeedPool — replayDirUpdate never seeded the partial-trie-page
  pool from the parent's on-disk trie, so replay inserts took the
  fresh-page branch where the live path reused a partial page, and
  ENOSPC'd on the full fs; (2) replayTrieBlocks — pass 1 never
  collected JRN_TRIE_ALLOC blocks for pass 2's page-inits to pop LIFO,
  so replay re-allocated blocks the window had already reserved
  (invisible to AllocBlock) and ENOSPC'd.  Both ported from kernel
  journal.c/trie.c with kernel-parity EEXIST tolerance; regression
  tests TestReplayTriePoolSeedingFullFs and TestReplayTrieBlockPoolFullFs
  pin them independently on full-fs crash-replays (each fails
  deterministically without its prong, passes with the other disabled).
  VM validation 09-11: solo generic/073 with the fixed binary PASSES
  (run-20260911-152737-fuse.txt), daemon log clean of replay errors.
  The flakey-remount family members 335/336/343 share the mechanism
  and should re-validate on the next subset run.**
  **UPDATE 09-12: they do NOT — the re-validation run
  (run-20260912-010647-fuse.txt) failed all three with the same
  073-family signature (replay JRN_DIR_UPDATE ENOSPC for 335/336; raw
  "bad trie page magic 0xabababab" for 343).  A second, distinct
  mechanism: see the "Resolution: 335/336/343" section below.**
- **B. sync(2) is a silent no-op (520).**  Only the `_scratch_sync`
  (global sync) cases fail; every fsync case passes.  sync(2) can never
  reach a regular FUSE mount's daemon (6.12 sets fc->sync_fs only for
  fuseblk; go-fuse has no SYNCFS dispatch — recorded during the reclaim
  work), so under fix B's deferred write-back nothing commits and the
  cut loses the whole workload ("After:" empty).  The per-op-sync
  bridge masked this.  Fix options: fuseblk mount + a patched go-fuse
  SYNCFS dispatch, or accept sync(2) as unwired and document it.
  **ACCEPTED 2026-09-12 (user decision): sync(2) is a documented
  limitation of the non-fuseblk FUSE mount — kernel-parity behavior
  would require fuseblk + a patched go-fuse FUSE_SYNCFS (opcode 50)
  dispatch, which is not worth the dependency patch for one test.
  520 joins 563 as an accepted FAIL; the forensics doc and
  xfstests-fuse-status record it.**
- **C. rename old-name loss (534).**  After truncate+rename+fsync and
  the cut, `bar` survives with correct size/content but `foo` STILL
  EXISTS too — same-inode rename where the old name's removal is lost.
  fsync committed everything (bar is right), so this is replay
  idempotency or a deferred-trie state issue in the rename path, not a
  sync gap.  **ROOT-CAUSED + FIXED 09-11 (utils 8a780be) — see the
  "Resolution: 534" section below.**
- **D. fio aio-dio EIO (300).**  Three `io_u error ... Input/output
  error` on 128K DIO writes under the random aio-dio pattern — real
  bridge DIO bug, unrelated to durability.  **ROOT-CAUSED + FIXED
  09-11/12 (utils e68e4a1 + ebf9863) — see the "Resolution: 300"
  section below.**

069's wedge produced no D-state client (FUSE waits are interruptible),
so the D-state-only watcher v1 missed it; watcher v2 (D-state OR frozen
check-log) is now watching the re-run.

### All 11 HANGs classified (2026-09-11, task #12 complete)

hang11b targeted re-run (run-20260911-102239-fuse.txt): 9 FAIL fast (the
watcher's SIGQUIT converts hangs by design), 410 HANG with no capture,
0 fsck-warn.  Every capture was settled by goroutine dumps plus a
per-thread /proc poller (/tmp/daemon-poll.log):

- **521/522/642/748/750 = fsync-path whole-device fsync ladder.**
  BrieFSNode.Fsync -> Journal.Sync -> syncLocked -> syncSuperblock ->
  `j.file.Sync()` on the raw device fd (journal_write.go:585), once per
  commit, seconds each on the VM virtio disk.  Poller proof for 521:
  daemon write_bytes climbs 25M->393M over 45 s with D-state threads
  cycling blkdev_issue_flush — continuous progress, a ladder of slow
  flushes, not a wedge.  Watcher trigger B ("waiting>=1 persistent")
  cannot distinguish one stuck request from a ladder of slow ones.
- **750 = same ladder.**  The "142 runnable go-fuse goroutines" is
  writers piling behind the serialized device flush, not a Go runtime
  pathology; write_bytes climbed 104M->2.7G over the window.
- **074/103 = checkpoint whole-device fsync** (checkpointLocked
  `j.file.Sync()` at journal_write.go:514/:563) under the journal lock.
- **476 = whole-device fdatasync** (DrainPendingData ->
  BlockDevice.Fdatasync, device.go:165) plus a sync.Mutex waiter.
- **069 = raw write throughput** (3M 4-byte O_APPEND writes, each a
  full tree walk) — workload cost, not a barrier.
- **410 = known flaky**, hung again, no capture.

NET: 8 of the 9 captures are the whole-device-barrier family, so
targeted flushes (fix D below) are the single lever for all of them.

### Residue-panic family fixed (utils ce27022 + 8424fe5)

The hang11b captures also yielded two daemon-panic root causes, both
fixed 2026-09-11 with regression tests, full fuse suite green (~23 s):

- **ce27022, inline truncates (generic/551 daemon death).**  The generic
  extent path's rebuildExtentIndex called SetInlineExtents([8]Extent{})
  which CLOBBERS the inline data region — inline_extents and inline_data
  are the same 256 bytes — so a down-truncate wiped the surviving head
  (the kernel handles inline separately, file.c:1236); a truncate-up past
  256 left the flag set with FileSize>256, panicking every later
  readFileData/promoteInlineData on the out-of-range region slice.
  InlineData() returns the region BY VALUE: mutations require
  SetInlineData back.
- **8424fe5, MAX_LFS_FILESIZE tail (generic/525 daemon death).**
  writeExtentData's blockEnd = blockStart + blockSize overflows to
  INT64_MIN at the final block; readFileData had endOff/extEnd/blkEnd
  overflows plus a PHANTOM loop iteration (blkOff += blockSize wraps
  negative and INT64_MIN < readEnd re-enters, slicing buf[-101:]).
  EFBIG gates mirror file.c:3483 and inode_newsize_ok; a write ending
  exactly at s_maxbytes is legal (525 passes on the kernel), so the
  loops are hardened, not gated.

### Fix D: targeted writeback flushes (utils 4e2761f, 2026-09-11)

Replaces the whole-device barrier family:

- BlockDevice tracks blocks written since the last flush (pendingWB,
  noted by every WriteBlock/WriteBlockSlot; capped at 8192 entries with
  an inline drain so no-fsync workloads like 069 cannot grow it without
  bound).  FlushPendingWB completes writeback with sync_file_range
  (WAIT_BEFORE|WRITE|WAIT_AFTER) over coalesced contiguous runs.
- Journal journal-block and superblock writes are write-through
  (WriteAt + sync_file_range), so the commit point never advances past
  a record whose block is still only in the page cache.
- The three `j.file.Sync()` barriers become a WBFlusher hook the bridge
  implements over FlushPendingWB (standalone users keep the old
  whole-file Sync fallback).
- checkpointLocked now flushes the allocator pools BEFORE the
  checkpoint block that snapshots them; the old code ordered this
  correctly only by accident, its single trailing Sync covering pools
  and checkpoint block together, after.
- Fsync keeps exactly ONE device flush (Fdatasync) at the end — power-
  fail parity with the kernel's per-fsync blkdev_issue_flush (82c9a61);
  unmount keeps its dev.Sync; the xattr and label-set paths use
  targeted flushes.

Design note: RWF_SYNC/RWF_DSYNC pwritev2 was considered and rejected —
the kernel routes both through generic_write_sync ->
vfs_fsync_range -> blkdev_issue_flush, i.e. the very per-write device
flush being removed.  sync_file_range is the correct primitive: it is
explicitly documented as not flushing disk caches, matching
sync_dirty_buffer semantics.

### Fix D residue: the 074/642 post-body unmount storm (09-11)

The 22-test fix-D validation subset came back 13/4/3 with the barrier
family gone (103 476 521 522 748 750 all PASS — 521 was the flagship
1M-op DIO fsx ladder) and 335/336/343 of the flakey-remount family
fixed by the changed writeback timing.  Three HANGs remained: 069
(raw-write throughput), 074, and-by-extension 642.  A solo 074 run with
the daemon poller + a SIGQUIT goroutine dump classified the 074 shape:

- The test body COMPLETES (check prints the NNs); the hang is inside
  check's `_check_filesystems` -> `_scratch_unmount`, while the kernel
  pushes the test's remaining mmap-dirty pages through the FUSE
  writeback-cache connection (fstest writes ~11.6 GB cumulative on a
  3 GB-RAM VM; poller showed write_bytes climbing 0.36 -> 11.61 GB at a
  steady 56-74 MB/s, only 3 transient D-state samples — continuous
  progress, NOT a wedge).
- Goroutine dump: goroutine 1 in go-fuse `sync.WaitGroup.Wait` (the
  unmount waiting for in-flight requests, 3 minutes), goroutine 47
  [running] inside a Write request blocked in noteWB's over-cap drain
  — the blocking `sync_file_range(WAIT_*)` — i.e. every 8192-block
  (32 MB) batch stalled a writer on disk completion.  The observed
  56-74 MB/s may be that convoy, not the virtio floor.

Mitigation (2nd commit of fix D, utils): noteWB's over-cap drain is now
KICK-ONLY — `sync_file_range(SYNC_FILE_RANGE_WRITE)` without any WAIT
flags — so the write path never stalls on disk and back-pressure is
left to the kernel's own dirty-page throttling (the same place the
kernel module's write path gets its).  Commit-time FlushPendingWB keeps
the WAIT flags, and the end-of-fsync Fdatasync still covers blocks
kicked out of the tracked set before they completed, so the ordering
and power-fail guarantees are unchanged.

Solo 074 re-run results (09-11, two runs):

- With kick-only (TIMEOUT_SECS=900): still HANG.  So the blocking drain
  was not the whole story.
- Instrumented re-run (TIMEOUT_SECS=1200 + root /proc/<pid>/io
  sampling): still HANG at 1200 s, with write_bytes at 28.1 GB by
  el=720 s and 34.5 GB by el=871 s — a steady ~45 MB/s, no plateau, no
  D-state accumulation.  Continuous progress, NOT a wedge: the daemon
  is simply being asked to write far more than the test's logical
  volume.  (The SIGQUIT goroutine capture for this run was lost to a
  tool outage inside the capture window; the rate evidence + code
  reading below were sufficient to classify.)

Root cause — extent-index rebuild write amplification (09-11): the
bridge's write path sets `rebuildNeeded` for every extent-adding op and
`rebuildExtentIndex` (file_ops.go) re-allocates and re-writes the ENTIRE
extent B+tree (BuildBtreeLeaves + BuildBtreeIndex over all E extents)
per FUSE WRITE request — the code comment says outright: "the
incremental insert is deferred".  generic/074's fstest writes 10-30 MB
files in 512 B fragments with holes (`-F -b 512`, stride 1 KB), so
each file fragments to ~7.7K extents = ~61+ leaf blocks (126 extents
per leaf) that are re-written on EVERY writeback request that extends
the tree — tens of GB of amplified device writes for a test whose
logical volume is a few GB, growing quadratically with file size.  The
kernel module passes 074 quickly because briefs_extent.c APPENDS
extents to a chain (O(1) amortized), never rebuilds.  A timeout
override cannot absorb this (the 1200 s run was still mid-storm at
34.5 GB); the fix is localized rebuilding — reuse leaf blocks whose
extent chunk is unchanged (reuse is only valid for an equal-content
prefix whose boundary leaf is rebuilt, because a reused leaf's stored
next_leaf must match its new successor), rebuild the few index nodes
on top, and free only the actually-replaced blocks.  Tracked as the
incremental-rebuild task; same root cause expected for 069 (raw-write
throughput) and 642.

### Resolution: 074 PASSES in 442 s (09-11)

The hang decomposed into FOUR stacked per-op costs, each fixed and
measured in turn (all in briefs-utils, committed on bu-refactor-1):

1. **Full-B+tree re-emit per WRITE** (the amplification above) →
   localized leaf-diff rebuild `rebuildExtentIndexWrite`
   (utils 8643303): positional 126-extent chunking, equal-content
   prefix minus boundary leaf reuses its old blocks, index levels
   always rebuilt fresh, only replaced leaves + old idx freed.
   Device writes for the 074 shape: 34.5 GB → ~5 GB.  Regression
   tests: prefix reuse, middle-shift reuses nothing, kill-9 crash
   replay through reused blocks.
2. **Ring-wrap whole-set writeback wait**: checkpointLocked's first
   syncWB waited (sync_file_range WAIT_*) over the whole mixed
   data+metadata pending set under j.mu every ring wrap (~every 500
   fragmented ops) — invisible on the local tmpfs probe, dominant on
   the VM.  Now KICK-ONLY (utils cd69023), matching the kernel's
   checkpoint (journal.c:705 flushes only journal-owned metadata +
   allocator bitmaps, never user data).  fsync/unmount keep the waits,
   so the power-fail promise is unchanged.
3. **Per-op O(E) tree walk + CRC on the WRITE path**
   (collectExtentTree per request; run #2 was CPU-bound at 131%) →
   per-inode walked-tree cache (utils 694151f): entries validated by
   the (root, total) pair against the freshly-read inode — every
   rebuild allocates a fresh root, so a matching root means the cached
   tree IS the on-disk tree.  Shared read-only entries; the write path
   clones before insertExtentSorted's in-place merges (the merge test
   pins this: a mutated shared chunk would make the prefix compare
   falsely match).  64 entries, arbitrary-victim eviction.
4. **Per-op O(E) walk on GETATTR/LOOKUP/READ — the real dominant CPU
   sink.**  A SIGQUIT goroutine dump of still-wedged run #3 (the dump
   cost the run: recorded FAIL by design) showed the spinning threads
   in WalkBtree/VerifyBtreeNodeChecksum under GETATTR, not WRITE: the
   attr fillers (fillAttrOut, fillEntryOut) walked the whole
   CRC-verified tree on EVERY Getattr/Lookup just to sum ext.Len for
   st_blocks, and the read path did the same per READ.  fstest stats
   and reads constantly.  Routed through the same cache: extentBlocksOf
   for the fillers, collectExtentTree in readExtentData (utils
   50027af).  In-process probe: 8-9 µs/op on the 074 shape.

Result: solo `generic/074` run-20260911-175053-fuse.txt — **PASS in
442 s** (73 s harness overhead + ~370 s test body + unmount), where
both post-kick runs HANGed past 900 s and the pre-fix run past 1200 s
at 34.5 GB device writes.  One contaminated intermediate run (my own
pkill hit a suite I didn't realize was live — the interrupted launch
HAD fired) confirmed the body pacing: ~22 MB into fstest.2 within
~2 min of test time.

## Resolution: 534 replay re-derives the old name across trie churn (09-11)

Root cause (utils fix 8a780be): the rename empties the parent's
directory trie, whose live path frees the old root
(collapseAncestry) and re-creates a fresh one — trie root churn the
kernel shares (trie.c frees the root and clears dir_trie_root at
lines 1082-1097).  The bridge's replayDirUpdate re-read the parent
inode block for EVERY JRN_DIR_UPDATE record, so the re-derived
DEL(foo) and ADD(foo→bar) could land on different trie instances
when a create-time JRN_INODE_FULL snapshot restore (which re-points
DirTrieRoot at the stale pre-churn root) interleaved mid-replay:
ADD applied to the final on-disk root R2, DEL applied to stale root
R1 — foo survived replay with bar correct.  fsync committed
everything (fsync is commit-not-checkpoint under fix B), so the
on-disk state at the cut was already post-rename and the old name
was pure re-derivation, exactly as triage suspected.

The kernel is immune by accident of its inode cache: replay_dir_update
(journal.c:898) igets the parent once and mutates the CACHED
binfo->disk_inode; later replay_inode_full restores write the raw
block but never refresh the cached copy, so every DIR_UPDATE in the
window applies against the same mutated inode.  The fix ports that:
replayDirUpdate reads the parent inode block once (replayParents map
scoped to replayJournal), and the persist step writes the mutated
copy back each time.  Freed-parent (magic 0) records skip, matching
kernel iget -EINVAL.  Regression test TestReplayRenameOldNameStaysGone534
mirrors the test's op sequence through the bridge methods, cuts
without the unmount checkpoint, replays, and asserts foo is gone —
fails deterministically pre-fix.

VM validation: solo generic/534 PASSES (run-20260912-001634-fuse.txt,
first post-fix run; the earlier FAIL run-20260911-234304-fuse.txt was
the launch without HOST_OPTIONS that mounted kernel-briefs by
mistake, and 534v's run-20260911-234944-fuse.txt reproduced the FAIL
on the FUSE path pre-fix).

## Resolution: 300 punch-path ENOSPC mislabeled EIO (09-11/12)

The triage's "DIO bug" read was wrong on two counts.  The solo repro
(run-20260912-002448, PASS with `errors: total=0`) still showed
`falloc_raicer: err=5 (func=td_io_queue, error=Input/output error)`,
and generic/300's fio config tells the rest: falloc_raicer and both
punch_hole_raicer jobs use `ioengine=falloc` — they issue ONLY
fallocate syscalls, no writes at all.  So the EIO was returned by the
FALLOCATE op itself.

Root cause (utils e68e4a1 + ebf9863): on the racer-filled device the
punch path's full extent-index rebuild needed a fresh leaf block and
the allocator (meta-class, shield-bypassed, reclaim-retried) genuinely
found none — ENOSPC, wrapped as `alloc leaf 145: no space left on
device`.  errToErrno's bare type switch saw only the fmt.Errorf
wrapper and converted EVERY wrapped syscall error to EIO.  fio's
config explicitly tolerates ENOSPC (`ignore_error=,ENOSPC`,
`continue_on_error=write`) but not EIO, so the mislabel poisoned the
job dispatch where the real errno would have been ignored.

Fix: errToErrno now unwraps via errors.As so a buried errno reaches
the caller, and logs only the truly errno-less internal failures
(e68e4a1 added the logging that captured the raw text above; ebf9863
added the unwrap — Fsync's three bare-EIO returns are routed through
the same helper).  Validation run-20260912-004446: PASS, and every
fio job error is now err=28 (No space left on device), which the test
tolerates — zero EIOs, no new daemon EIO lines.

Known divergence remaining (accepted for now): the bridge's punch
does a full index rebuild that allocates every leaf fresh, where the
kernel's briefs_btree_delete_range punches in place and never
allocates — so a punch on a genuinely full fs can ENOSPC where the
kernel cannot.  The write path's localized leaf-diff rebuild
(utils 8643303) is the shape of the eventual fix if this ever bites a
test that does NOT tolerate punch ENOSPC.

## 520 accepted: sync(2) cannot reach a non-fuseblk daemon (09-12)

Root cause confirmed against the 6.12 source: fuse registers
.sync_fs = fuse_sync_fs, but sets `fc->sync_fs = 1` only under
`if (ctx->is_bdev)` (fs/fuse/inode.c:1742 — fuseblk mounts); for every
other FUSE mount fuse_sync_fs returns 0 immediately when
`!fc->sync_fs` (inode.c:740).  The FUSE_SYNCFS opcode (50) is simply
never sent, and go-fuse v2.10.1 has no dispatch for it in any case.
So sync(2) on a fuse.briefs mount commits nothing — under fix B's
deferred write-back the cut at the end of generic/520's sync-only
workload loses everything, and only the fsync cases pass.

Decision (user, 2026-09-12): ACCEPTED as a documented limitation —
same class as every other non-fuseblk FUSE filesystem (the kernel
would permanently disable sync_fs on ENOSYS even if we could see the
requests, inode.c:755).  520 joins 563 as an accepted FAIL.  The fix
shape if ever revisited: fuseblk mount so the kernel sets fc->sync_fs,
plus a go-fuse FUSE_SYNCFS dispatch patched in via a local fork or a
replace directive.

## Resolution: 335/336/343 — freed trie pages never reached the disk (09-12)

After 01734ab, the re-validation run (run-20260912-010647-fuse.txt)
still failed all three with the same signatures (JRN_DIR_UPDATE replay
ENOSPC for 335/336; raw "bad trie page magic 0xabababab" for 343), so
a second mechanism was at work.  In-process repro of 335 built
(utils fuse TestReplay335FullWindowReplay): it failed deterministically
at the re-derived create-time dir-add of a/b/foo, which walks a/b's trie
root 2368 — the page the mv later emptied and freed.

All three tests call `_scratch_sync` — a NO-OP on the FUSE mount (the
520 finding) — so the single fsync commits the ENTIRE window since
mount and replay must re-derive every create against already-applied
on-disk state.  When the mv empties a/b's trie, collapseAncestry frees
root block 2368, and `deferBlockFree` DROPPED the page's deferred
content at free time (cache.go, the generic/040/041 clobber guard):
the page had never been written to disk, so at the cut block 2368 held
mkfs-era garbage.  Replay's re-derived dir-add walked it → bad magic →
TrieInsert collapsed the error to ENOSPC (the known 073 gotcha (e));
343 surfaced the raw magic error through a different call path.

The kernel avoids this twice over: its sync_fs works (the mid-test
_scratch_sync checkpoints the creates and shrinks the window), and its
Phase 2 pin-survives-free design (5524267 + ca4478b) keeps freed-but-
dirty metadata buffers journal-owned until the commit writes them.

Fix (utils efb2c87): deferBlockFree keeps the deferred content, and
SyncMeta now drains BEFORE applying the pending frees, so a freed block
cannot enter the allocator while its stale copy is still pending — the
drain can never clobber the block's next owner (the free-vs-commit
class the drop guarded).  cacheDrop still clears the per-op cache;
freeBlockNow keeps its drop (its callers free after their own sync
already drained).  On a drain error the taken frees are restored to
pendingFrees.  Replay tolerates the re-derived dir-del re-freeing the
resurrected root (dataAlloc.FreeBlock is an idempotent bitmap clear).

VM validation 09-12: run-20260912-014325-fuse.txt — 335/336/343 all
PASS, 0 FAIL/0 HANG/0 fsck-warn.  With this, every member of the 7-FAIL
regression set from the full re-run is resolved or accepted (073 534
300 fixed, 335 336 343 fixed, 520 accepted).

## The 09-12 full 793-test run: fix D + efb2c87, second mechanism (09-12)

run-20260912-022251-fuse.txt (commit 35dfb93, fsck_enabled 0): **289 PASS
/ 67 FAIL / 431 NOT RUN / 2 SKIP / 4 HANG** vs the 09-11 run's 277/72/11
— 12 more PASS, 5 fewer FAIL, 11 HANGs down to 4.  The HANG→PASS
promotions are fix D paying off across the whole barrier family
(incl. 103 113 521 522 748); the remaining 4 HANGs are 074 410 642 750,
all 300 s-budget KILLs of legitimate soaks (074 442 s and 642 421 s
measured solo on 09-10; 750 first surfaced under fix D; 410 needs a
solo measurement).  get_timeout now gives all four 900 s.

FAIL diff (09-11 → 09-12): 15 fixed, 6 new.  The 6:

- **341 342 376 510 771 — one cluster, one mechanism** (below).
- **299 — deterministic bridge OOM, root-caused (09-12, corrected
  below)**: the initial "OOM flake, not a bridge bug" read was wrong.
  Solo re-runs reproduced the kill identically (4 kills, all
  `anon-rss:3193760kB`), and SIGUSR1 MemStats dumps (utils mem_debug.go)
  showed the persistent deferred structures (dirtyBlocks,
  pendingFrees, extent trees) staying tiny while one falloc's transient
  working set blew the heap.  See the resolution section below.

### Resolution: 341/342/376/510/771 — poisoned first-touch replay anchor (09-12)

All five failed with DOUBLED directory entries after a crash-replay
(341's a/ = `[x x y y]`).  In-process repro (utils
TestReplay341RenameDirRecreateName): mkdir a; mkdir a/x; 32K foo/bar
in a/x; `_scratch_sync` (no-op on FUSE); `mv a/x a/y`; mkdir a/x (new
empty dir reusing the old name); fsync; cut; replay.

Root cause: `replayParents` (8a780be's per-replay parent-inode cache,
loaded once at the parent's first DIR_UPDATE) was **poisoned by an
early-window INODE_FULL snapshot**.  The rename that emptied a/ freed
its trie root, the add of "y" re-created one, and the mkdir of the new
"x" journaled a parent INODE_FULL — whose post-mutation DirTrieRoot was
restored onto the inode block BEFORE the first DIR_UPDATE touched a/.
First-touch then cached that snapshot's STALE root instead of the live
path's final state.  The stale root's freed slot had been live-reused
(pool scan order) for the new "y" leaf, so the re-derived dir-adds
`trieLinkChild`ed a second copy of the entries into pages reachable
from the final root.

Why the kernel cannot hit it: its crash cut loses unflushed metadata,
so the inode block holds the window START at replay iget and an inode
restored before its first DIR_UPDATE is simply uncached; the bridge
drains all metadata at every fsync, so the pre-replay block holds the
window END — which is the anchor re-derivation needs.

Fix (utils 5e62601, "pristine anchor"): the first time replay restores
an inode not yet in replayParents, stash the PRE-restore slot content
(`replayParentsPristine`); replayDirUpdate's first touch prefers the
stash.  At the pre-replay anchor every drained add EEXISTS / del
ENOENTs as a no-op, undrained-tail records re-derive genuinely, and
every rename's del/add pair stays on one root (a refresh-on-restore
design was tried first and REVERTED — it fixed 341 but broke 534, whose
first parent snapshot arrives AFTER its first dir-add).  Full utils
suite green; binaries deployed to the VM.  Solo re-validation
(run-20260912-095821-fuse.txt): 341 342 376 510 771 all PASS, 0 HANG,
0 fsck-warn — the cluster is closed.

### Resolution: 299 — per-block journal-record amplification, one falloc = 26M records (09-12)

Solo re-runs killed the daemon identically (4 kills, all
`anon-rss:3193760kB` on the 3.9 GB VM), so the full-run "OOM flake"
disposition was wrong — the kill is deterministic.  The USR1 MemStats
dumps (new mem_debug.go instrumentation, `sudo kill -USR1` to the root
daemon) separated the suspects: dirtyBlocks=3 (0 MB),
pendingFrees≤2.5K, extentTrees≈6 — every persistent deferred structure
tiny, while heapAlloc oscillated 600 MB→2.1 GB with heapSys pinned at
2.8 GB and `journalDirty=true` mid-op.  Not a leak: one operation's
transient working set.

Root cause: `commitExtentChange` journaled **one JRN_EXTENT_ALLOC per
allocated block** (file_ops.go, `Length: 1` hardcoded).  generic/299's
`falloc 0 $FILE_SIZE` over the whole ~100 GB scratch = 26,214,278
blocks = 26.2M records from a single operation.  The ~2800-record ring
filled ~9,300 times — a back-pressure checkpoint each fill — and the
26M transient record buffers grew the Go heap arena until the oom-killer
chose the daemon (fio's oom_score_adj 250 made it the pick, but the
daemon genuinely held the memory).  The kernel never had this shape:
btree.c journals **run-encoded** — one
`briefs_journal_extent_alloc(..., ext->len, ...)` per extent — and the
on-disk record format always carried a length; only the bridge producer
hardcoded 1.  Both replay sides (kernel journal.c
replay_extent_alloc/free, bridge journal_replay.go) already handled
Length > 1, and fsck does not interpret extent records.

Fix (utils efc8cf4, `fuse: run-encode journal EXTENT_ALLOC/FREE
records`; instrumentation e85cb03):
`journalContigRuns` coalesces commitExtentChange's block lists into
maximal contiguous runs (one record per run); the single-block producers
(dir trie promotion, inline promotion, xattr/symlink blocks, inode free)
pass an explicit length; freeInodeData journals one EXTENT_FREE per
extent like the kernel's briefs_btree_free_all (a giant-file unlink had
the same amplification).  Pinned by TestExtentJournalRunEncodedAndReplay:
record counts O(runs) for a half-device falloc + truncate, and crash
replay reserves/frees the same allocator bits the live path did.

Why 09-11's full run survived: marginality, not a different code path —
the same 26M records were written; GC timing kept peak RSS just under
the kill ceiling.  The 09-12 run lost that race.

## Remaining follow-ups

1. ~~Triage 007~~ DONE.  ~~Triage 585~~ DONE.  ~~ENOSPC cluster 102
   226 274 275 371 511~~ DONE (reclaim, utils 0e4fc16, above).
   ~~Triage 127~~ DONE (drain window, utils ffdc719, above).
2. ~~Scope-CPU~~ DONE (all 11 < 48 s CPU — wedge-shaped, above).
   ~~Triage the 11 HANGs from the watcher goroutine dumps~~ DONE 09-11
   (classification section above; 8 of 9 captures = whole-device barrier
   family, fixed by utils 4e2761f).  ~~The 7 genuine FAIL regressions~~
   073 + 534 + 300 root-caused and fixed (01734ab, 8a780be,
   e68e4a1+ebf9863, sections above);
   074/642/069 rebuild amplification fixed (074 PASSES, above).
   ~~520~~ ACCEPTED 09-12 (sync(2) cannot reach a non-fuseblk daemon —
   section above; joins 563 as accepted FAIL).
   ~~335/336/343~~ FIXED 09-12 (freed trie pages never reached the
   disk — not the 01734ab mechanism; see the "Resolution: 335/336/343"
   section above; utils efb2c87, run-20260912-014325-fuse.txt 3/3).
   The 7-FAIL regression set from the full re-run is fully closed
   (fixed: 073 300 335 336 343 534; accepted: 520).  Remaining: the
   residual FAILs from the full run's 72 (accepted: 563, 520).
3. 563 remains the known accepted cgroup-writeback FAIL.
4. generic/475 dm-error (separate, pre-existing: fsstress D-state ~5 h).
5. Benchmark 736a395 (fsx A/B for the residual 1,052 fdatasyncs/10K ops;
   the 45-test re-run archive measured 42c9b58 only).
6. ~~Re-run the full 793-test suite~~ DONE 2026-09-11
   (run-20260911-011508-fuse.txt, section above).
7. pendingFrees never drain on ring wrap: writeRecordLocked clears
   j.dirty at journal_write.go:242 BEFORE the back-pressure checkpoint
   at :252 fires mid-op, so the in-flight record commits only at the
   next Sync and the bridge's queued deferred frees are not applied by
   the checkpoint path — recorded 2026-09-09, still unaddressed.