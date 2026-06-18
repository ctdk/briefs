#!/bin/bash
# Phase-1 gate verification: a clean v0.9 image mounts; corrupting the
# minor_version or clearing the incompat bit is rejected at mount AND by fsck.
set -u
MK=/go/bin/mkfs.briefs
FSCK=/go/bin/fsck.briefs
IMG=/tmp/p1-gate-$$.img
MNT=/tmp/p1-gate-mnt-$$
SIZE=8192
PASS=0; FAIL=0
pass(){ echo "  PASS: $1"; PASS=$((PASS+1)); }
fail(){ echo "  FAIL: $1 ${2:-}"; FAIL=$((FAIL+1)); }

cleanup(){
  umount "$MNT" 2>/dev/null || true
  [ -n "${LOOP:-}" ] && losetup -d "$LOOP" 2>/dev/null || true
  rm -f "$IMG"; rmdir "$MNT" 2>/dev/null || true
  rmmod briefs_fs 2>/dev/null || true
}
trap cleanup EXIT

mkdir -p "$MNT"
# Module must be loaded for mount -t briefs to work (the test runner rmmod's on exit).
rmmod briefs_fs 2>/dev/null || true
insmod /vagrant/briefs_fs.ko || { echo "ABORT: cannot insmod"; exit 2; }
mkimg(){ "$MK" -s "$SIZE" "$IMG" >/dev/null 2>&1; }
attach(){ LOOP=$(losetup -f --show "$IMG"); }
detach(){ losetup -d "$LOOP" 2>/dev/null; LOOP=""; }

# baseline: clean v0.9 image mounts and fsck is clean
mkimg; attach
if mount -t briefs "$LOOP" "$MNT" 2>/dev/null; then pass "clean v0.9 mounts"; umount "$MNT"; else fail "clean v0.9 mounts"; fi
detach

# gate A: minor_version != 9 (corrupt offset 16, 8 bytes -> value 8)
mkimg; attach
dd if=/dev/zero of="$IMG" bs=1 count=8 seek=16 conv=notrunc 2>/dev/null
printf '\x08' | dd of="$IMG" bs=1 seek=16 conv=notrunc 2>/dev/null
if mount -t briefs "$LOOP" "$MNT" >/dev/null 2>&1; then fail "mount rejects minor=8" "(mounted!)"; umount "$MNT" 2>/dev/null; else pass "mount rejects minor=8"; fi
if "$FSCK" "$IMG" >/dev/null 2>&1; then fail "fsck rejects minor=8" "(passed!)"; else pass "fsck rejects minor=8"; fi
detach

# gate B: incompat bit cleared (offset 144, 8 bytes -> 0)
mkimg; attach
dd if=/dev/zero of="$IMG" bs=1 count=8 seek=144 conv=notrunc 2>/dev/null
if mount -t briefs "$LOOP" "$MNT" >/dev/null 2>&1; then fail "mount rejects no-incompat" "(mounted!)"; umount "$MNT" 2>/dev/null; else pass "mount rejects no-incompat bit"; fi
if "$FSCK" "$IMG" >/dev/null 2>&1; then fail "fsck rejects no-incompat" "(passed!)"; else pass "fsck rejects no-incompat bit"; fi
detach

# baseline again: re-mkfs clean and confirm mount + fsck clean
mkimg; attach
if mount -t briefs "$LOOP" "$MNT" 2>/dev/null; then pass "clean v0.9 mounts again"; umount "$MNT"; else fail "clean v0.9 mounts again"; fi
if "$FSCK" "$IMG" >/dev/null 2>&1; then pass "fsck clean on restored image"; else fail "fsck clean on restored image"; fi
detach

echo "=== Results ===  PASS: $PASS  FAIL: $FAIL"
[ "$FAIL" -eq 0 ] && exit 0 || exit 1