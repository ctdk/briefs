#!/bin/bash
# run-suite-fuse.sh — run xfstests against the FUSE mount (fuse.briefs).
# Exports the FUSE-mount env vars and execs run-suite.sh, which defaults to
# the kernel mount; the overrides here switch it to the FUSE path.
#
# Usage (inside the VM, as root):
#   bash /vagrant/tests/xfstests/run-suite-fuse.sh generic/001 generic/003 ...
export MOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-mount
export UMOUNT_CMD=/vagrant/tests/xfstests/fuse-briefs-umount
export HOST_OPTIONS=configs/briefs-fuse.config
exec /vagrant/tests/xfstests/run-suite.sh "$@"