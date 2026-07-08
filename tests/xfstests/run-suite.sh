#!/bin/bash
# BrieFS xfstests per-test runner.
#
# xfstests mounts TEST_DEV once per ./check invocation and does NOT reformat it
# between tests; SCRATCH_DEV is reformatted by _scratch_mkfs, but TEST_DIR
# accumulates files/inodes from tests that leave residue or fail cleanup.  That
# poisons bulk-run tallies (e.g. generic/001 exhausting the fixed inode table
# and causing an ENOSPC cascade for hundreds of later tests).
#
# This script runs each requested test in isolation: unmount both devices,
# mkfs.briefs them, mount TEST_DEV, and run exactly one xfstest.  It is the
# standard way to get trustworthy BrieFS pass/fail numbers.
#
# Usage (inside the VM, as root or via sudo):
#   bash /vagrant/tests/xfstests/run-suite.sh generic/001 generic/003 ...
#   bash /vagrant/tests/xfstests/run-suite.sh $(awk '{print "generic/"$1}' /xfstests/tests/generic/group.list)
#
# The script uses /xfstests/configs/briefs.config written by setup-vm.sh.
set -euo pipefail

: "${XFSTESTS_DIR:=/xfstests}"
: "${BRIEFS_SRC:=/vagrant}"
: "${MKFS_BRIEFS_PROG:=/go/bin/mkfs.briefs}"
: "${FSCK_BRIEFS_PROG:=/go/bin/fsck.briefs}"
: "${HOST_OPTIONS:=configs/briefs.config}"

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/sbin:/usr/bin:/bin:${PATH}"
export HOST_OPTIONS

cd "$XFSTESTS_DIR"

# Resolve device names from the active config.
TEST_DEV="${TEST_DEV:-/dev/loop0}"
SCRATCH_DEV="${SCRATCH_DEV:-/dev/loop1}"
TEST_MNT="${TEST_DIR:-/mnt/briefs-test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/briefs-scratch}"

# Ensure module is loaded.
modprobe briefs_fs 2>/dev/null || insmod "/lib/modules/$(uname -r)/extra/briefs/briefs_fs.ko" 2>/dev/null || true

PASS=0
FAIL=0
NOTRUN=0
TIMEOUT_SECS=300

for testname in "$@"; do
    echo "========================================"
    echo "  $testname"
    echo "========================================"

    # Clean slate for both devices.
    umount "$TEST_MNT" 2>/dev/null || true
    umount "$SCRATCH_MNT" 2>/dev/null || true
    "$MKFS_BRIEFS_PROG" -f "$TEST_DEV" >/dev/null
    "$MKFS_BRIEFS_PROG" -f "$SCRATCH_DEV" >/dev/null

    # Mount TEST_DEV; SCRATCH_DEV is mounted by the test itself.
    mount -t briefs "$TEST_DEV" "$TEST_MNT"

    status=0
    timeout "$TIMEOUT_SECS" ./check -b briefs "$testname" || status=$?

    # Tally based on xfstests result files if available.
    bad="$XFSTESTS_DIR/results/briefs/${testname##*/}.out.bad"
    if [ -f "$bad" ]; then
        FAIL=$((FAIL + 1))
        echo "  -> FAIL"
    else
        full="$XFSTESTS_DIR/results/briefs/${testname##*/}.full"
        if grep -q "\[not run\]" "$full" 2>/dev/null; then
            NOTRUN=$((NOTRUN + 1))
            echo "  -> NOT RUN"
        else
            PASS=$((PASS + 1))
            echo "  -> PASS"
        fi
    fi

done

echo ""
echo "========================================"
echo "  BrieFS per-test run complete"
echo "  PASS:    $PASS"
echo "  FAIL:    $FAIL"
echo "  NOT RUN: $NOTRUN"
echo "========================================"
