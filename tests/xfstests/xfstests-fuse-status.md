# BrieFS FUSE bridge — xfstests status

Record of the FUSE-mount xfstests campaign for the Go FUSE bridge
(`fuse.briefs`, repo briefs-utils, branch `bu-refactor-1`).  The harness is
`run-suite-fuse.sh` (one mkfs/mount per test, FUSE mount via
`fuse-briefs-mount`); run archives live in `runs/` and are diffed with
`diff-runs.sh`.

## Family 1 — permission/setid/privilege tests (closed 2026-09-13)

The 20 tests: generic/087 088 093 125 193 256 314 355 375 444 597 598 633
680 683 684 685 688 696 697.  All failed at plan time (run 20260912-132029,
300/59/1); all 20 PASS as of run-20260913-200549 (single binary @5a2093b).

### Root causes and fixes

| # | Root cause | Fix (briefs-utils commit) |
|---|-----------|---------------------------|
| R1 | No `allow_other`: `fuse_permissible_uidgid` EACCES'd every non-daemon-uid process before any semantics ran | `628bd48` `fuse: mount with allow_other` |
| R1b | `default_permissions` already forced on by `EnableAcl` (FUSE_POSIX_ACL) — kernel runs all DAC/setattr_prepare/protected_* checks; bridge does NOT re-check permissions in setattr | (no change; documented) |
| R2 | Unconditional killpriv: fallocate strips nothing without killpriv_v2, and the strip ignored CAP_FSETID/file type; security.capability removal was unconditional | `9aa8ae6` caller caps/umask/groups from /proc + `d319c89` gate on CAP_FSETID, S_ISREG, S_IXGRP/in_group for sgid |
| R3 | FUSE never runs `inode_init_owner`: setgid-dir gid inheritance missing | `846c5b6` |
| R3b | Create modes applied verbatim: no umask, no default-ACL computation | `2aff8e9` `applyCreateMode` (umask or `posix_acl_create_masq` from the parent's `system.posix_acl_default`) |
| R4 | `fc->dont_mask` is set only from the daemon's init-reply FUSE_DONT_MASK (fs/fuse/inode.c:1312) — not from SB_POSIXACL — so the kernel pre-masked create modes with the umask, and the default-ACL masq intersected the wrong mode (444, 697) | `0eb4d12` negotiate FUSE_DONT_MASK via `ExtraCapabilities` |
| R5 | `fuse_set_acl` delegates the ACL→mode update to the daemon ("Fuse userspace is responsible for updating access permissions in the inode"); the bridge stored the blob verbatim (375) | `1b2528c` `setXattrOp` recomputes the mode from the access ACL and clears S_ISGID unless in_group_or_capable — the same condition the kernel signals (invisibly to go-fuse 2.10.1) via FUSE_SETXATTR_ACL_KILL_SGID in `setxattr_flags` |
| R6 | fuse_setattr never runs `posix_acl_chmod` (fs/fuse/dir.c:2365 defers it): a chmod left the stored access ACL stale, so the next setfacl that matched the stale ACL issued no setxattr and the mode never followed (375, second shape) | `c4688dc` `syncAccessAclToMode` in setattrOp's chmod branch (`__posix_acl_chmod_masq` port) |
| R7 | fusermount3 mounts nosuid,nodev,noexec: setid exec from the mount impossible (633 "setid binaries on regular mounts") | `5a2093b` mount with suid,dev,exec (kernel-mount parity) |
| R8 | `mount.fuse.briefs` discarded the `-o` value, so `mount -t fuse.briefs -o nosuid` mounted with the daemon's defaults and the request never reached the FUSE mount. generic/128 passed vacuously before the allow_other fix (its qa_user exec died with EACCES before any semantics ran); with allow_other its setuid-root `ls` really ran as root | `9bd009c` helper forwards `-o` via `--mount-opts`; the daemon merges — nosuid/nodev/noexec drop the matching permissive default, unrecognized options pass through for the kernel to reject (696's `-o noacl` fails its `_try_scratch_mount` and is skipped, same PASS) |

generic/633 passes outright — better than the plan's target of 19/20 with 633
a documented partial.  Its remaining `--test-core` suite needs no idmapped
mounts; the idmapped-mount campaign stays out of scope as planned.

### Validation run history (all in `runs/`)

- `run-20260913-190610-fuse.txt` — 16/20 (after C1–C5); 375 444 633 697 fail
- `run-20260913-194221-fuse.txt` — 18/20 (after R4+R5); 444/697 flip to PASS,
  375 fails on the stale-ACL shape, 633 on setid binaries
- `run-20260913-195632-fuse.txt` — 20/20 (after R6; mixed-binary caveat: the
  R7 binary landed mid-run)
- `run-20260913-200549-fuse.txt` — 20/20 clean single-binary confirmation

### Known non-Family-1 facts this campaign established

- go-fuse 2.10.1's `Setxattr` never sees `setxattr_flags` (FUSE_SETXATTR_EXT
  is not negotiated; the kernel sends the 8-byte compat header), so any
  flag the kernel passes there must be re-derived from caller status.
- go-fuse's `CAP_*` constants follow the kernel fuse.h, not libfuse's
  fuse_common.h (which reverses KILLPRIV/POSIX_ACL).
- go-fuse's mount defaults (`MS_NOSUID|MS_NODEV` in `mountDirect`, plus
  fusermount3's own noexec default) diverge from the kernel `mount -t
  briefs` defaults; `MountOptions.Options` clears them.

## Shutdown support (XFS_IOC_GOINGDOWN, landed 2026-09-16)

The bridge ports the kernel's `briefs_shutdown` (inode.c:159):
XFS_IOC_GOINGDOWN — numerically identical to BRIEFS_IOC_GOINGDOWN and
F2FS_IOC_SHUTDOWN — rides FUSE_IOCTL to the daemon's mount-ioctl dispatch
alongside FITRIM/FSLABEL (briefs-utils `fuse/ioctl_mount.go`, commit
69a4dfe). Kernel parity: DEFAULT/LOGFLUSH/NOLOGFLUSH flags (else EINVAL),
idempotency checked before flag validation (so an already-RO mount may
still shut down, generic/599), CAP_SYS_ADMIN gate, post-shutdown EROFS
mutations and EIO reads/fsync, and an unmount that skips the checkpoint
*and* the journal Close-flush while the deferred-metadata drain and device
sync still run (generic/417). The `_IOR` flags word arrives only as the
payload address — restricted FUSE ioctl mode copies no input for _IOR
commands — so the daemon reads it from the caller's memory at
IoctlIn.Arg via `/proc/<pid>/mem`, degrading to DEFAULT flags when
unreadable.

Unit tests (`fuse/shutdown_test.go`) pin flag semantics, the freeze
refusal contract, and the crash-sim unmount for both flag halves.
Notable finding: NOLOGFLUSH "loses" only the uncommitted journal
records, not the mutation itself — the unmount's deferred-metadata drain
persists the trie/inode blocks directly, which is exactly what the
kernel's put_super does after a shutdown (flush_owned, generic/417),
so the pinned observable is the on-disk replay range, not mutation
visibility.

### Solo run of the 37 shutdown-gated tests (`run-20260916-185725`)

18 PASS / 2 FAIL / 16 NOT RUN / 0 HANG, binary @69a4dfe. The semantically
critical ones all pass: 052 (LOGFLUSH journal-replay survival), 599
(RO-mount shutdown), 417 (post-shutdown unmount sync), 474, 051/388,
042, 392, 461, 468, 505, 507, 530, 536, 635, 646, 730, 737. The 16
NOT RUN are other-feature gates: fiemap 043–049, norecovery 050, log
configs 054/055, quota 506, crtime 508, exchangerange 722, logdev 766,
atomic writes 775/778. (623 does execute — this xfs_io build has the
`shutdown` command — and its gate `_require_xfs_io_shutdown` passes;
it was excluded from the solo list on the mistaken belief it did not.)

The 2 FAILs are pre-existing bridge gaps newly unmasked by the gate
opening, not shutdown bugs:

- generic/622 — fails at "atime didn't increase (in-memory)": the
  bridge's known atime gap. Its shutdown usage works; the lazytime
  atime checks do not.
- generic/705 — the extent check is `filefrag`, and filefrag silently
  degrades to "0 extents found" for *any* bridge file: FIEMAP is
  EOPNOTSUPP (the 6.12 VFS intercepts FS_IOC_FIEMAP before FUSE, and
  fs/fuse implements no `->fiemap`; even a forwarding client would cap
  the restricted-mode transfer at 32 bytes). Verified live on a healthy
  file with a real extent — filefrag prints "0 extents found" with
  exit 0. The test's shutdown/cycle-mount flow itself works: the 10000
  files survive with sizes intact. 705 is bridge-unaddressable until
  and unless the kernel FUSE client grows fiemap support (same class
  as O_TMPFILE; generic/032 stays NOT RUN at its explicit fiemap gate).

- generic/623 — the post-shutdown fsync reported EROFS, not the
  expected EIO: fsync flushes the mmap-dirtied page first, and that
  writeback write hit the frozen gate. Fixed in briefs-utils 855fb23:
  post-shutdown *data writes* now return EIO — the kernel's fs-level
  contract (writepages' early -EIO, the path the mmap flush exercises;
  XFS, the test's origin, returns EIO from the whole write path after a
  shutdown) — while the journal-failure freeze keeps EROFS. The kernel
  module's EROFS for a plain post-shutdown write is the VFS SB_RDONLY
  check at the syscall layer, which never reaches the filesystem and
  cannot be reproduced in the bridge (go-fuse's fs API drops the
  FUSE write_flags that would distinguish writeback writes). 623 PASS,
  042 536 622 unchanged by the fix (re-checked, run-20260917-011750).

### Full-suite validation

Two 793-test full runs at the shutdown-campaign HEADs, both zero-
regression vs the 328/32/0 baseline (run-20260915-212640):

- `run-20260916-192804-fuse.txt` (binary @69a4dfe): **346/35/0 hangs**
  — the per-test diff vs the baseline is exactly the 21 shutdown-set
  changes: 18 NOT RUN→PASS and 3 NOT RUN→FAIL (622, 623, 705); the
  other 772 tests byte-identical.
- `run-20260917-012004-fuse.txt` (binary @855fb23, after the 623
  write-errno fix): **347/34/0 hangs** — the only change vs the run
  above is 623 FAIL→PASS.  Solo re-check of the fix on 623, 042, 536,
  622: `run-20260917-011750-fuse.txt`.

Closing record: 19 shutdown-gated tests newly PASS (18 at the feature
plus 623 at the errno fix); the 2 residual FAILs are pre-existing
bridge gaps (622 atime, 705 fiemap), not shutdown bugs; 16 of the 37
gated tests stay NOT RUN on other feature gates (fiemap 043–049,
norecovery 050, log configs 054/055, quota 506, crtime 508,
exchangerange 722, logdev 766, atomic writes 775/778).

## Full-suite runs

- 20260912-132029 (pre-Family-1 baseline): 300/59/1
- 20260913-201548 (after Family 1, 793 tests): **327/33/0 hangs** — diff
  vs baseline: 27 FAIL→PASS (the 20 family tests plus 126 237 294 317
  318 452 547), 476 HANG→PASS, and one PASS→FAIL regression generic/128
  — a *vacuous* baseline pass unmasked by allow_other (root cause R8
  above, fixed by `9bd009c` after this run).  No new hangs; every
  durability/perf closure from the 09-08..09-12 campaigns held.
- 20260914-021234 (with the R8 fix, 793 tests): **328/32/0 hangs** —
  the only change vs 201548 is generic/128 FAIL→PASS.  Vs the baseline:
  27 FAIL→PASS, 476 HANG→PASS, and ZERO PASS→FAIL regressions.  This is
  the campaign's closing record: all 20 Family-1 tests pass, every prior
  closure holds, and the mount now honors `-o` options.
- 20260915-212640 (793 tests, at the briefs-utils re-review HEAD `a24c72d`
  after the 18-commit refactor series): **328/32/0 hangs** — per-test
  status byte-identical to 021234 across all 793 tests.  The refactor
  introduced no behavioral change at suite scale.
- 20260916-192804 and 20260917-012004 (shutdown campaign, binaries @69a4dfe
  then @855fb23): **346/35/0** then **347/34/0** — per-test detail in the
  shutdown section above.
- 20260920-185047 (final pre-push closing run, same binary @855fb23 as
  012004, same skip list 475+492): **349/33/0 hangs** — only two per-test
  changes vs 20260917, both improvements, ZERO regressions: generic/081
  NOT RUN→PASS and generic/108 FAIL→PASS.  Both are dm-error-path tests
  (dm-snapshot fill / partial-device failure) whose dm setup is
  nondeterministic; no bridge code changed between the runs.  Closing
  record for the fuse-failure-closing push.