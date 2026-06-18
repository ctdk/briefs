#!/bin/bash
# Stress test for the multi-block allocator + fallocate wiring (#4).
# The 97/97 suite only exercises FALLOC_FL_PUNCH_HOLE, never plain mode-0
# pre-allocation.  This test covers both code paths added in #4:
#   (A) contiguous pre-alloc  -> briefs_alloc_blocks(run_len>1) + one len=k
#       extent append (incl. word-spanning runs N=65 and large N=10000).
#   (B) interleaved pre-alloc -> run_len==1 everywhere, exercising the
#       per-block fallback path (scattered writes first, then fallocate the
#       gaps).  Verifies size, all-zero read-back, fsck, and remount survival.
#   (C) two contiguous fallocate calls -> the second run MERGES into the last
#       extent (len += ext->len, the #6-Step-1 fix; the old len++ corrupted
#       the on-disk length for len>1).  Verifies size, zeros, fsck, remount.
# Usage: stress-fallocate.sh [device]   (device defaults to /dev/vdc1)
set -u
DEV="${1:-/dev/vdc1}"
MNT=/mnt/stress
MKFS=/go/bin/mkfs.briefs
FSCK=/go/bin/fsck.briefs
MOD=/vagrant/briefs_fs.ko
BS=4096

fail() { echo "FAIL: $*"; exit 1; }

fresh_mount() {
	umount "$MNT" 2>/dev/null
	umount "$DEV" 2>/dev/null
	mount | grep -q "$DEV" && umount "$DEV"
	# Force-reload the freshly built module: a stale module from a prior
	# session would silently skip insmod (lsmod grep) and test old code.
	rmmod briefs_fs 2>/dev/null
	insmod "$MOD" || fail "insmod"
	mkfs.briefs -f "$DEV" >/dev/null 2>&1 || "$MKFS" "$DEV" >/dev/null 2>&1 || fail "mkfs"
	mkdir -p "$MNT"
	mount -t briefs "$DEV" "$MNT" || fail "mount"
}

fsck_clean() {
	echo "==> fsck after unmount"
	umount "$MNT" || fail "umount"
	local out rc
	out="$("$FSCK" "$DEV" 2>&1 | tr -d '\0')"; rc=$?
	echo "$out" | tail -5
	# Real error signatures (the 0a regression was "superblock free ... mismatch").
	# Benign "checkpoint free_count differs from superblock" / "no overlapping
	# extents found" / "no orphaned inodes found" are NOT errors.
	if echo "$out" | grep -a -iE "mismatch|corrupt|ERROR:"; then
		echo "FSCK ERRORS DETECTED"; fail "fsck"
	fi
	[ "$rc" -ne 0 ] && fail "fsck exit $rc"
	echo "FSCK CLEAN (exit $rc)"
	mount -t briefs "$DEV" "$MNT" || fail "remount"
}

# ---------- Phase A: contiguous pre-alloc, several sizes ---------------
fresh_mount
echo "==> Phase A: contiguous pre-alloc (multi-block path)"
for N in 1 64 65 1000 10000; do
	f="$MNT/pre_$N.dat"
	fallocate -l $((N*BS)) "$f" || fail "fallocate N=$N"
	python3 - "$f" "$N" <<'PY' || fail "verify N=$N"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDONLY)
BS = 4096
st = os.fstat(fd)
if st.st_size != N*BS:
    print(f"SIZE_BAD N={N}: {st.st_size} != {N*BS}"); sys.exit(1)
# pre-allocated blocks must read back as zeros
buf = os.pread(fd, min(N, 64)*BS, 0)
if buf != bytes(len(buf)):
    print(f"ZERO_BAD N={N}: non-zero in prealloc region"); sys.exit(1)
# spot-check the tail too
if N > 64:
    tail = os.pread(fd, BS, (N-1)*BS)
    if tail != bytes(BS):
        print(f"ZERO_BAD N={N} tail: non-zero"); sys.exit(1)
os.close(fd)
print(f"N={N} size+zeros OK")
PY
done
fsck_clean
# Re-verify all sizes survived the remount
for N in 1 64 65 1000 10000; do
	python3 - "$MNT/pre_$N.dat" "$N" <<'PY' || fail "re-verify N=$N"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDONLY)
BS = 4096
st = os.fstat(fd)
if st.st_size != N*BS:
    print(f"REMOUNT_SIZE_BAD N={N}: {st.st_size} != {N*BS}"); sys.exit(1)
buf = os.pread(fd, min(N,64)*BS, 0)
if buf != bytes(len(buf)):
    print(f"REMOUNT_ZERO_BAD N={N}"); sys.exit(1)
os.close(fd)
PY
done
echo "==> Phase A remount re-verify OK"
umount "$MNT" 2>/dev/null

# ---------- Phase B: interleaved pre-alloc (per-block fallback) ---------
fresh_mount
echo "==> Phase B: interleaved pre-alloc (fallback path)"
# write 100 scattered single blocks at EVEN logical positions (100 mapped),
# then fallocate the whole 200-block range so the ODD gaps are run_len==1 and
# take the per-block fallback path.
python3 - "$MNT/inter.dat" 100 <<'PY' || fail "interleaved write"
import sys, os
path, M = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
# write data at even blocks 0,2,...,2M-2 (leaving odd blocks as holes)
for i in range(M):
    pat = bytes([(i % 251) + 1]) * BS
    os.pwrite(fd, pat, (2*i)*BS)
os.fsync(fd)
os.close(fd)
print("INTERLEAVED WRITE OK")
PY
# pre-allocate the full 2M-block range; odd blocks are unmapped runs of 1 and
# take the per-block fallback path (briefs_alloc_blocks not used for run_len==1).
INTER_M=100
fallocate -l $((INTER_M*2*BS)) "$MNT/inter.dat" || fail "fallocate interleaved"
sync
python3 - "$MNT/inter.dat" "$INTER_M" <<'PY' || fail "interleaved verify"
import sys, os
path, M = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDONLY)
BS = 4096
st = os.fstat(fd)
if st.st_size != 2*M*BS:
    print(f"SIZE_BAD: {st.st_size} != {2*M*BS}"); sys.exit(1)
# even blocks keep their pattern; odd blocks (the fallocated gaps) are zeros
for i in range(M):
    want = bytes([(i % 251) + 1]) * BS
    got = os.pread(fd, BS, (2*i)*BS)
    if got != want:
        print(f"DATA_BAD at block {2*i}"); sys.exit(1)
    zero = os.pread(fd, BS, (2*i+1)*BS)
    if zero != bytes(BS):
        print(f"ZERO_BAD at block {2*i+1} (fallocated gap)"); sys.exit(1)
os.close(fd)
print("INTERLEAVED VERIFY OK")
PY
fsck_clean
# re-verify interleaved after remount
python3 - "$MNT/inter.dat" 100 <<'PY' || fail "interleaved re-verify"
import sys, os
path, M = sys.argv[1], int(sys.argv[2])
fd = os.open(path, os.O_RDONLY)
BS = 4096
st = os.fstat(fd)
if st.st_size != 2*M*BS:
    print(f"REMOUNT_SIZE_BAD: {st.st_size} != {2*M*BS}"); sys.exit(1)
for i in range(M):
    want = bytes([(i % 251) + 1]) * BS
    if os.pread(fd, BS, (2*i)*BS) != want:
        print(f"REMOUNT_DATA_BAD at block {2*i}"); sys.exit(1)
    if os.pread(fd, BS, (2*i+1)*BS) != bytes(BS):
        print(f"REMOUNT_ZERO_BAD at block {2*i+1}"); sys.exit(1)
os.close(fd)
print("INTERLEAVED REMOUNT VERIFY OK")
PY
umount "$MNT" 2>/dev/null

# ---------- Phase C: two contiguous fallocate calls (run-merge path) ----
# A second fallocate call adjacent to the first produces a multi-block run
# extent that MERGES into the last extent (logically + physically contiguous
# on a fresh fs).  Before the #6-Step-1 fix __briefs_append_extent grew the
# last extent by 1 (len++) instead of ext->len, so the on-disk extent claimed
# N+1 blocks while N+M were allocated -> fsck free-count / leak mismatch.
fresh_mount
echo "==> Phase C: two contiguous fallocate calls (run-merge, len += ext->len)"
for pair in "64 65" "100 200" "1 4096"; do
	set -- $pair; N=$1; M=$2
	f="$MNT/merge_${N}_${M}.dat"
	fallocate -l $((N*BS)) "$f" || fail "fallocate1 N=$N M=$M"
	# adjacent run at offset N*BS -> blocks N..N+M-1, allocated as one run
	# that merges into the last extent (phys contiguous on fresh fs).
	fallocate -o $((N*BS)) -l $((M*BS)) "$f" || fail "fallocate2 N=$N M=$M"
	python3 - "$f" "$N" "$M" <<'PY' || fail "verify merge N=$N M=$M"
import sys, os
path, N, M = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BS = 4096
fd = os.open(path, os.O_RDONLY)
st = os.fstat(fd)
if st.st_size != (N+M)*BS:
    print(f"SIZE_BAD N={N} M={M}: {st.st_size} != {(N+M)*BS}"); sys.exit(1)
# whole range must read back as zeros (pre-allocated)
tot = min(N+M, 256)
buf = os.pread(fd, tot*BS, 0)
if buf != bytes(len(buf)):
    print(f"ZERO_BAD N={N} M={M}: non-zero in prealloc region"); sys.exit(1)
# tail spot-check
tail = os.pread(fd, BS, (N+M-1)*BS)
if tail != bytes(BS):
    print(f"ZERO_BAD N={N} M={M} tail"); sys.exit(1)
os.close(fd)
print(f"merge N={N} M={M} size+zeros OK")
PY
done
fsck_clean
# re-verify after remount: the merged extent must still cover N+M blocks
for pair in "64 65" "100 200" "1 4096"; do
	set -- $pair; N=$1; M=$2
	python3 - "$MNT/merge_${N}_${M}.dat" "$N" "$M" <<'PY' || fail "re-verify merge N=$N M=$M"
import sys, os
path, N, M = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
BS = 4096
fd = os.open(path, os.O_RDONLY)
st = os.fstat(fd)
if st.st_size != (N+M)*BS:
    print(f"REMOUNT_SIZE_BAD N={N} M={M}: {st.st_size} != {(N+M)*BS}"); sys.exit(1)
tot = min(N+M, 256)
if os.pread(fd, tot*BS, 0) != bytes(tot*BS):
    print(f"REMOUNT_ZERO_BAD N={N} M={M}"); sys.exit(1)
os.close(fd)
PY
done
echo "==> Phase C remount re-verify OK"
umount "$MNT" 2>/dev/null

# dmesg check: rover/run-search bugs surface as these signatures
dmesg | tail -30 | grep -a -i "briefs" | grep -a -iE "error|warn|bug|oops|null|out of range" && fail "dmesg errors" || true
echo "==> ALL PASS (phases A+B+C)"