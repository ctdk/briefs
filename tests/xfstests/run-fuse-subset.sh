#!/bin/bash
exec >/tmp/fuse-subset.out 2>&1
set -uo pipefail
pkill -9 -f fuse.briefs 2>/dev/null; sleep 1
umount /mnt/briefs-test /mnt/briefs-scratch 2>/dev/null
fusermount -u /mnt/briefs-test 2>/dev/null; fusermount -u /mnt/briefs-scratch 2>/dev/null

# Source the FUSE config exports directly (avoids the [briefs] section
# mechanism in common/config, which has a chicken-and-egg with FSTYP).
eval "$(grep -E "^export" /xfstests/configs/briefs-fuse.config)"
export MOUNT_PROG=/vagrant/tests/xfstests/fuse-briefs-mount
export UMOUNT_PROG=/vagrant/tests/xfstests/fuse-briefs-umount
export TEST_DIR=/mnt/briefs-test
export SCRATCH_MNT=/mnt/briefs-scratch
export TEST_DEV=/dev/vdb1
export SCRATCH_DEV=/dev/vdc1

cd /xfstests

# generic/547 (fsstress data mismatch) is skipped intentionally — it is a
# known FUSE-bridge bug, see xfstests-fuse-status.md.
for t in generic/003 generic/029 generic/030 generic/032 generic/321 \
         generic/322 generic/640 generic/475 generic/011; do
  echo "===== $t ====="
  pkill -9 -f fuse.briefs 2>/dev/null; sleep 0.5
  umount /mnt/briefs-test /mnt/briefs-scratch 2>/dev/null
  fusermount -u /mnt/briefs-test 2>/dev/null
  fusermount -u /mnt/briefs-scratch 2>/dev/null
  rm -f /tmp/fuse-briefs-*.pid
  rm -rf results/$(echo $t | cut -d/ -f1)/$(echo $t | cut -d/ -f2)* 2>/dev/null
  # dirstress (011) and dm-error (475) can run long; give them more room.
  case "$t" in
    generic/011|generic/475) tout=900 ;;
    *) tout=300 ;;
  esac
  timeout "$tout" ./check "$t" 2>&1 | tail -5
  echo "---"
  fusermount -u /mnt/briefs-test 2>/dev/null
  fusermount -u /mnt/briefs-scratch 2>/dev/null
  pkill -9 -f fuse.briefs 2>/dev/null; sleep 0.5
done
echo "SUBSET_DONE"
