#!/bin/bash
# run-fuse-subset.sh — run the FUSE-targeted xfstests subset against the
# fuse.briefs FUSE bridge (NOT the kernel module), with per-test isolation.
#
# This is a thin wrapper over run-suite.sh.  It exports the FUSE-mount
# environment so that both the TEST_DEV mount run-suite.sh performs itself
# (MOUNT_CMD) and the SCRATCH_DEV mount ./check performs (MOUNT_PROG, set
# by the [briefs] section of briefs-fuse.config, which common/config
# sources AFTER its line-116 `MOUNT_PROG=$(type -P mount)` reset) go through
# the fuse.briefs bridge.  It clears the kernel-tuned SKIP_TESTS list so the
# subset — including generic/475 — runs in full.
#
# Usage (inside the VM, as root):
#   bash /vagrant/tests/xfstests/run-fuse-subset.sh
exec >/tmp/fuse-subset.out 2>&1
set -uo pipefail

export HOST_OPTIONS=/xfstests/configs/briefs-fuse.config
export MOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-mount
export UMOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-umount
export SKIP_TESTS=""
# Run fsck.briefs after each test to catch on-disk consistency bugs.
export FSCK_ENABLED=1

# generic/011 (dirstress) and generic/475 (dm-error crash-replay) are the
# long-running members; run-suite.sh's get_timeout gives them 900s.
exec /vagrant/tests/xfstests/run-suite.sh \
  generic/003 generic/029 generic/030 generic/032 generic/321 \
  generic/322 generic/547 generic/640 generic/475 generic/011