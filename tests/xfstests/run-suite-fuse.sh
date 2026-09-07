#!/bin/bash
# run-suite-fuse.sh — run xfstests against the FUSE mount (fuse.briefs).
# Exports the FUSE-mount env vars and execs run-suite.sh, which defaults to
# the kernel mount; the overrides here switch it to the FUSE path.
#
# Usage (inside the VM, as root):
#   bash /vagrant/tests/xfstests/run-suite-fuse.sh generic/001 generic/003 ...

# One suite at a time: a second concurrent instance would share TEST_DEV/
# SCRATCH_DEV, and each runner's per-test cleanup (umount loops + pkill of
# fuse daemons + mkfs) tears down the other's live mounts mid-test — every
# test ENOTCONNs (observed 2026-09-07 after a double launch).  The bracket
# trick keeps the pattern from matching this script's own command line
# ("run-suite-fuse.sh" does not contain "run-suite.sh").
if pgrep -f "run-suit[e]\.sh" >/dev/null 2>&1; then
	echo "FATAL: a run-suite instance is already active:" >&2
	pgrep -af "run-suit[e]\.sh" >&2
	exit 1
fi

export MOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-mount
export UMOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-umount
export HOST_OPTIONS=configs/briefs-fuse.config
exec /vagrant/tests/xfstests/run-suite.sh "$@"