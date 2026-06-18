#!/bin/bash
# Stress test for the extent-tail cache + append fast-path (#1).
# Writes N non-contiguous 4 KB blocks (logical blocks 0,2,4,...,2N-2),
# forcing N single-block extents (well past the 8 inline slots). Verifies
# file size, per-block content, holes, fsck, and survival across remount.
# Usage: stress-noncontig.sh <N> [device]   (device defaults to /dev/vdc1)
set -u
N="${1:-400}"
DEV="${2:-/dev/vdc1}"
MNT=/mnt/stress
MKFS=/go/bin/mkfs.briefs
FSCK=/go/bin/fsck.briefs
MOD=/vagrant/briefs_fs.ko

fail() { echo "FAIL: $*"; exit 1; }

# Fresh module + fs + mount
lsmod | grep -q briefs_fs || insmod "$MOD" || fail "insmod"
umount "$MNT" 2>/dev/null
umount "$DEV" 2>/dev/null
mount | grep -q "$DEV" && umount "$DEV"
mkfs.briefs -f "$DEV" >/dev/null 2>&1 || "$MKFS" "$DEV" >/dev/null 2>&1 || fail "mkfs"
mkdir -p "$MNT"
mount -t briefs "$DEV" "$MNT" || fail "mount"
echo "==> N=$N fresh mount OK"

python3 - "$MNT/stress.dat" "$N" <<'PY' || fail "python write/verify"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
BS = 4096
# Write N non-contiguous 4KB blocks at logical block 0,2,4,...,2N-2
for i in range(N):
    blk = 2*i
    pat = bytes([(i % 251) + 1]) * BS
    os.pwrite(fd, pat, blk*BS)
os.fsync(fd)
# Verify file size: highest block = 2N-2, so size = (2N-2+1)*BS = (2N-1)*BS
st = os.fstat(fd)
expect = (2*N - 1) * BS
if st.st_size != expect:
    print(f"SIZE_BAD: got {st.st_size} expect {expect}")
    sys.exit(1)
# Verify written blocks hold their pattern and odd blocks are holes (zeros)
for i in range(N):
    blk = 2*i
    want = bytes([(i % 251) + 1]) * BS
    got = os.pread(fd, BS, blk*BS)
    if got != want:
        print(f"DATA_BAD at block {blk} (ext {i})")
        sys.exit(1)
    # odd block (hole) should read zeros
    if 2*i+1 < 2*N-1:
        hole = os.pread(fd, BS, (2*i+1)*BS)
        if hole != bytes(BS):
            print(f"HOLE_BAD at block {2*i+1}: non-zero in hole")
            sys.exit(1)
os.close(fd)
print("WRITE+VERIFY OK")
PY

sync
echo "==> fsck after unmount"
umount "$MNT" || fail "umount"
fsck_out="$("$FSCK" "$DEV" 2>&1 | tr -d '\0')"; fsck_rc=$?
echo "$fsck_out" | tail -5
# Real error signatures (the 0a regression was "superblock free ... mismatch").
# Benign "WARNING: checkpoint free_count differs from superblock" and lines
# like "no overlapping extents found" / "no orphaned inodes found" are NOT errors.
if echo "$fsck_out" | grep -a -iE "mismatch|corrupt|ERROR:"; then
  echo "FSCK ERRORS DETECTED"; fail "fsck"
fi
[ "$fsck_rc" -ne 0 ] && fail "fsck exit $fsck_rc"
echo "FSCK CLEAN (exit $fsck_rc)"

# Remount and verify extents survive
mount -t briefs "$DEV" "$MNT" || fail "remount"
python3 - "$MNT/stress.dat" "$N" <<'PY' || fail "python re-verify after remount"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDONLY)
BS = 4096
st = os.fstat(fd)
expect = (2*N - 1) * BS
if st.st_size != expect:
    print(f"REMOUNT_SIZE_BAD: {st.st_size} != {expect}")
    sys.exit(1)
for i in range(N):
    blk = 2*i
    want = bytes([(i % 251) + 1]) * BS
    got = os.pread(fd, BS, blk*BS)
    if got != want:
        print(f"REMOUNT_DATA_BAD at block {blk}")
        sys.exit(1)
os.close(fd)
print("REMOUNT VERIFY OK")
PY

# dmesg check
dmesg | tail -20 | grep -a -i "briefs" | grep -a -iE "error|warn|bug|oops|null" && fail "dmesg errors" || true
umount "$MNT" 2>/dev/null
echo "==> N=$N ALL PASS"