#!/bin/bash
# repro-299.sh — run inside the VM as root.  Replays generic/299's workload
# in a retry loop (up to N attempts) and, on the first fio verify failure,
# PRESERVES the evidence instead of cleaning up: fio's hdr_fail dumps are
# copied to the NFS LOG_DIR, the fio output is kept, and the scratch fs is
# left MOUNTED for inspection (caller should not umount on rc=2).
#
# Usage: bash repro-299.sh <logdir_on_/vagrant> [max_attempts]
set -u
DEV=/dev/vdc1
MNT=/mnt/briefs-scratch
FIO_CFG=/tmp/299.fio
FIO_OUT=/tmp/299.fio.out
LOGDIR=$1
MAX=${2:-10}
mkdir -p "$LOGDIR"

BLK_DEV_SIZE=$(blockdev --getsz "$DEV")
FILE_SIZE=$((BLK_DEV_SIZE * 512))

cat > "$FIO_CFG" <<EOF
[global]
ioengine=libaio
bs=128k
directory=${MNT}
filesize=${FILE_SIZE}
size=999G
iodepth=128
continue_on_error=write
ignore_error=,ENOSPC
error_dump=0
create_on_open=1
fallocate=none
exitall=1

[direct_aio]
direct=1
buffered=0
numjobs=4
rw=randwrite
runtime=100
time_based

[aio-dio-verifier]
numjobs=1
verify=crc32c-intel
verify_fatal=1
verify_dump=1
verify_backlog=1024
verify_async=4
verifysort=1
direct=1
bs=4k
rw=randrw
filename=aio-dio-verifier

[buffered-aio-verifier]
numjobs=1
verify=crc32c-intel
verify_fatal=1
verify_dump=1
verify_backlog=1024
verify_async=4
verifysort=1
direct=0
buffered=1
bs=4k
rw=randrw
filename=buffered-aio-verifier
EOF

for attempt in $(seq 1 "$MAX"); do
    umount "$MNT" 2>/dev/null
    /go/bin/mkfs.briefs -f "$DEV" >/dev/null 2>&1 || { echo "attempt $attempt: MKFS FAIL" >> "$LOGDIR/attempts"; continue; }
    mount -t briefs "$DEV" "$MNT" || { echo "attempt $attempt: MOUNT FAIL" >> "$LOGDIR/attempts"; continue; }

    rm -f "$FIO_OUT"
    fio "$FIO_CFG" --output="$FIO_OUT" 2>/dev/null &
    pid=$!
    for ((i=0; ; i++)); do
        for k in 1 2 3 4; do
            xfs_io -f -c "falloc 0 $FILE_SIZE" "$MNT/direct_aio.$k.0" >/dev/null 2>&1
        done
        for k in 1 2 3 4; do
            xfs_io -c "truncate 0" "$MNT/direct_aio.$k.0" >/dev/null 2>&1
        done
        pgrep -x fio >/dev/null 2>&1 || break
    done
    wait $pid; rc=$?

    cp "$FIO_OUT" "$LOGDIR/fio-attempt-$attempt.out" 2>/dev/null
    if grep -q "bad magic" "$FIO_OUT" 2>/dev/null; then
        echo "attempt $attempt: VERIFY FAIL (fio rc=$rc) — evidence preserved, fs left mounted" >> "$LOGDIR/attempts"
        mkdir -p "$LOGDIR/dumps-$attempt"
        cp "$MNT"/*.hdr_fail* "$LOGDIR/dumps-$attempt/" 2>/dev/null
        ls "$MNT" > "$LOGDIR/dumps-$attempt/mnt-listing.txt" 2>/dev/null
        exit 2
    else
        echo "attempt $attempt: PASS (fio rc=$rc)" >> "$LOGDIR/attempts"
        umount "$MNT"
    fi
done
echo "no reproduction in $MAX attempts" >> "$LOGDIR/attempts"
exit 1