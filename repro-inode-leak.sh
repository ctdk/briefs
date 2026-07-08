#!/bin/bash
# Reproduce possible BrieFS inode leak on TEST_DEV.
# Run inside the VM as root.
set -euo pipefail

TEST_DEV=/dev/loop0
TEST_MNT=/mnt/briefs-test

echo "=== fresh mkfs ==="
/go/bin/mkfs.briefs -f "$TEST_DEV" >/dev/null

echo "=== mount ==="
mount -t briefs "$TEST_DEV" "$TEST_MNT"

echo "=== create/delete 10000 files ==="
mkdir -p "$TEST_MNT/leaktest"
for i in $(seq 1 10000); do
    touch "$TEST_MNT/leaktest/file_$i"
    rm "$TEST_MNT/leaktest/file_$i"
done
rmdir "$TEST_MNT/leaktest"

echo "=== free inodes before unmount ==="
python3 - <<'PY'
import struct
with open("/dev/loop0","rb") as f:
    sb = f.read(4096)
print("FreeInodes:", struct.unpack("<Q", sb[112:120])[0])
PY

echo "=== unmount ==="
umount "$TEST_MNT"

echo "=== fsck ==="
/go/bin/fsck.briefs -n "$TEST_DEV" 2>&1 | tail -20

echo "=== free inodes after unmount ==="
python3 - <<'PY'
import struct
with open("/dev/loop0","rb") as f:
    sb = f.read(4096)
print("FreeInodes:", struct.unpack("<Q", sb[112:120])[0])
PY
