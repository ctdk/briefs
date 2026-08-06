#!/bin/bash
exec >/tmp/blockdev-fuse.out 2>&1
pkill -9 -f fuse.briefs 2>/dev/null; sleep 1
umount /mnt/briefs-test /mnt/briefs-scratch 2>/dev/null
fusermount -u /mnt/briefs-test 2>/dev/null; fusermount -u /mnt/briefs-scratch 2>/dev/null
/go/bin/mkfs.briefs -f /dev/vdc1 >/dev/null 2>&1
mkdir -p /mnt/briefs-scratch
setsid /go/bin/fuse.briefs -i /dev/vdc1 -m /mnt/briefs-scratch </dev/null >/tmp/fuse-blockdev.log 2>&1 &
sleep 2
echo "=== ls ==="
ls -la /mnt/briefs-scratch 2>&1
echo "=== write + read ==="
echo testblock > /mnt/briefs-scratch/f 2>&1
cat /mnt/briefs-scratch/f 2>&1
echo "=== fuse log ==="
tail -3 /tmp/fuse-blockdev.log
fusermount -u /mnt/briefs-scratch 2>/dev/null
echo "BLOCKDEV_FUSE_DONE"
