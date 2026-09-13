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

## Full-suite baseline

- 20260912-132029 (pre-Family-1): 300/59/1
- Full-suite re-run after Family 1: see `runs/` for the newest archive and
  `diff-runs.sh` for the regression diff against the baseline.