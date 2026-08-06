#!/bin/bash
exec >/tmp/fuse-suite.out 2>&1
pkill -9 -f fuse.briefs 2>/dev/null; sleep 1
umount /mnt/briefs-test /mnt/briefs-scratch 2>/dev/null
fusermount -u /mnt/briefs-test 2>/dev/null; fusermount -u /mnt/briefs-scratch 2>/dev/null
rm -f /var/tmp/xfstests-logs/check-003-* /var/tmp/xfstests-logs/mount-err-003-* /tmp/fuse-briefs-*.pid
rm -rf /xfstests/results/generic/003* 2>/dev/null
export MOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-mount
export UMOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-umount
export HOST_OPTIONS=configs/briefs-fuse.config
cd /xfstests
bash /vagrant/tests/xfstests/run-suite.sh generic/003
echo "SUITE_EXIT=$?"
