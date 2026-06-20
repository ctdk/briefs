#!/bin/bash
# Isolate whether fsync makes file data+extents durable (page-cache dropped, re-read).
set -u
DEV=/dev/loop1
MNT=/mnt/probe
sudo umount $MNT 2>/dev/null
sudo mkfs.briefs $DEV >/dev/null 2>&1
sudo mkdir -p $MNT
sudo mount -t briefs $DEV $MNT || { echo "MOUNT FAIL"; exit 1; }
sudo xfs_io -f -c "pwrite -S 0xaa 0 8K" -c "fsync" $MNT/foo >/dev/null
sudo mkdir $MNT/td1 $MNT/td2
sudo touch $MNT/td1/bar
sync
sudo xfs_io -c "pwrite -S 0xbb 8K 8K" $MNT/foo >/dev/null
sudo mv $MNT/td1/bar $MNT/td2/bar
sudo xfs_io -c "fsync" $MNT/td1 >/dev/null
sudo xfs_io -c "fsync" $MNT/foo >/dev/null
echo "=== before drop_caches (page cache) ==="
sudo od -A d -t x1 $MNT/foo | head -3
echo "=== drop caches + re-read (from disk) ==="
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null
sudo od -A d -t x1 $MNT/foo | head -3
echo "=== fsck -n scratch ==="
sudo fsck.briefs -n $DEV 2>&1 | grep -iE "foo|inode [0-9]+ |extent|error|warn" | head -20
sudo umount $MNT
