#!/bin/bash
# Capture the D-state stack trace of the umount-under-error hang, writing to
# a reboot-persistent path (/root/wedge_trace.txt) so it survives the
# unavoidable post-wedge reboot. Run as root.
set -u
OUT=/root/wedge_trace.txt
: > "$OUT"
exec >>"$OUT" 2>&1
echo "=== wedge_trace $(date -u +%s) ==="
DEVSIZE_MB=1024; MNT=/mnt/wt; IMG=/root/wt.img; DM=wt
timeout 20 umount "$MNT" 2>/dev/null; dmsetup remove "$DM" 2>/dev/null
losetup -d /dev/loop3 2>/dev/null; rm -f "$IMG"
dd if=/dev/zero of="$IMG" bs=1M count="$DEVSIZE_MB" status=none
LO=$(losetup -f --show "$IMG"); sz=$(blockdev --getsz "$LO")
dmsetup create "$DM" --table "0 $sz linear $LO 0"
/go/bin/mkfs.briefs -f /dev/mapper/"$DM" >/dev/null 2>&1
mkdir -p "$MNT"; mount -t briefs /dev/mapper/"$DM" "$MNT"
mkdir "$MNT/fs"
/xfstests/ltp/fsstress -d "$MNT/fs" -n 999999 -p 4 -s 7 >/dev/null 2>&1 &
WP=$!
sleep 4
dmsetup suspend --nolockfs "$DM"; dmsetup load "$DM" --table "0 $sz error $LO 0"; dmsetup resume "$DM"
kill -9 "$WP" 2>/dev/null; pkill -9 -P "$WP" 2>/dev/null; wait "$WP" 2>/dev/null
echo "=== launching umount (expected to hang) ==="
umount "$MNT" &
UP=$!
sleep 3
echo "=== D-state procs ==="
ps -eo stat,pid,comm | grep -E '^[DR]'
echo "=== umount pid=$UP stack ==="
cat /proc/$UP/stack 2>/dev/null
echo "=== all briefs/mount/kworker D-state stacks ==="
for p in $(ps -eo stat,pid,comm | awk '/^[DR]/{print $2}'); do
  echo "--- pid $p ---"; cat /proc/$p/stack 2>/dev/null; cat /proc/$p/wchan 2>/dev/null; echo
done
echo "=== sysrq-T (if enabled) ==="
echo 1 > /proc/sys/kernel/sysrq 2>/dev/null; echo t > /proc/sysrq-trigger 2>/dev/null; sleep 1
dmesg | tail -60
echo "=== done (reboot VM to recover) ==="