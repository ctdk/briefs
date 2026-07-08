#!/bin/bash
# Run a single xfstest on a freshly-mkfs'd TEST_DEV and fsck afterwards.
set -euo pipefail

TEST_DEV=/dev/loop0
TEST_MNT=/mnt/briefs-test

echo "=== fresh mkfs ==="
/go/bin/mkfs.briefs -f "$TEST_DEV" >/dev/null

echo "=== run test ==="
cd /xfstests
HOST_OPTIONS=configs/briefs.config PATH=/usr/sbin:/sbin:$PATH ./check briefs "$1"

echo "=== fsck ==="
/go/bin/fsck.briefs -n "$TEST_DEV" 2>&1 | tail -15

echo "=== free inodes after test ==="
python3 - <<'PY'
import struct
with open("/dev/loop0","rb") as f:
    sb = f.read(4096)
print("FreeInodes:", struct.unpack("<Q", sb[112:120])[0])
PY
