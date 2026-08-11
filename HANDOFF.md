# Handoff: BrieFS xfstests, performance, iomap migration & dedup

This file tracks where the BrieFS kernel module + userspace tools stand and
what remains. The top section is current; the refactor-round-1 history below
is kept as a record. Branch of record: `refactor-round-2` (11 commits ahead of
`master` as of 2026-08-10).

## Current state (refactor-round-2, 2026-08-11)

### Branch contents (11 ahead of master)
- **iomap data-path migration** — DONE. The regular-file data path moved off
  `buffer_head` to `iomap` (phases 0-8). Direct I/O closes generic/704;
  `bmap`+`swap` close generic/472, 643. Metadata (btree/trie/inode/journal
  blocks) still uses `buffer_head` — see task 4b.
- **K2/K4 cross-site dedup** (per `~/src/briefs-notes/quirky-fluttering-floyd.md`,
  i.e. the dedup plan):
  - K1 `briefs_sync_dirty_buffer` wrapper — `e6321c9` (replay-path sites) +
    `6a1fbc0` (remaining checked sites).
  - K2 `briefs_persist_and_journal_inode` wrapper — `72ae10c`.
  - K3 `briefs_sync_inode_fields` (VFS-field mirror) — `6c3eba0`; extent
    `briefs_append_extent` fold — `6393a46`; `briefs_compute_i_blocks` to
    header — `7e8b205`.
  - K4 `btree_descend_to_leaf` (routes lookup + clear_unwritten) — `9dc82f4`.
  - K0 `briefs_block_in_range` device-end guard — `84be966`.
- **FUSE read/write bridge** — works (`c3f4670`…`c794549`). A per-op block
  cache is the key insight. The FUSE xfstests subset harness was fixed (it had
  been mounting via the kernel module, not `fuse.briefs`).
- **generic/411 fix (prior session)** — `fb176b8` + `67ab528` (see below).
- **4b investigation (prior session)** — researched whether to move metadata
  off `buffer_head`; conclusion: **reject 4b as written**, reframe to
  "journal-owned metadata buffer lifetimes". See task table + full report at
  `~/src/briefs-notes/4b-metadata-off-buffer_head-plan.md`.

### generic/411 + 410 — RESOLVED this session
generic/410 and generic/411 (mount-namespace / propagation tests) were
long-skipped. Both are now un-skipped and PASS.

- **generic/410** passes; it exercises pure VFS shared-subtree machinery and
  BrieFS needs no special support. Its skip-list entry was stale.
- **generic/411 was a memory-safety bug, not a missing feature.** Concurrent
  fsstress exposed a kmalloc-4k slab-out-of-bounds in the journal record
  writer. KASAN pinpointed it at `__briefs_journal_write_record_locked+0x263`
  (symlink-data `memcpy` overrunning the 4096-byte `j->cur_block` by 496 B).
  Root cause: the bounds check at the top of the flush branch and the record
  `memcpy` were not atomic — the back-pressure ring-full checkpoint releases
  `j->write_lock` (to avoid an AB-BA deadlock with `alloc->lock`), concurrent
  writers advanced `write_offset` past the checked value, and the post-checkpoint
  `memcpy` ran unchecked. On a normal kernel the overflow smashed the adjacent
  `struct briefs_sb_info` → `vfree(garbage)` oops → umount died holding
  `s_umount` → every later mount wedged.
- **Fix (`fb176b8`):** extracted `briefs_journal_flush_cur_block_locked()` and
  added a `while (write_offset + total_size > JOURNAL_BLOCK_SIZE)` re-check
  loop *after* the lock-releasing checkpoint, so the record write is atomic
  w.r.t. `write_offset`. Verified: 12/12 iterations clean under a KASAN kernel;
  4/4 iterations clean on the normal kernel (411 PASS, no oops/wedge).

### Test status (recent full-suite runs)
- 2026-07-13: 368 PASS / 11 FAIL / 399 NOTRUN (mount fails were DM residue;
  `force_umount` + DM cleanup added).
- 2026-06-28: 783 ran, only generic/311 + 563 failing.
- Most earlier failures are FIXED (see `MEMORY.md`). Standing failures:
  - **generic/475** — DEFERRED. Robust fix needs a journal-format or sync-model
    change; a bounds-check guard (`da618f6`) prevents the unkillable busy-loop,
    but umount-under-error hangs (writeback redirty of EIO folios) is the real
    blocker. See `~/src/briefs-notes/generic-475-plan.md`.
  - **generic/563** — 6.12.y kernel cgroup-writeback race (CVE-2026-31703),
    not a BrieFS bug; `SB_I_CGROUPWB` was dropped (`9385fc8`).

## Remaining tasks — evaluated 2026-08-11

| Task | Status | Current reality |
|------|--------|-----------------|
| 3a per-inode journal batching | ✅ DONE | refactor-round-1; deferred `JRN_INODE_FULL` + fsync snapshot. |
| 3b checkpoint interval | ✅ DONE | `3ee8bfe` (1024→4096). |
| 4a xattr lock ordering | ⏳ PENDING | Still **documented-only** — the `xattr_sem → alloc->lock` inversion note + TODO remain in `xattr.c:24-28`; refactor (alloc before `xattr_sem`) not done. Low priority: provably safe today (no path takes `alloc->lock → xattr_sem`). |
| 4b metadata off buffer_heads | ❌ REJECTED (reframed) | Investigated; **do NOT move metadata off `buffer_head`.** iomap has no metadata API (by design — Chinner: "never intended for metadata use"); bespoke alternatives (XFS `xfs_buf`, Btrfs, bcachefs) are all ~22k lines, larger than BrieFS's whole module; no filesystem has migrated existing metadata off bh. BrieFS's "iomap data + bh metadata" split IS the destination (GFS2 in-tree precedent; ext4 converging there; LSFMM 2023). **Reframed successor:** harden the bh-metadata layer by borrowing GFS2's `gfs2_bufdata` (off `bh->b_private`) + `BH_Pinned` pattern so the journal owns metadata bh lifetimes — addresses the real recurring bugs (8 unchecked `sync_dirty_buffer` sites, 18 dropped persist `-EIO`s, generic/475 umount-redirty-EIO wedge). 4-phase plan (Phase 1 `briefs_bufdata`; Phase 2 `BH_Pinned`; Phase 3 error funnel + fsync ordering; Phase 4 deferred folio cache only if `CONFIG_BUFFER_HEAD` pressure forces it). Full report: `~/src/briefs-notes/4b-metadata-off-buffer_head-plan.md`. |
| 4c mount namespaces (410/411) | ✅ DONE | This session: 410 un-skipped (VFS-handled); 411 = memory-safety bug FIXED `fb176b8`; both un-skipped `67ab528`. |
| 5a generic/127 mmap/fsx wedge | ⚠️ DEFERRED | **Intermittent.** Passes in Jul-13 / Aug-2 full runs but exhibits a pre-existing silent full-VM freeze under fsx+mmap at other times (needs `virsh destroy` to recover). Root cause UNPINNED (no trace; `CONFIG_LOCKDEP` not enabled). Not the iomap migration's fault. Not in skip list. Needs lockdep or journal/writeback decoupling. See memory `briefs-generic-127-fsx-mmap-wedge-preexisting`. |
| 5b generic/299 btree corruption | ✅ DONE | `e59c1e4` (zero stale btree tail entries). |
| 5c profile journal lock contention | ⏳ NOT STARTED | Low priority; `write_lock` contention largely addressed by 3a batching + commit-before-flush. |
| Full xfstests suite | ✅ DONE | Run multiple times (tallies above). |
| lockdep verification | ⚠️ DEFERRED | Needs a custom kernel with `CONFIG_LOCKDEP`. Defer until BrieFS tracks mainline master. (A KASAN kernel was built this session — `6.12.101-kasan` — for the 411 investigation; a lockdep kernel would be similar effort.) |
| fsck after tests | ✅ DONE | `FSCK_ENABLED=1` in run-suite.sh (`44247fe`). |
| K2/K4 cross-site dedup | ✅ DONE | See branch contents above. (K5 flag-mask was attempted and REVERTED — it exposed a latent `+D` DIRSYNC value-specific bug; see memory `briefs-k5-audit-flagmask-deferred`.) |
| generic/475 | ⚠️ DEFERRED | See test-status section. |
| generic/563 | ➖ N/A | Upstream kernel race, not BrieFS. |

## How to build and test (current)

Build the module **on the VM** (host headers mismatch the VM kernel → "Invalid
module format"); rsync `/vagrant → /tmp/briefssrc`, `make clean` first.

```bash
# Rebuild + install the module on the VM (normal kernel)
vagrant ssh -c '
  rm -rf /tmp/briefssrc && rsync -a --exclude=.git /vagrant/ /tmp/briefssrc/
  cd /tmp/briefssrc && make clean && make
  sudo cp briefs_fs.ko /lib/modules/$(uname -r)/extra/briefs/ && sudo depmod -a
'

# Rebuild Go tools (scp'd source ≠ installed /go/bin/*)
vagrant ssh -c '
  cd /go/src/github.com/ctdk/briefs-utils
  go build -o /go/bin/mkfs.briefs ./cmd/mkfs/ && go build -o /go/bin/fsck.briefs ./cmd/fsck/
'

# Run xfstests (resumable; SKIP_TESTS env forces all)
vagrant ssh -c 'sudo bash /vagrant/tests/xfstests/run-suite.sh generic/001 generic/003'
vagrant ssh -c 'sudo SKIP_TESTS="" bash /vagrant/tests/xfstests/run-suite.sh generic/410 generic/411'
```

The module's version is the git rev baked at build time (build-id observability,
landed `297b478`); `modinfo` / sysfs / debugfs surface it.

## VM state (current)

- VM: vagrant libvirt, IP `192.168.121.206` (libvirt DHCP — varies on rebuild;
  use `vagrant ssh` rather than the IP).
- Running kernel: `6.12.101+deb13-amd64` (normal). A `6.12.101-kasan` kernel
  is also installed (CONFIG_KASAN_GENERIC+INLINE+VMALLOC+STACK) for the 411
  memory-safety investigation; grub default is the normal kernel.
- Module: `/lib/modules/6.12.101+deb13-amd64/extra/briefs/briefs_fs.ko`.
- Go binaries: `/go/bin/mkfs.briefs`, `/go/bin/fsck.briefs`.
- xfstests: `/xfstests/` (NFS mount; empty after restart → `vagrant halt &&
  vagrant up` — NOT `vagrant reload` — to repopulate).
- Test devices: `/dev/vdb1` (TEST_DEV), `/dev/vdc1` (SCRATCH_DEV).
- NFS mounts in the VM: `/vagrant`, `/kernel-src`, `/go`, `/xfstests`.

## Historical: refactor-round-1 (what was done)

### Investigation (3 parallel agents + direct code review)
- Analyzed journal write path, checkpoint, sync, and replay in `journal.c`
- Analyzed mmap, writeback, page fault, and fsync paths in `file.c` and `iomap.c`
- Analyzed btree drain, lock ordering, and xattr paths
- Reviewed xfstests runner (`tests/xfstests/run-suite.sh`) and test infrastructure
- Reviewed briefs-utils (`mkfs.briefs`, `fsck.briefs`, on-disk format)

### Root cause analysis (refactor-round-1)
The primary bottleneck was `sync_blockdev()` called under `j->write_lock` on
every fsync and checkpoint — flushing ALL dirty metadata buffers while holding
the global journal mutex, blocking all other fs operations. Secondary issues:
64-block journal too small, btree drain doing synchronous I/O, writeback path
allocating blocks from writeback context.

### Changes implemented (branch `refactor-round-1`)

**Kernel module:**

1. `briefs_journal.h` — Added `BRIEFS_MIN_JOURNAL_BLOCKS` (4) and
   `BRIEFS_MAX_JOURNAL_BLOCKS` (65536) constants.
2. `journal.c` — Added journal size validation in `briefs_journal_init()`.
3. `journal.c` — **Commit-before-flush**: restructured
   `__briefs_journal_sync_locked` to advance `journal_log_end` and sync the
   superblock BEFORE releasing `j->write_lock`, then run `sync_blockdev()` and
   the per-block journal sync loop OUTSIDE the lock. Other threads can write
   journal records during the metadata flush (journal replay is idempotent).
4. `journal.c` — Moved `sync_blockdev()` outside `j->write_lock` in
   `__briefs_journal_checkpoint_locked` (alongside the existing
   `briefs_alloc_sync()` release).
5. `journal.c` — Changed checkpoint buffer `kzalloc(..., GFP_KERNEL)` to
   `GFP_NOFS`.
6. `briefs.h` — Added `BRIEFS_BTREE_ERROR_LIMIT` (100) and `atomic_t
   btree_error_count` to `struct briefs_sb_info`.
7. `btree.c` — Changed `pr_err` to `pr_warn_ratelimited` for btree checksum /
   bad-magic errors. Added a circuit breaker: after 100 errors, sets
   `BRIEFS_MF_ERROR_FS` to force read-only remount (prevents the generic/299
   dmesg-flood spiral).
8. `file.c` — Changed 3 `GFP_KERNEL` to `GFP_NOFS` in `kvmalloc_array` calls
   under `extent_lock` (collect_all_extents, truncate, punch hole).
9. `tests/xfstests/run-suite.sh` — Added `get_timeout()` with per-test timeout
   overrides (089→3600s, 127/521/522→1200s).

**Userspace tools:**

10. `briefs/briefs.go` — Replaced `DefaultJournalSize = 64` with
    `DefaultJournalMinBlocks = 64` + `DefaultJournalMaxBlocks = 4096`. Added
    `DefaultJournalBlocks(totalBlocks)`: `max(64, totalBlocks/4096)`, capped 4096.
11. `cmd/mkfs/mkfs.go` — Changed `--journal-size` default 64 → 0 (auto), with
    auto-scaling when 0.
12. `README.md` — version refs v0.9.3 → v0.9.4.

### generic/013 / 299 btree checksum corruption — FIXED
Root cause: btree nodes store variable-length arrays in fixed 4096-byte blocks;
operations reducing `num_keys` left stale data in unused slots, and since
CRC32C covers bytes 0-4079, stale data caused non-deterministic checksum
mismatches. Fix (`e59c1e4`): `memset()` zero unused tail entries in
`btree_delete_range_subtree`, `btree_maybe_split_child`, `btree_ensure_root_room`,
`btree_spill_inline`. Verified 19+ consecutive generic/013 passes, 0 checksum
errors.

### refactor-round-1 commit summary (key commits)

| Commit | Description |
|--------|-------------|
| `e59c1e4` | btree: zero stale tail entries to fix checksum mismatches ✅ |
| `3ee8bfe` | kernel: increase JRN_CHECKPOINT_INTERVAL 1024→4096 ✅ |
| `f38f53e` | kernel: fix generic/127/521/522 hangs and ENOENT punch hole bug |
| `5581394` | kernel: fix journal replay to propagate I/O errors |
| `a5fedb6` | kernel: add metadata write-error propagation to btree drain |
| `a6c8b2e` | kernel: fix lock ordering inversions to prevent deadlocks |

### Phase 3a (per-inode journal batching)
`briefs_journal_inode_full()` copies the snapshot to
`binfo->pending_journal_snapshot` and defers the write until
`briefs_inode_sync()` at syscall boundaries; `briefs_fsync()` captures a fresh
inode snapshot after `file_write_and_wait_range()` (this fixed generic/073).
Files: `briefs.h`, `briefs_journal.h`, `journal.c`, `file.c` (27 call sites
updated to pass `struct inode *`). Spot tests 001/003/013/073/127 passed.

### SCRATCH_DEV cleanup (commit `50731b6`)
sync + sleep before mkfs, lazy-unmount fallback, double-check SCRATCH_MNT
before mkfs, aggressive cleanup after tests — eliminated the ~300 false FAILs
from device RO/mounted residue between tests.