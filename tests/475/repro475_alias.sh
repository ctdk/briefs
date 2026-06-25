#!/bin/bash
# Isolated repro of BrieFS dm-error partial-durability block aliasing
# (generic/475 family). Run as ROOT (sudo bash ...). Single cycle, self-contained
# loop device (does NOT touch xfstests' loop0/loop1), explicit dm+loop teardown.
# Faithful to 475: working table -> burst of in-flight allocs -> sudden death
# (error table) -> umount (loses unflushed writes = the "crash") -> working
# table -> remount (replay) -> check.
#
# Do NOT call global `sync` while the error table is active (BrieFS writeback
# redirties EIO'd metadata buffers forever -> D-state hang). umount under the
# error table returns promptly, so we umount but never `sync` under error.
# Usage: RUNS=40 sudo -E bash repro475_alias.sh
set -u
DEVSIZE_MB=${DEVSIZE_MB:-1024}
BASE_FILES=${BASE_FILES:-500}    # durable baseline (committed via one sync under WORKING table)
BURST_FILES=${BURST_FILES:-2500} # in-flight allocs during the error window (split across PROCS)
PROCS=${PROCS:-4}                # concurrent burst processes (more in-flight BIOs at the switch)
SLEEP=${SLEEP:-0.2}              # delay before loading error table (short = more in-flight)
MNT=/mnt/repro475
IMG=/root/repro475.img
DM=repro475
RUNS=${RUNS:-20}
MKFS=/go/bin/mkfs.briefs
FSCK=/go/bin/fsck.briefs

teardown() {
  timeout 20 umount "$MNT" 2>/dev/null
  dmsetup remove "$DM" 2>/dev/null
  [ -n "${LO:-}" ] && { losetup -d "$LO" 2>/dev/null; LO=""; }
  rm -f "$IMG" 2>/dev/null
}

load_table() { # <table>
  dmsetup suspend --nolockfs "$DM" 2>/dev/null
  dmsetup load "$DM" --table "$1" 2>/dev/null
  dmsetup resume "$DM" 2>/dev/null
}

trig=0
for run in $(seq 1 "$RUNS"); do
  teardown
  dmesg -C >/dev/null
  dd if=/dev/zero of="$IMG" bs=1M count="$DEVSIZE_MB" status=none
  LO=$(losetup -f --show "$IMG")
  sz=$(blockdev --getsz "$LO")
  if ! dmsetup create "$DM" --table "0 $sz linear $LO 0" 2>/dev/null; then
    echo "run $run: dmsetup create FAILED"; teardown; continue
  fi
  DDEV=/dev/mapper/$DM
  $MKFS -f "$DDEV" >/dev/null 2>&1
  mkdir -p "$MNT"
  if ! mount -t briefs "$DDEV" "$MNT" 2>/dev/null; then
    echo "run $run: mount1 FAILED"; teardown; continue
  fi
  mkdir "$MNT/d"
  # 1) durable baseline under the WORKING table (one global sync = safe)
  for i in $(seq 1 "$BASE_FILES"); do echo x > "$MNT/d/base$i" 2>/dev/null; done
  sync
  base_count=$(ls "$MNT/d" 2>/dev/null | wc -l)
  # 2) sustained mixed-op pressure via fsstress (the real 475 driver: keeps a
  #    large dirty set + in-flight BIOs so the error-table switch catches a
  #    mid-batch partial-durability state). Load error table mid-run, then kill.
  FSSTRESS=/xfstests/ltp/fsstress
  mkdir -p "$MNT/fs"
  $FSSTRESS -d "$MNT/fs" -n 999999 -p "$PROCS" -s $((run*1000+1)) >/dev/null 2>&1 &
  WP=$!
  sleep "$SLEEP"
  load_table "0 $sz error $LO 0"
  kill -9 "$WP" 2>/dev/null
  pkill -9 -P "$WP" 2>/dev/null
  wait "$WP" 2>/dev/null
  # 3) umount under error = the "crash" (unflushed burst writes lost)
  if ! timeout 30 umount "$MNT" 2>/dev/null; then
    echo "run $run: UMOUNT-HANG (wedge); aborting run"
    teardown
    continue
  fi
  # 4) working table + remount (replays journal)
  load_table "0 $sz linear $LO 0"
  rc_remount=FAIL
  if mount -t briefs "$DDEV" "$MNT" 2>/dev/null; then
    rc_remount=OK
    # how many entries survived? (baseline should survive; burst mostly lost)
    post_count=$(find "$MNT/fs" 2>/dev/null | wc -l)
    timeout 20 umount "$MNT" 2>/dev/null
  else
    post_count=-
  fi
  fsck_out=$(timeout 60 $FSCK "$DDEV" 2>&1); fsck_rc=$?
  # REAL corruption signals only (fsck stdout "no errors found" is success)
  ndi=$(dmesg | grep -ciE "checksum mismatch|not a directory|bad magic|alias|ENOSPC")
  fsck_ok=$(echo "$fsck_out" | grep -ci "FSCK COMPLETE: no errors found")
  echo "run $run: base=$base_count remount=$rc_remount post=$post_count fsck_rc=$fsck_rc fsck_ok=$fsck_ok dmesg_corr=$ndi"
  if [ "$rc_remount" = FAIL ] || [ $fsck_rc -ne 0 ] || [ $fsck_ok -eq 0 ] || [ $ndi -gt 0 ]; then
    trig=$((trig+1))
    echo "  >>> TRIGGERED; fsck tail:"
    echo "$fsck_out" | tail -6 | sed 's/^/      /'
    dmesg | grep -iE "checksum mismatch|not a directory|bad magic|alias|ENOSPC" | head -6 | sed 's/^/      dmesg: /'
  fi
done
teardown
echo "===== TRIGGERED $trig / $RUNS ====="