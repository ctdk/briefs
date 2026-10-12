#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# stress-fsync-laps.sh - crash-consistency test for journal ring laps under a
# per-record fsync stream (the large-file write-perf fix's regression test).
#
# Every fsync advances the journal write position one ring block; a long
# enough stream completes many laps of the default (64-block, aligned)
# ring.  Two historical holes live exactly at the lap boundary:
#
#   - the sync-entry back-pressure test ("write_pos == journal_log_start")
#     is ambiguous between wrapped-full and freshly-retired-empty, so it
#     used to fire a whole-fs checkpoint on EVERY fsync after a retire;
#   - when the condition was narrowed by the log_end != log_start
#     inequality, the LAP-COMPLETING sync became able to commit
#     journal_log_end = own_next = journal_log_start -- persisting an
#     EMPTY replay window over the partial lap's live records.  A crash
#     then replays nothing while the allocator bitmaps (only persisted by
#     checkpoints) are stale since the last checkpoint, so in-use blocks
#     get re-allocated and trie_page_init() zeroes live pages
#     (the generic/040/041 corruption family).
#
# This script writes PAIRS records with one fsync each, then hard-resets the
# VM via sysrq-b with NO umount and NO global sync after the last fsync --
# crashing squarely inside the post-commit, pre-checkpoint exposure window.
# After reboot, run tests/stress-fsync-laps-verify.sh: it checks fsck, the
# record content, and then deliberately allocates MORE (the stale-bitmap
# corruption only manifests when the post-crash allocator hands out the
# blocks the stale bitmap thinks are free) before re-verifying.
#
# Usage (inside the VM, as root):
#   tests/stress-fsync-laps.sh [pairs]
#     pairs  number of write+fsync pairs; default 3000 (~48 ring laps at
#            one block advance per fsync).  Use 35 for the mid-lap sanity
#            variant (crashes before the first lap boundary).
set -uo pipefail
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

PAIRS="${1:-3000}"
REC_KIB=64

BRIEFS_MODULE="${BRIEFS_MODULE:-/vagrant/briefs_fs.ko}"
[ -f "$BRIEFS_MODULE" ] || BRIEFS_MODULE="${PWD}/briefs_fs.ko"

IMG=/var/lib/xfstests-briefs/fsync-laps.img
MNT=/mnt/fsync-laps
LOG=/var/lib/xfstests-briefs/fsync-laps.log
STATE=/var/lib/xfstests-briefs/fsync-laps.state

mkdir -p /var/lib/xfstests-briefs "$MNT"
exec >"$LOG" 2>&1

echo "start $(date -Iseconds) pairs=$PAIRS rec_kib=$REC_KIB"

# Record what the verify script should expect BEFORE touching the test fs,
# so the (global) sync below cannot heal anything that matters.
echo "pairs=$PAIRS" > "$STATE"
echo "rec_kib=$REC_KIB" >> "$STATE"
echo "image=$IMG" >> "$STATE"
sync

rm -f "$IMG"
truncate -s 1G "$IMG"
mkfs.briefs -f "$IMG" >/dev/null 2>&1 || { echo "mkfs failed"; exit 1; }

rmmod briefs_fs 2>/dev/null || true
insmod "$BRIEFS_MODULE" || { echo "insmod $BRIEFS_MODULE failed"; exit 1; }

mount -t briefs -o loop "$IMG" "$MNT" || { echo "mount failed"; exit 1; }

echo "workload start $(date -Iseconds)"

python3 - "$MNT/laps.dat" "$PAIRS" "$REC_KIB" <<'PY'
import sys, os
path, pairs, kib = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
recsz = kib * 1024
fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
for i in range(pairs):
    os.pwrite(fd, bytes([(i * 7 + 13) & 0xff]) * recsz, i * recsz)
    os.fsync(fd)
os.close(fd)
print("wrote+fsynced", pairs, "records")
PY
[ $? -eq 0 ] || { echo "workload python failed"; exit 1; }

echo "workload done $(date -Iseconds) -- crashing now (no umount, no sync)"

# Hard reset, fully detached so it survives the session dying with the VM.
# The last fsync's commit point is already durable; nothing may run that
# could checkpoint the journal between that fsync and the reset.
setsid bash -c "sleep 5; echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger" >/dev/null 2>&1 &
disown

echo "reset scheduled; reboot, then run tests/stress-fsync-laps-verify.sh"