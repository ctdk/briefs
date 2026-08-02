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
: "${FSCK_BRIEFS_PROG:=/go/bin/fsck.briefs}"
: "${HOST_OPTIONS:=configs/briefs.config}"
# Set FSCK_ENABLED=1 to run fsck after each test (slower, catches on-disk bugs)
: "${FSCK_ENABLED:=0}"

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

# Remove any device-mapper devices that wrap TEST_DEV or SCRATCH_DEV.  Tests
# such as generic/475 create dm-error/dm-thin-pool/dm-log-writes stacks on top
# of the scratch device; if the test is interrupted they stay behind and make
# the underlying loop device busy (mount fails with EBUSY / "Can't open
# blockdev").  Removing them restores the loop device for the next test.
cleanup_dm_for_device() {
    local dev="$1"
    local real_dev
    real_dev="$(realpath -e "$dev" 2>/dev/null)" || return 0

     # Find DM devices that have $dev as a slave and remove them.
    local slave
    slave="$(basename "$real_dev")"
    for slave_dir in /sys/block/dm-*/slaves; do
        [ -d "$slave_dir" ] || continue
        if [ -e "$slave_dir/$slave" ]; then
            local dm_name dm_num
            dm_num="$(basename "${slave_dir%/slaves}")"
            dm_name="$(cat "/sys/block/$dm_num/dm/name" 2>/dev/null)" || continue
            [ -n "$dm_name" ] || continue
            # Best-effort unmount first, then remove the DM device.
            umount "/dev/mapper/$dm_name" 2>/dev/null || true
            umount "/dev/$dm_num" 2>/dev/null || true
            dmsetup remove "$dm_name" >/dev/null 2>&1 || true
        fi
    done
}

PASS=0
FAIL=0
NOTRUN=0
HANG=0
MKFS_FAIL=0
MOUNT_FAIL=0
TIMEOUT_SECS=300

# Per-test timeout overrides for known long-running soak tests.
# These tests do 1M+ operations and need more than the default 300s.
get_timeout() {
    local testname="$1"
    case "$testname" in
        generic/089) echo 3600 ;;   # bulk fsx + many small files (~65 min)
        generic/127) echo 1200 ;;   # 6x concurrent fsx (mmap variants)
        generic/521) echo 1200 ;;   # 1M-op DIO fsx soak
        generic/522) echo 1200 ;;   # 1M-op buffered fsx soak
        *)           echo "$TIMEOUT_SECS" ;;
    esac
}

# Unmount a mount point aggressively.  Tests can leave daemons, lazy-unmount
# the device themselves, or hold references in other ways; a plain umount is
# not enough for a reliable per-test loop.
force_umount() {
    local mnt="$1"
    local i

    # Nothing to do if it is not currently mounted.
    mountpoint -q "$mnt" 2>/dev/null || return 0

    # Kill any userspace processes still using the mount.
    if command -v fuser >/dev/null 2>&1; then
        fuser -km "$mnt" >/dev/null 2>&1 || true
        sleep 0.5
    fi

    # Normal, lazy, and forced unmount attempts.
    for i in 1 2 3; do
        umount "$mnt" 2>/dev/null && return 0
        umount -l "$mnt" 2>/dev/null && return 0
        umount -f "$mnt" 2>/dev/null && return 0
        sleep 1
    done

    # If it is still mounted, give up; the caller will report the problem.
    return 1
}

# List of tests to skip due to known hangs or unsupported features.
# These tests either wedge the filesystem or test features BrieFS doesn't implement.
SKIP_TESTS="generic/068 generic/070 generic/074 generic/410 generic/475 generic/476"

should_skip() {
    local test="$1"
    local skip
    for skip in $SKIP_TESTS; do
        [ "$test" = "$skip" ] && return 0
    done
    return 1
}

for testname in "$@"; do
    testbase="${testname##*/}"
    echo "========================================"
    echo "  $testname"
    echo "========================================"

    # Skip known hanging/unsupported tests.
    if should_skip "$testname"; then
        echo "  -> SKIPPED (known hang/unsupported)"
        NOTRUN=$((NOTRUN + 1))
        continue
    fi

    # Clean slate for both devices.
    force_umount "$TEST_MNT" || true
    force_umount "$SCRATCH_MNT" || true
    cleanup_dm_for_device "$TEST_DEV"
    cleanup_dm_for_device "$SCRATCH_DEV"

    # Extra cleanup: remove any leftover files from interrupted tests.
    # This prevents "File exists" cascade failures.
    if mountpoint -q "$TEST_MNT" 2>/dev/null; then
        rm -rf "${TEST_MNT:?}"/* 2>/dev/null || true
        rm -rf "${TEST_MNT:?}"/.* 2>/dev/null || true
    fi
    if mountpoint -q "$SCRATCH_MNT" 2>/dev/null; then
        rm -rf "${SCRATCH_MNT:?}"/* 2>/dev/null || true
        rm -rf "${SCRATCH_MNT:?}"/.* 2>/dev/null || true
    fi

    sync

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
    if ! mount -t briefs "$TEST_DEV" "$TEST_MNT" >/tmp/mount-err.log 2>&1; then
        echo "  -> MOUNT FAIL: $(cat /tmp/mount-err.log)"
        MOUNT_FAIL=$((MOUNT_FAIL + 1))
        # Try to leave things as clean as possible for the next test.
        force_umount "$TEST_MNT" || true
        force_umount "$SCRATCH_MNT" || true
        continue
    fi

    status=0
    # Run without -b briefs so result files land in results/generic/ and the
    # existing generic golden outputs are used.
    tsecs=$(get_timeout "$testname")
    timeout "$tsecs" ./check "$testname" >/tmp/check_last.log 2>&1 || status=$?
    cat /tmp/check_last.log

    # Clean up mounts before moving on, best effort.
    force_umount "$TEST_MNT" || true
    force_umount "$SCRATCH_MNT" || true
    cleanup_dm_for_device "$TEST_DEV"
    cleanup_dm_for_device "$SCRATCH_DEV"

    # Optional fsck validation (enabled via FSCK_ENABLED=1).
    # Runs after each test to catch on-disk consistency bugs early.
    if [ "$FSCK_ENABLED" = "1" ] && [ "$status" -ne 124 ]; then
        if "$FSCK_BRIEFS_PROG" -n "$TEST_DEV" >/tmp/fsck_test.log 2>&1; then
            : # fsck clean, no output needed
        else
            echo "  -> FSCK WARN (see /tmp/fsck_test.log)"
            HANG=$((HANG + 1))  # Count fsck failures separately (not a hang, but tracked)
        fi
    fi

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
    elif grep -qE "Passed all [1-9][0-9]* tests" /tmp/check_last.log; then
        echo "  -> PASS"
        PASS=$((PASS + 1))
    elif grep -qE "Passed all 0 tests" /tmp/check_last.log; then
        # Test was interrupted or could not run (e.g. mount failure inside
        # ./check itself); count it as a failure, not a pass.
        echo "  -> FAIL (interrupted, 0 tests passed)"
        FAIL=$((FAIL + 1))
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
if [ "$FSCK_ENABLED" = "1" ]; then
    echo "  (fsck validation enabled)"
fi
echo "========================================"
