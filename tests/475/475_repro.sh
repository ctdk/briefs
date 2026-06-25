#!/bin/bash
cd /xfstests
sudo dmesg -C
sudo timeout 30 umount /mnt/briefs-scratch 2>/dev/null
sudo timeout 30 umount /mnt/briefs-test 2>/dev/null
sudo /go/bin/mkfs.briefs -f /dev/loop0 >/dev/null 2>&1
sudo /go/bin/mkfs.briefs -f /dev/loop1 >/dev/null 2>&1
out=$(sudo env HOST_OPTIONS=configs/briefs.config timeout 300 ./check -s briefs generic/475 2>&1)
rc=$?
echo "rc=$rc"
echo "=== dmesg (briefs) ==="
sudo dmesg | grep -iE "briefs|replay|ENOSPC|magic|EIO|alloc|fill_super|trie pool|seed|cap" | tail -25