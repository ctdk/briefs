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

### Why every op checkpoints: a misported back-pressure condition

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

The kernel condition this was ported from is deliberately different
(`journal.c:591-618`, comment verbatim):

> Having just advanced write_pos, write_pos == journal_log_start holds ONLY
> when the ring has wrapped completely full: in the empty state (right after
> a checkpoint, or at fresh mount) journal_log_start == write_pos but
> next_block(write_pos) != write_pos, so the condition above is never taken
> there.

The kernel tests equality *after advancing write_pos in the record-append
path* (fires only on a genuine full-ring wrap).  The bridge tests the same
equality *at Sync time before writing* — precisely the empty state — turning
a rare back-pressure path into the common case.  The comparison was ported
without its context.

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

- **A. Fix the misport (minimal, kernel-faithful):** make the Sync-time
  back-pressure fire only on a genuine full-ring wrap (kernel semantics),
  e.g. gate on `recordsSinceCheckpoint > 0` (0 right after a checkpoint) or
  an explicit live-blocks count.  Cuts ~6 fsyncs/op to ~2–3.  006 drops to
  roughly half its current time — still likely over 300 s; the fsstress-scale
  HANGs need more.  CAUTION: this activates the bridge's ring-wrap path,
  which the current checkpoint-every-op behavior never exercises; it needs
  the same concurrency care the kernel path got (cf. journal.c:591 lock-order
  fix, generic/411).
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

## Remaining follow-ups

1. Livelock family: repro one of 091/617/751/760, capture where the CPU goes
   (test-side spin; 617 is the known io_uring DIO soak flake candidate).
2. Classify the 11 tests with no scope-CPU report (014 069 103 108 133 249
   449 511 524 563 747) — likely family 1, but confirm at least one.
3. generic/475 dm-error (separate, pre-existing: fsstress D-state ~5 h).
4. After the fix lands: re-run the 63 and re-diff against kernel baseline.