# FUSE bridge vs kernel module — full generic-suite comparison (2026-09-08)

First full-suite run of the fuse.briefs bridge with the kernel module
completely absent, compared against the kernel-module baseline.  Both runs
process the same 793 generic tests.

|                    | Kernel module<br>(run-20260904-063947) | fuse.briefs bridge<br>(run-20260907-225019) |
|--------------------|------:|------:|
| Processed          | 793 | 793 |
| PASS               | 456 | 188 |
| FAIL               |   3 | 110 |
| NOT RUN            | 332 | 430 |
| SKIP               |   2 |   2 |
| HANG               |   0 |  63 |
| MKFS/MOUNT FAIL    |   0 |   0 |
| FSCK WARN          |   — |   0 |

Kernel baseline: commit cdf60b7, 6.12.101-lockdep, 456/3 FAIL(311 547 563)/332/2,
fsck validation off.
FUSE run: commit 3805089 (bu-refactor-1), bridge with the RENAME_WHITEOUT
shard fix (briefs-utils 747ef76), fsck validation on (0 warnings — the
bridge left no on-disk corruption fsck can detect, across 793 tests).

## Cross-tab (kernel outcome -> FUSE outcome)

- kernel 456 PASS -> **187 PASS, 110 FAIL, 61 HANG, 98 NOT RUN**
- kernel   3 FAIL -> 547 **PASS**, 311 + 563 **HANG**
- kernel 332 NOT RUN -> all 332 stay NOT RUN
- kernel   2 SKIP  -> same 2 (475, 492)

So of the 456 tests the kernel passes, the bridge re-passes 41% outright;
59% regress to FAIL, HANG, or NOT RUN (feature gap).

## FUSE failure taxonomy

### 1. The 63 HANGs — ROOT CAUSE FOUND 2026-09-08 (was the dominant unknown)
Forensics in `fuse-hang-forensics-20260908.md` (evidence:
`fuse-hang-forensics-20260908.txt`).  Not one family:
- **~48 = throughput blowout, not a deadlock.**  The bridge runs a full
  journal checkpoint on every metadata op (~6 device fsyncs, ~31 ms per
  create — measured 24,406 fsyncs / 125 s for 4096 creates on generic/006),
  because `syncLocked`'s back-pressure condition
  (journal_write.go:278 `writePos == JournalLogStart`) is true after every
  checkpoint (it sets logStart = writePos) — a context-free misport of the
  kernel's ring-full-only test (journal.c:591-618).  Metadata-heavy tests
  exceed the 300 s timeout with zero output.
- **4 = genuine livelock** (full-core CPU burn all 300 s): 091 617 751 760.
- **11 unclassified** (no scope-CPU report): 014 069 103 108 133 249 449
  511 524 563 747.

006 007 014 027 069 074 077 091 095 100 102 103 108 113 114 127 129 132
133 224 226 247 249 273 274 275 310 311 320 339 347 366 371 410 416 418
449 460 465 471 476 488 500 511 524 558 563 585 589 590 610 617 627 642
676 707 736 747 748 750 751 760 761

### 2. Readdir omits entries behind long names (011 family)
011 013 070 078 — `rm: cannot remove …: Directory not empty` on cleanup;
readdir silently omits entries in directories behind long name components
whose cumulative depth overflows TrieIterator's fixed [256] name stack
(fuse/trie.go:149-150, 204-208, 221-225, 243-247).  Review finding E1
secondary, now with four reproducers.

### 3. Durability / journal replay
071 — wrote 32 MB, after unmount/remount only 16 MB survives (daemon
exit = full journal replay path loses the file tail).  NOT the
never-ported generation guards: fda802f already ported kernel 33e4019
(guards verified in fuse/journal_replay.go:452).  Needs its own triage.

### 4. fallocate punch / zero-range
008 059 — holes not created; stale data read back where the hole should
be.  The kernel's punch-split fixes (truncate_pagecache_range folded
into briefs_btree_delete_range etc.) have no bridge counterpart.

### 5. Kernel semantic fixes never ported to the bridge
All the kernel-module fix clusters from Aug-Sep regress on the bridge:
- setattr/permissions: 086 087 088 089 093 198 285 441 444 437 438 439
  446 452 631 633 680 683 684 685 688 696 697
- killpriv-on-modify: 093 193 683 684 688
- timestamps: 423 424 (current_time at all time-write sites)
- immutable +i: 079 (not enforced at all); large ACLs: 026 (E2BIG)
- rename whiteout: 626 (the shard deadlock is fixed, content still fails)
- leaf-chain: 112 (the Go btree has the same dangling next_leaf bug the
  kernel fixed in e6d3d6d)
- DIO stress cluster: 250 251 252 299 619; io_uring: 617 HANG

### 6. Feature gaps (98 kernel-PASS -> NOT RUN)
The bridge lacks whole features the kernel has, so check's _require_*
probes bail: fiemap, POSIX ACLs, O_TMPFILE, EXCHANGE_RANGE/SWAPEXT,
FSTRIM, FS_IOC_SETFSLABEL, fallocate ZERO/COLLAPSE/INSERT_RANGE —
review findings E3/E4/E6.

### 7. Previously-known bridge bugs confirmed again
003 (atime never updated on read), 029 (mmap writeback loses EOF tail),
322 (sparse write past EOF never advances i_size) — same as the
2026-09-06 honest subset run.

### Notable FUSE positives
- 547 PASSES on the bridge (the kernel's accepted FAIL — the chown/trie
  replay path works in the Go implementation).
- 521 522 (1M-op fsx soaks) pass with the per-op-sync bridge.
- FSCK WARN = 0 across all 793 tests.
- 321 547 640 (the crash-consistency trio from the subset run) hold.

## What the comparison says
The bridge's on-disk format handling is fundamentally sound (fsck clean,
547/521/522 pass, the big stress soaks survive), but it (a) still wedges
under load — 63 hangs, root cause not yet captured; (b) is missing the
entire Aug-Sep corpus of kernel correctness fixes (setattr, killpriv,
punch, timestamps, DIO); (c) has the 011 readdir bug; and (d) lacks the
kernel's feature surface (ACLs, fiemap, ioctls, fallocate modes), which
alone converts 98 kernel passes into NOT RUN.

## Follow-ups
1. ~~Reproduce one HANG with SIGQUIT forensics~~ DONE 2026-09-08 — root
   cause of the dominant family found (per-op checkpoint misport; see
   fuse-hang-forensics-20260908.md).  Remaining: fix it (options A/B in the
   forensics doc), chase the 4 livelocks (091 617 751 760) and the 11
   unclassified HANGs, then re-run the 63.
2. Port TrieIterator to a dynamic name stack (011 013 070 078).
3. Triage the remaining FAILs individually (the taxonomy above covers
   ~60 of the 110; the rest need per-test .out.bad triage).
4. Port the kernel fix corpus (071's replay guards are already ported;
   start from the setattr/killpriv clusters).