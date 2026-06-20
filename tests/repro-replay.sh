#!/bin/bash
# Reproduce journal replay on a populated fs (clean unmount persists
# journal_log_end but does not checkpoint if < JRN_CHECKPOINT_INTERVAL records,
# so the next mount runs briefs_journal_replay). Verifies the replay path no
# longer oopses on repeated iget/evict of inodes pre-SB_ACTIVE.
set -u
DEV=/dev/loop0
MNT=/mnt/repro
sudo dmesg -C 2>/dev/null
sudo umount $MNT 2>/dev/null
sudo mkfs.briefs $DEV >/dev/null 2>&1
sudo mkdir -p $MNT
sudo mount -t briefs $DEV $MNT || { echo "MOUNT1 FAIL"; exit 1; }
cd $MNT
sudo mkdir adir
for i in $(seq 1 60); do echo "d-$i" | sudo tee file$i >/dev/null; done
sync
cd /
sudo umount $MNT || { echo "UMOUNT FAIL"; exit 1; }
echo "=== clean unmount done; remounting to trigger replay ==="
sudo mount -t briefs $DEV $MNT || { echo "MOUNT2 (replay) FAIL"; exit 1; }
echo "=== remount OK; checking files ==="
ls $MNT | sort | head -5
echo "file count: $(sudo ls $MNT | wc -l)"
sudo umount $MNT
echo "=== dmesg (replay + any oops) ==="
sudo dmesg | grep -iE "briefs:|replay|BUG|oops|list_del|call trace|segfault|invalid opcode" | tail -30