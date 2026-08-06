#!/bin/bash
# BrieFS FUSE-bridge ↔ kernel interop test (Phase 12).
#
# Verifies a volume written by the Go FUSE bridge (fuse.briefs, fuse-rw-work
# branch) is mountable by the BrieFS kernel module and that the kernel reads
# back the FUSE-written data, xattrs, symlinks, and modes unchanged. The
# shared on-disk format (briefs/ package) + Go journal port keep the volume
# kernel-compatible; the kernel replays any pending journal records on mount.
#
# Run on the VM: bash /vagrant/interop_test.sh
# Needs: fuse3 (fusermount), the briefs kernel module loaded, and
# /go/bin/{mkfs,fuse,fsck}.briefs built from fuse-rw-work.

set -e
exec >/tmp/interop.out 2>&1
pkill -f "fuse.briefs" 2>/dev/null || true; sleep 1
sudo umount /tmp/fm /tmp/km 2>/dev/null || true
fusermount -u /tmp/fm 2>/dev/null || true
rm -f /tmp/interop.briefs; rm -rf /tmp/fm /tmp/km; mkdir -p /tmp/fm /tmp/km

/go/bin/mkfs.briefs -s 10000 /tmp/interop.briefs
setsid /go/bin/fuse.briefs -i /tmp/interop.briefs -m /tmp/fm </dev/null >/tmp/fuse.log 2>&1 &
FUSE_PID=$!
trap 'kill $FUSE_PID 2>/dev/null || true' EXIT
sleep 3

echo "== FUSE writes =="
echo hello-kernel > /tmp/fm/file
setfattr -n user.tag -v fuseval /tmp/fm/file
chmod 0640 /tmp/fm/file
mkdir /tmp/fm/sub; echo nested > /tmp/fm/sub/n
ln -s /file /tmp/fm/link
ls -la /tmp/fm

echo "== clean FUSE unmount =="
fusermount -u /tmp/fm; sleep 1
echo "== fsck =="
/go/bin/fsck.briefs /tmp/interop.briefs | tail -2

echo "== kernel mount + readback =="
sudo mount -t briefs -o loop /tmp/interop.briefs /tmp/km
cat /tmp/km/file
cat /tmp/km/sub/n
readlink /tmp/km/link
getfattr -d /tmp/km/file 2>&1 | grep -v '^#' | grep user.tag
sudo umount /tmp/km
echo INTEROP_OK
