#!/bin/bash
# guards-stage2.sh — Stage 2 (fast briefs_crc32c: slicing-by-8 over the frozen
# nonstandard polynomial) guard set, per the io_uring-perf plan.  Wrong math
# fails loudly (every verify -EIOs), so the guards are round-trips and
# cross-implementation reads:
#
#   1. 138-test test-runner round-trip (write + read + fsck via loop image,
#      module built with the new CRC).
#   2. Interop: mount the perf image (/dev/vdd, written by the OLD module and
#      partially read by the Stage-1 module) and read the WHOLE 32G file with
#      the new module — every btree node's checksum was computed by the old
#      byte-at-a-time table; any deviation in the slicing tables -EIOs.
#   3. Journal probe-replay (tests/repro-replay.sh on a fresh loop0).
#   4. fsck.briefs clean on the perf image after unmount (Go CRC cross-check
#      over the same on-disk bytes).
# Prints "GUARDS2 DONE <verdict>"; run AFTER guards-stage1.sh has finished
# (this script swaps the loaded module).
set -u

export PATH=/go/bin:/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin
BLD="${BLD:-/tmp/briefssrc}"
PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
MKFS="${MKFS:-/go/bin/mkfs.briefs}"
FSCK="${FSCK:-/go/bin/fsck.briefs}"

# reload — test-runner.sh rmmods the module in its cleanup, and a bare
# `mount -t briefs` would auto-load whatever stale module depmod knows about
# rather than the tree under test.
reload() {
    umount "$MNT" 2>/dev/null || true
    rmmod briefs_fs 2>/dev/null || true
    insmod "$BLD/briefs_fs.ko" || { echo "GUARDS2 DONE FAIL:insmod"; exit 1; }
}

# 0. (re)load the module under test
reload
dmesg -C >/dev/null 2>&1 || true

# 1. test-runner round-trip
echo "=== 1. test-runner round-trip ==="
if env BRIEFS_MODULE="$BLD/briefs_fs.ko" BRIEFS_MKFS="$MKFS" \
     BRIEFS_FSCK="$FSCK" bash /vagrant/tests/test-runner.sh; then
    echo "test-runner: PASS"
else
    echo "test-runner: FAIL"
    echo "GUARDS2 DONE FAIL:test-runner"
    exit 1
fi
reload

# 2. old-image interop: read the whole 32G file with the new CRC
echo "=== 2. old-module-written image full read ==="
# The Stage-1 module already verified every node and left BH_Verified set on
# the surviving buffers (drop_caches cannot evict pages with buffer_heads),
# which would turn this into a skip-fest instead of a CRC cross-check.
# BLKFLSBUF invalidates the bdev so every node is re-read and re-verified.
blockdev --flushbufs "$PERF_DEV" || { echo "GUARDS2 DONE FAIL:flushbufs"; exit 1; }
mkdir -p "$MNT"
mount -t briefs -o noatime,debug "$PERF_DEV" "$MNT" \
    || { echo "GUARDS2 DONE FAIL:mount"; exit 1; }
if ! dd if="$MNT/w34.bin" of=/dev/null bs=1M status=none; then
    echo "interop read: FAIL (dd nonzero)"
    echo "GUARDS2 DONE FAIL:interop-read"
    umount "$MNT"
    exit 1
fi
vstats=$(cat /sys/kernel/debug/briefs/*/stats 2>/dev/null | grep -E "verify_" | tr '\n' ' ')
echo "interop stats: $vstats"
if grep -qE "bad magic|checksum mismatch" <(dmesg); then
    echo "GUARDS2 DONE FAIL:checksum-mismatch"
    umount "$MNT"
    exit 1
fi
echo "interop read: PASS (no EIO, no checksum splats)"

# 3. probe replay
echo "=== 3. journal probe-replay ==="
reload
# repro-replay.sh hardcodes /dev/loop0 and expects it pre-attached
rm -f /tmp/guards2-loop.img
losetup -d /dev/loop0 2>/dev/null || true   # a prior aborted run may have left it attached
truncate -s 256M /tmp/guards2-loop.img
losetup /dev/loop0 /tmp/guards2-loop.img || { echo "GUARDS2 DONE FAIL:losetup"; exit 1; }
if bash /vagrant/tests/repro-replay.sh 2>&1 | tee /tmp/guards2-replay.log \
    | grep -qE "BUG:|Oops|Call Trace|replay FAIL|MOUNT1 FAIL"; then
    # NB: case-sensitive on purpose — this log's own section headers say
    # "oops"/"BUG" in prose and would trip a -i grep.
    echo "GUARDS2 DONE FAIL:replay"
    umount "$MNT"
    exit 1
fi
losetup -d /dev/loop0
rm -f /tmp/guards2-loop.img
echo "probe-replay: PASS"

# 4. fsck clean on the perf image
echo "=== 4. fsck after unmount ==="
# step 3's reload() already umounts $MNT; only umount if still mounted
mountpoint -q "$MNT" && { umount "$MNT" || { echo "GUARDS2 DONE FAIL:umount"; exit 1; }; }
fsckout=$("$FSCK" -n "$PERF_DEV" 2>&1)
echo "$fsckout" | tail -5
if echo "$fsckout" | grep -qiE "error|corrupt|mismatch"; then
    echo "GUARDS2 DONE FAIL:fsck"
    exit 1
fi
echo "GUARDS2 DONE PASS"