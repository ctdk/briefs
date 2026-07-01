#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# xattr-crash-test.sh - deliberate crash-consistency test for chained xattrs.
# Set a small inline xattr and a large chained xattr, sync, then hard-reset the
# VM via sysrq-b before a clean unmount. After reboot, remount and verify both
# values are intact and fsck reports no errors.
set -uo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

IMG=/var/lib/xfstests-briefs/xattr-crash.img
MNT=/mnt/xattr-crash
PASSFILE=/tmp/xattr-crash.pass

cleanup_mounts() {
    umount "$MNT" 2>/dev/null || true
    losetup -D 2>/dev/null || true
}

# Phase 1: fresh image + mount + set xattrs
sudo rm -f "$IMG"
sudo mkdir -p "$MNT"

cleanup_mounts
sudo mkfs.briefs -s 8192 "$IMG" >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }

sudo modprobe -r fs-briefs 2>/dev/null || true
sudo modprobe fs-briefs || { echo "modprobe failed"; exit 1; }

sudo mount -t briefs -o loop "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
sudo touch "$MNT/crashfile"

sudo python3 - "$MNT/crashfile" 64 user.small <<'PY'
import sys, os
os.setxattr(sys.argv[1], b'user.small', b'S' * 64)
PY

sudo python3 - "$MNT/crashfile" 60000 user.big <<'PY'
import sys, os
os.setxattr(sys.argv[1], b'user.big', b'V' * 60000)
PY

sync

# Record what we expect on the next boot, then reset before clean unmount.
sudo python3 - "$MNT/crashfile" <<'PY'
import sys, os
path = sys.argv[1]
small = os.getxattr(path, 'user.small')
big   = os.getxattr(path, 'user.big')
with open('/tmp/xattr-crash.expected', 'wb') as f:
    f.write(small + b'\n' + big)
PY

echo "xattrs written; scheduling hard reset in 5s"
(
    sleep 5
    echo 1 > /proc/sys/kernel/sysrq
    echo b > /proc/sysrq-trigger
) >/dev/null 2>&1 &
echo "reset_scheduled"
