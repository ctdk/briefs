#!/bin/bash
# Single 475 run, full dmesg preserved (no truncation), dump all replay summaries.
cd /xfstests
sudo dmesg -C >/dev/null
sudo timeout 30 umount /mnt/briefs-scratch 2>/dev/null
sudo timeout 30 umount /mnt/briefs-test 2>/dev/null
sudo /go/bin/mkfs.briefs -f /dev/loop0 >/dev/null 2>&1
sudo /go/bin/mkfs.briefs -f /dev/loop1 >/dev/null 2>&1
sudo env HOST_OPTIONS=configs/briefs.config timeout 300 ./check -s briefs generic/475 >/tmp/475_diag.out 2>&1
rc=$?
echo "rc=$rc"
echo "===== ALL replay summaries (every crash-remount cycle) ====="
sudo dmesg | grep -iE "replaying journal|replay complete|replay trie pool|trie collected|replay error|fill_super returning|ENOSPC"
echo "===== corruption / error-phase signatures ====="
sudo dmesg | grep -iE "checksum mismatch|Not a directory|link failed|briefs: error|EIO|bad magic|corrupt" | sort | uniq -c | sort -rn | head -30
echo "===== .out.bad (failure description) ====="
grep -iE "mount|directory|golden|silence" /xfstests/results//briefs/generic/475.out.bad 2>/dev/null | head