#!/bin/bash
# Run a cluster of xfstests on a freshly-mkfs'd TEST_DEV (no reformat between
# tests) and fsck afterwards. This mimics a single ./check invocation.
set -euo pipefail

TEST_DEV=/dev/loop0
TEST_MNT=/mnt/briefs-test

echo "=== fresh mkfs ==="
/go/bin/mkfs.briefs -f "$TEST_DEV" >/dev/null

echo "=== run tests $* ==="
cd /xfstests
for t in "$@"; do
    HOST_OPTIONS=configs/briefs.config PATH=/usr/sbin:/sbin:$PATH ./check briefs "$t" || true
done

echo "=== fsck ==="
/go/bin/fsck.briefs -n "$TEST_DEV" 2>&1 | tail -20

echo "=== free inodes after cluster ==="
python3 - <<'PY'
import struct
with open("/dev/loop0","rb") as f:
    sb = f.read(4096)
print("FreeInodes:", struct.unpack("<Q", sb[112:120])[0])
PY
