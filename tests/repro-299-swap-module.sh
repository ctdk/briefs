#!/bin/bash
# repro-299-swap-module.sh — run inside the VM as root.  Unmounts any briefs
# mounts, swaps the loaded briefs_fs module, and launches the repro-299.sh
# loop into a fresh log dir.  Usage:
#   bash repro-299-swap-module.sh /vagrant/briefs_fs-P2fix.ko /vagrant/tests/repro299/run2 20
set -u
KO=$1
LOGDIR=$2
MAX=$3

for m in $(mount | awk '$5=="briefs" {print $3}'); do
    umount "$m" || { echo "cannot umount $m" >&2; exit 1; }
done
rmmod briefs_fs || { echo "rmmod failed" >&2; exit 1; }
insmod "$KO" || { echo "insmod $KO failed" >&2; exit 1; }
echo "loaded $(basename "$KO")"
nohup bash /vagrant/tests/repro-299.sh "$LOGDIR" "$MAX" >/tmp/repro-299-$(basename "$LOGDIR").log 2>&1 &
disown
echo "loop launched in $LOGDIR"