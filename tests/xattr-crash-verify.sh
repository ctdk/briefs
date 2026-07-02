#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# xattr-crash-verify.sh - post-reboot verification for xattr-crash-test.sh.
#
# Mounts the crash-test image, checks that both xattrs survived, then runs
# fsck.briefs and reports success or failure.
set -uo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BRIEFS_MODULE="${BRIEFS_MODULE:-/vagrant/briefs_fs.ko}"
[ -f "$BRIEFS_MODULE" ] || BRIEFS_MODULE="${PWD}/briefs_fs.ko"

IMG=/var/lib/xfstests-briefs/xattr-crash.img
MNT=/mnt/xattr-crash
LOG=/var/lib/xfstests-briefs/xattr-crash-verify.log

exec >"$LOG" 2>&1

echo "verify start $(date -Iseconds)"

sudo mkdir -p "$MNT"
sudo umount "$MNT" 2>/dev/null || true
sudo losetup -D 2>/dev/null || true

sudo modprobe -r fs-briefs 2>/dev/null || true
sudo rmmod briefs_fs 2>/dev/null || true
if [ -f "$BRIEFS_MODULE" ]; then
    sudo insmod "$BRIEFS_MODULE" || { echo "insmod $BRIEFS_MODULE failed"; exit 1; }
else
    sudo modprobe fs-briefs || { echo "modprobe fs-briefs failed"; exit 1; }
fi

sudo mount -t briefs -o loop "$IMG" "$MNT" || { echo "mount failed"; exit 1; }

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

sudo python3 - "$MNT/crashfile" <<'PY'
import sys, os
path = sys.argv[1]
small_ok = big_ok = False
try:
    small = os.getxattr(path, 'user.small')
    small_ok = (len(small) == 64 and small == b'S' * 64)
except OSError as e:
    print('small err', e.errno, e.strerror)
try:
    big = os.getxattr(path, 'user.big')
    big_ok = (len(big) == 60000 and big == b'V' * 60000)
except OSError as e:
    print('big err', e.errno, e.strerror)
print('small_ok', small_ok)
print('big_ok', big_ok)
PY

SMALL_OK=$(grep '^small_ok' "$LOG" | tail -1 | awk '{print $2}')
BIG_OK=$(grep '^big_ok' "$LOG" | tail -1 | awk '{print $2}')

[ "$SMALL_OK" = "True" ] && pass "user.small survived (64 bytes)" || fail "user.small missing/corrupt"
[ "$BIG_OK" = "True" ] && pass "user.big survived (60000 bytes)" || fail "user.big missing/corrupt"

sudo umount "$MNT"

if sudo fsck.briefs -y "$IMG" >/tmp/xattr-crash-fsck.out 2>&1; then
    pass "fsck.briefs reports no errors"
else
    fail "fsck.briefs reported errors (see /tmp/xattr-crash-fsck.out)"
fi

cat /tmp/xattr-crash-fsck.out

echo ""
echo "=== Summary: $PASS passed, $FAIL failed ==="
exit $FAIL
