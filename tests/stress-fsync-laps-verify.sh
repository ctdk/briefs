#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# stress-fsync-laps-verify.sh - post-reboot verification for
# stress-fsync-laps.sh (see the header there for what this catches).
#
# Gate order matters: the crashed image is mounted FIRST so the kernel's
# journal replay runs on the raw post-crash state (an fsck -y before the
# mount would repair the stale allocator bitmaps and mask exactly the
# hole-A/B shape this test exists to catch: an empty replay window plus
# a stale bitmap only corrupts when the post-crash allocator hands out
# blocks the bitmap thinks are free).
#
#   1. Mount succeeds (replay of the surviving window must not wedge or
#      fail) and every fsynced record reads back with its pattern: the
#      fsync durability contract held across the crash.
#   2. MORE records are written on the post-crash mount -- the
#      stale-bitmap corruption manifests here (generic/040/041 family:
#      reallocated blocks get zeroed, earlier records vanish).
#   3. fsck after clean unmount is clean (the pre-repair fsck of the raw
#      crashed state legitimately reports stale-bitmap deltas between
#      checkpoints -- that is what replay is for -- so fsck runs last,
#      on the state the kernel actually left).
#
# Usage (inside the VM, as root, after the reboot):
#   tests/stress-fsync-laps-verify.sh [extra_pairs]
#     extra_pairs  records to add in the post-crash phase; default 200.
set -uo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

EXTRA="${1:-200}"

BRIEFS_MODULE="${BRIEFS_MODULE:-/vagrant/briefs_fs.ko}"
[ -f "$BRIEFS_MODULE" ] || BRIEFS_MODULE="${PWD}/briefs_fs.ko"

IMG=/var/lib/xfstests-briefs/fsync-laps.img
MNT=/mnt/fsync-laps
LOG=/var/lib/xfstests-briefs/fsync-laps-verify.log
STATE=/var/lib/xfstests-briefs/fsync-laps.state

mkdir -p /var/lib/xfstests-briefs "$MNT"
exec >"$LOG" 2>&1

echo "verify start $(date -Iseconds)"

PAIRS=$(sed -n 's/^pairs=//p' "$STATE")
REC_KIB=$(sed -n 's/^rec_kib=//p' "$STATE")
[ -n "$PAIRS" ] && [ -n "$REC_KIB" ] || { echo "no state file; run stress-fsync-laps.sh first"; exit 1; }
echo "expecting pairs=$PAIRS rec_kib=$REC_KIB extra=$EXTRA"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# Records [0, count) must each read back as (i*7+13)&0xff fill.
verify_records() {
	local count=$1
	python3 - "$MNT/laps.dat" "$count" "$REC_KIB" <<'PY'
import sys, os
path, count, kib = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
recsz = kib * 1024
size = os.stat(path).st_size
if size != count * recsz:
    print("SIZE-MISMATCH size=%d expected=%d" % (size, count * recsz))
    sys.exit(1)
fd = os.open(path, os.O_RDONLY)
for i in range(count):
    buf = os.pread(fd, recsz, i * recsz)
    if len(buf) != recsz or buf != bytes([(i * 7 + 13) & 0xff]) * recsz:
        print("RECORD-BAD index=%d len=%d" % (i, len(buf)))
        sys.exit(1)
os.close(fd)
print("RECORDS-OK count=%d" % count)
PY
}

umount "$MNT" 2>/dev/null || true
losetup -D 2>/dev/null || true

rmmod briefs_fs 2>/dev/null || true
insmod "$BRIEFS_MODULE" || { echo "insmod $BRIEFS_MODULE failed"; exit 1; }

echo "==> gate 0: fsck -n on the raw crashed image (diagnostics only)"
fsck.briefs -n "$IMG" >/tmp/fsync-laps-fsck-raw.out 2>&1 || true
grep -aE "journal records|free count|mismatch|FSCK" /tmp/fsync-laps-fsck-raw.out | head -10

echo "==> gate 1: mount raw crashed image (kernel replay) + record readback"
mount -t briefs -o debug,loop "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
LOOP=$(losetup -j "$IMG" | head -1 | cut -d: -f1)
STATS="/sys/kernel/debug/briefs/${LOOP#/dev/}/stats"
echo "replay stats: $(grep -E 'journal_replay_records|journal_checkpoints' "$STATS" 2>/dev/null | tr '\n' ' ')"
if mountpoint -q "$MNT"; then
	pass "mount after crash (replay ran)"
else
	fail "mount after crash (replay failed)"
	echo "=== Summary: $PASS passed, $FAIL failed ==="
	exit $((FAIL + 1))
fi

if verify_records "$PAIRS"; then
	pass "all $PAIRS fsynced records intact"
else
	fail "record content/size mismatch on crashed image"
fi

echo "==> gate 2: post-crash allocation pressure + re-verify"
# Phase A: fsynced appends (the normal workload shape).
python3 - "$MNT/laps.dat" "$PAIRS" "$EXTRA" "$REC_KIB" <<'PY'
import sys, os
path, pairs, extra, kib = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
recsz = kib * 1024
fd = os.open(path, os.O_WRONLY | os.O_APPEND)
for j in range(extra):
    i = pairs + j
    os.pwrite(fd, bytes([(i * 7 + 13) & 0xff]) * recsz, i * recsz)
    os.fsync(fd)
os.close(fd)
print("post-crash phase wrote", extra, "records")
PY
# Phase B: filler until the allocator runs out.  The stale-bitmap shape
# this test hunts has live blocks that the on-disk bitmap still marks
# free; the corruption only manifests when the allocator is pushed past
# the genuinely free tail into that phantom-free region.  A big
# buffered write fills the tail and then MUST reuse live blocks (the
# bitmap claims they are free) -- ENOSPC never arrives or arrives too
# late, and the next record readback catches the overwrite.  On a
# consistent bitmap this phase just hits a clean ENOSPC.  fsync the
# filler before removing it: allocation happens at writeback (delalloc),
# so dropping it first would cancel the writes and allocate nothing.
python3 - "$MNT/filler.dat" <<'PY'
import sys, os
path = sys.argv[1]
chunk = b"\xf0" * (1 << 20)
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
written = 0
try:
    while written < 1100:
        os.write(fd, chunk)
        written += 1
except OSError as e:
    print("filler stopped after %d MiB: %s" % (written, e.strerror))
try:
    os.fsync(fd)
except OSError as e:
    print("filler fsync: %s" % e.strerror)
os.close(fd)
print("filler wrote+fsynced %d MiB" % written)
PY
rm -f "$MNT/filler.dat"
if verify_records "$((PAIRS + EXTRA))"; then
	pass "all $((PAIRS + EXTRA)) records intact after post-crash allocations"
else
	fail "records lost/corrupted by post-crash allocations (stale-bitmap re-alloc)"
fi

umount "$MNT" || { echo "umount failed"; exit 1; }

echo "==> gate 3: fsck after clean unmount"
if fsck.briefs -y "$IMG" >/tmp/fsync-laps-fsck.out 2>&1; then
	pass "fsck clean after post-crash unmount"
else
	fail "fsck reported errors after post-crash unmount (see /tmp/fsync-laps-fsck.out)"
fi
grep -aiE "mismatch|corrupt|error" /tmp/fsync-laps-fsck.out | head -10

echo ""
echo "=== Summary: $PASS passed, $FAIL failed ==="
exit $FAIL