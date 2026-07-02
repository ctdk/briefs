#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# xattr-crash-test.sh - deliberate crash-consistency test for chained xattrs.
#
# Set a small inline xattr and a large chained xattr, sync, then hard-reset the
# VM via sysrq-b before a clean unmount. After reboot, run
# xattr-crash-verify.sh to verify both values are intact and fsck reports no
# errors.
#
# Use the BRIEFS_MODULE environment variable to point at the briefs_fs.ko to
# load; otherwise defaults to /vagrant/briefs_fs.ko (the build target for
# `make test`) and falls back to the module in the current directory.
set -uo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

BRIEFS_MODULE="${BRIEFS_MODULE:-/vagrant/briefs_fs.ko}"
[ -f "$BRIEFS_MODULE" ] || BRIEFS_MODULE="${PWD}/briefs_fs.ko"

IMG=/var/lib/xfstests-briefs/xattr-crash.img
MNT=/mnt/xattr-crash
LOG=/var/lib/xfstests-briefs/xattr-crash.log

exec >"$LOG" 2>&1

echo "start $(date -Iseconds)"

sudo rm -f "$IMG"
sudo mkdir -p "$MNT" /var/lib/xfstests-briefs

sudo mkfs.briefs -s 8192 "$IMG" >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }

sudo modprobe -r fs-briefs 2>/dev/null || true
if [ -f "$BRIEFS_MODULE" ]; then
    sudo insmod "$BRIEFS_MODULE" || { echo "insmod $BRIEFS_MODULE failed"; exit 1; }
else
    sudo modprobe fs-briefs || { echo "modprobe fs-briefs failed"; exit 1; }
fi

sudo mount -t briefs -o loop "$IMG" "$MNT" || { echo "mount failed"; exit 1; }
sudo touch "$MNT/crashfile"

sudo python3 - "$MNT/crashfile" <<'PY'
import sys, os
path = sys.argv[1]
os.setxattr(path, b'user.small', b'S' * 64)
os.setxattr(path, b'user.big', b'V' * 60000)
print('xattrs set ok')
PY

sync
LOOP_DEV=$(losetup -j "$IMG" | head -1 | cut -d: -f1)
if [ -n "$LOOP_DEV" ]; then
    sudo blockdev --flushbufs "$LOOP_DEV" 2>/dev/null || true
fi
sync

echo "sync done $(date -Iseconds)"

# Schedule a hard reset in 10 seconds. Use setsid so the reset subshell is
# fully detached from this SSH/session process group and survives any disconnect
# that happens before the reset.
setsid bash -c "sleep 10; echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger" >/dev/null 2>&1 &
disown

echo "reset scheduled; reboot, then run tests/xattr-crash-verify.sh"
