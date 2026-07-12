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
#   bash /vagrant/tests/xfstests/run-suite.sh $(awk '/^[^#]/ {print "generic/"$1}' /xfstests/tests/generic/group.list)
#
# The script uses /xfstests/configs/briefs.config written by setup-vm.sh.
set -uo pipefail

: "${XFSTESTS_DIR:=/xfstests}"
: "${MKFS_BRIEFS_PROG:=/go/bin/mkfs.briefs}"
: "${HOST_OPTIONS:=configs/briefs.config}"

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/sbin:/usr/bin:/bin:${PATH}"
export HOST_OPTIONS

cd "$XFSTESTS_DIR"

# xfstests config files contain a [briefs] section header; source only the
# simple export lines we need and ignore the section marker.
eval "$(grep -E "^(export )?(TEST_DEV|SCRATCH_DEV|TEST_DIR|SCRATCH_MNT)=" "$HOST_OPTIONS" | sed "s/^export //")"

TEST_DEV="${TEST_DEV:-/dev/loop0}"
SCRATCH_DEV="${SCRATCH_DEV:-/dev/loop1}"
TEST_MNT="${TEST_DIR:-/mnt/briefs-test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/briefs-scratch}"

# Ensure module is loaded.
modprobe briefs_fs 2>/dev/null || insmod "/lib/modules/$(uname -r)/extra/briefs/briefs_fs.ko" 2>/dev/null || true

PASS=0
FAIL=0
NOTRUN=0
HANG=0
MKFS_FAIL=0
MOUNT_FAIL=0
TIMEOUT_SECS=300

for testname in "$@"; do
    testbase="${testname##*/}"
    echo "========================================"
    echo "  $testname"
    echo "========================================"

    # Clean slate for both devices.
    umount "$TEST_MNT" 2>/dev/null || true
    umount "$SCRATCH_MNT" 2>/dev/null || true

    if ! "$MKFS_BRIEFS_PROG" -f "$TEST_DEV" >/dev/null 2>&1; then
        echo "  -> MKFS TEST FAIL"
        MKFS_FAIL=$((MKFS_FAIL + 1))
        continue
    fi
    if ! "$MKFS_BRIEFS_PROG" -f "$SCRATCH_DEV" >/dev/null 2>&1; then
        echo "  -> MKFS SCRATCH FAIL"
        MKFS_FAIL=$((MKFS_FAIL + 1))
        continue
    fi

    # Mount TEST_DEV; SCRATCH_DEV is mounted by the test itself.
    if ! mount -t briefs "$TEST_DEV" "$TEST_MNT" 2>/dev/null; then
        echo "  -> MOUNT FAIL"
        MOUNT_FAIL=$((MOUNT_FAIL + 1))
        continue
    fi

    status=0
    # Run without -b briefs so result files land in results/generic/ and the
    # existing generic golden outputs are used.
    timeout "$TIMEOUT_SECS" ./check "$testname" >/tmp/check_last.log 2>&1 || status=$?
    cat /tmp/check_last.log

    # Clean up mounts before moving on, best effort.
    umount "$TEST_MNT" 2>/dev/null || true
    umount "$SCRATCH_MNT" 2>/dev/null || true

    if [ "$status" -eq 124 ]; then
        echo "  -> HANG (timeout)"
        HANG=$((HANG + 1))
        continue
    fi

    # Parse xfstests' own summary lines.  They are more reliable than guessing
    # from result-file existence, and they correctly distinguish a test that
    # passed from one that was entirely not-run.
    if grep -qE "^Failures: (generic/)?${testbase}(\s|$)" /tmp/check_last.log; then
        echo "  -> FAIL"
        FAIL=$((FAIL + 1))
    elif grep -qE "^Not run: (generic/)?${testbase}(\s|$)" /tmp/check_last.log; then
        echo "  -> NOT RUN"
        NOTRUN=$((NOTRUN + 1))
    elif grep -qE "Passed all [0-9]+ tests" /tmp/check_last.log; then
        echo "  -> PASS"
        PASS=$((PASS + 1))
    else
        # Ambiguous result (e.g. ./check aborted before printing a summary).
        echo "  -> UNKNOWN (exit $status)"
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "========================================"
echo "  BrieFS per-test run complete"
echo "  PASS:       $PASS"
echo "  FAIL:       $FAIL"
echo "  NOT RUN:    $NOTRUN"
echo "  HANG:       $HANG"
echo "  MKFS FAIL:  $MKFS_FAIL"
echo "  MOUNT FAIL: $MOUNT_FAIL"
echo "========================================"
