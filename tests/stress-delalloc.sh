#!/bin/bash
# Stress test for #6 Step 2: delayed allocation.
# write_begin now defers (BH_Delay); allocation happens at writeback via
# briefs_get_block (mpage / __block_write_full_folio).  Covers:
#   (A) sequential full-block writes -> per-block alloc at writeback coalesces
#       via the merge logic into ONE extent; verify size, per-block content,
#       fiemap extent count, fsck, remount.
#   (B) partial sub-block writes -> the custom write_begin must zero the
#       unwritten head/tail of a delayed block (the kernel skips this for
#       BH_Delay); verify bytes outside the write read back as zero.
#   (C) random overwrites of already-written blocks -> the "already mapped"
#       path in briefs_get_block_write (map_bh, no re-alloc, no double-extent).
#   (D) sparse non-contiguous writes -> holes read zero, written blocks have
#       data; per-block alloc at writeback with NO merge (gaps).
# Usage: stress-delalloc.sh [device]   (device defaults to /dev/vdc1)
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
	if echo "$out" | grep -a -iE "mismatch|corrupt|ERROR:"; then
		echo "FSCK ERRORS DETECTED"; fail "fsck"
	fi
	[ "$rc" -ne 0 ] && fail "fsck exit $rc"
	echo "FSCK CLEAN (exit $rc)"
	mount -t briefs "$DEV" "$MNT" || fail "remount"
}

# ---------- Phase A: sequential full-block writes (merge -> 1 extent) ----
fresh_mount
echo "==> Phase A: sequential full-block writes (delalloc + merge)"
NA=300
python3 - "$MNT/seq.dat" "$NA" <<'PY' || fail "seq write"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
for i in range(N):
    pat = bytes([(i % 251) + 1]) * BS
    os.pwrite(fd, pat, i*BS)
os.fsync(fd)
os.close(fd)
print("SEQ WRITE OK")
PY
python3 - "$MNT/seq.dat" "$NA" <<'PY' || fail "seq verify"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDONLY)
st = os.fstat(fd)
if st.st_size != N*BS:
    print(f"SIZE_BAD: {st.st_size} != {N*BS}"); sys.exit(1)
for i in range(N):
    want = bytes([(i % 251) + 1]) * BS
    if os.pread(fd, BS, i*BS) != want:
        print(f"DATA_BAD at block {i}"); sys.exit(1)
os.close(fd)
print("SEQ VERIFY OK")
PY
echo "  fiemap (expect 1 extent for sequential writes):"
filefrag -v "$MNT/seq.dat" 2>/dev/null | tail -4 || true
fsck_clean
python3 - "$MNT/seq.dat" "$NA" <<'PY' || fail "seq remount verify"
import sys, os
path, N = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDONLY)
if os.fstat(fd).st_size != N*BS:
    print("REMOUNT_SIZE_BAD"); sys.exit(1)
for i in range(N):
    want = bytes([(i % 251) + 1]) * BS
    if os.pread(fd, BS, i*BS) != want:
        print(f"REMOUNT_DATA_BAD at block {i}"); sys.exit(1)
os.close(fd)
print("SEQ REMOUNT VERIFY OK")
PY
umount "$MNT" 2>/dev/null

# ---------- Phase B: partial sub-block writes (zero tails) --------------
fresh_mount
echo "==> Phase B: partial sub-block writes (zero tails)"
python3 - "$MNT/part.dat" <<'PY' || fail "partial write"
import sys, os
path = sys.argv[1]
BS = 4096
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
# 100 bytes at offset 0 -> fits inline (<=256), stored in the inode.
os.pwrite(fd, bytes([0xA5])*100, 0)
# 50 bytes at offset 8192+200 -> exceeds inline, promotes the inode to
# extent-backed, then writes block 2 [200,250).  The custom write_begin must
# zero the unwritten head [0,200) and tail [250,4096) of this delayed block.
os.pwrite(fd, bytes([0x5A])*50, 8192+200)
# 100 bytes at offset 12288 -> block 3 [0,100): a second fresh delayed block
# whose tail [100,4096) must also be zeroed.
os.pwrite(fd, bytes([0xC3])*100, 12288)
# A FULL block 4 at offset 16384 pushes i_size to 20480 so the tails of
# blocks 2 and 3 (ending at 12288 and 16384) are within EOF and readable --
# reads stop at i_size, so a partial-write block's own tail is otherwise
# beyond EOF and unverifiable.
os.pwrite(fd, bytes([0xEE])*BS, 16384)
os.fsync(fd)
os.close(fd)
print("PARTIAL WRITE OK")
PY
python3 - "$MNT/part.dat" <<'PY' || fail "partial verify"
import sys, os
path = sys.argv[1]
BS = 4096
fd = os.open(path, os.O_RDONLY)
st = os.fstat(fd)
if st.st_size != 20480:
    print(f"PARTIAL SIZE_BAD: {st.st_size} != 20480"); sys.exit(1)
# block 0: promoted inline -> [0,100)=0xA5, [100,4096)=0
b0 = os.pread(fd, BS, 0)
if b0[:100] != bytes([0xA5])*100:
    print("PARTIAL DATA_BAD block0 head"); sys.exit(1)
if b0[100:] != bytes(BS-100):
    print("PARTIAL ZERO_BAD block0 tail (stale bytes leaked!)"); sys.exit(1)
# block 1: never written -> all zero (hole)
b1 = os.pread(fd, BS, 4096)
if b1 != bytes(BS):
    print("PARTIAL ZERO_BAD block1 (hole)"); sys.exit(1)
# block 2: [0,200)=0, [200,250)=0x5A, [250,4096)=0  (zeroed tail within EOF)
b2 = os.pread(fd, BS, 8192)
if b2[:200] != bytes(200):
    print("PARTIAL ZERO_BAD block2 head"); sys.exit(1)
if b2[200:250] != bytes([0x5A])*50:
    print("PARTIAL DATA_BAD block2 mid"); sys.exit(1)
if b2[250:] != bytes(BS-250):
    print("PARTIAL ZERO_BAD block2 tail (stale bytes leaked!)"); sys.exit(1)
# block 3: [0,100)=0xC3, [100,4096)=0
b3 = os.pread(fd, BS, 12288)
if b3[:100] != bytes([0xC3])*100:
    print("PARTIAL DATA_BAD block3 head"); sys.exit(1)
if b3[100:] != bytes(BS-100):
    print("PARTIAL ZERO_BAD block3 tail"); sys.exit(1)
# block 4: full 0xEE
b4 = os.pread(fd, BS, 16384)
if b4 != bytes([0xEE])*BS:
    print("PARTIAL DATA_BAD block4"); sys.exit(1)
os.close(fd)
print("PARTIAL VERIFY OK (zero tails correct)")
PY
fsck_clean
umount "$MNT" 2>/dev/null

# ---------- Phase C: random overwrites of written blocks ----------------
fresh_mount
echo "==> Phase C: random overwrites (already-mapped path)"
NC=400
python3 - "$MNT/ow.dat" "$NC" <<'PY' || fail "overwrite"
import sys, os, random
path, N = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
# initial fill
for i in range(N):
    os.pwrite(fd, bytes([(i % 251) + 1]) * BS, i*BS)
os.fsync(fd)
# overwrite ~half the blocks with a distinct pattern (random order)
rng = random.Random(12345)
targets = rng.sample(range(N), N//2)
for i in targets:
    os.pwrite(fd, bytes([0xE0 + (i % 16)]) * BS, i*BS)
os.fsync(fd)
os.close(fd)
print("OVERWRITE WRITE OK")
PY
python3 - "$MNT/ow.dat" "$NC" <<'PY' || fail "overwrite verify"
import sys, os, random
path, N = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDONLY)
rng = random.Random(12345)
overwritten = set(rng.sample(range(N), N//2))
for i in range(N):
    if i in overwritten:
        want = bytes([0xE0 + (i % 16)]) * BS
    else:
        want = bytes([(i % 251) + 1]) * BS
    if os.pread(fd, BS, i*BS) != want:
        print(f"OW DATA_BAD at block {i}"); sys.exit(1)
os.close(fd)
print("OVERWRITE VERIFY OK")
PY
fsck_clean
umount "$MNT" 2>/dev/null

# ---------- Phase D: sparse non-contiguous writes (no merge) ------------
fresh_mount
echo "==> Phase D: sparse non-contiguous writes (holes, no merge)"
ND=200
python3 - "$MNT/sparse.dat" "$ND" <<'PY' || fail "sparse write"
import sys, os
path, M = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
# write at even blocks 0,2,...,2M-2 ; odd blocks are holes
for i in range(M):
    os.pwrite(fd, bytes([(i % 251) + 1]) * BS, (2*i)*BS)
os.fsync(fd)
os.close(fd)
print("SPARSE WRITE OK")
PY
python3 - "$MNT/sparse.dat" "$ND" <<'PY' || fail "sparse verify"
import sys, os
path, M = sys.argv[1], int(sys.argv[2])
BS = 4096
fd = os.open(path, os.O_RDONLY)
# Writes at even blocks 0,2,...,2M-2 -> i_size is the end of the last even
# block, (2M-1)*BS (the trailing odd block 2M-1 is beyond EOF, not a hole).
expect = (2*M - 1) * BS
if os.fstat(fd).st_size != expect:
    print(f"SPARSE SIZE_BAD: {os.fstat(fd).st_size} != {expect}"); sys.exit(1)
for i in range(M):
    want = bytes([(i % 251) + 1]) * BS
    if os.pread(fd, BS, (2*i)*BS) != want:
        print(f"SPARSE DATA_BAD at block {2*i}"); sys.exit(1)
# odd blocks 1,3,...,2M-3 are holes within EOF; block 2M-1 is beyond EOF
for i in range(M - 1):
    if os.pread(fd, BS, (2*i+1)*BS) != bytes(BS):
        print(f"SPARSE ZERO_BAD at block {2*i+1} (hole)"); sys.exit(1)
os.close(fd)
print("SPARSE VERIFY OK")
PY
fsck_clean
umount "$MNT" 2>/dev/null

# dmesg check: delalloc bugs surface as these signatures
dmesg | tail -40 | grep -a -i "briefs" | grep -a -iE "error|warn|bug|oops|null|out of range|lockup|returned 0 despite" && fail "dmesg errors" || true
echo "==> ALL PASS (phases A+B+C+D)"