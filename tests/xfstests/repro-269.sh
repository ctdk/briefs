#!/bin/bash
# Attribute the b455-fix-fullsuite generic/269 wedge (10-02): the full 7.3-rc4
# lockdep suite round on the uncommitted 455-fix build ran generic/001-268
# clean, then wedged during generic/269 -- a ~25s DIO test that passed in the
# 10-01 pre-fix baseline round and is NOT on the known-wedge list (068/127).
# This loop runs generic/269 standalone N times on the 455-fix module so the
# wedge either reproduces (capture-hang.sh saves the blocked stacks BEFORE the
# VM reboot a wedge makes inevitable -- the suite round left no splat behind)
# or the flake theory strengthens.  dmesg is sliced per iteration to catch
# lockdep/splat output even on PASS (no /dev/kmsg markers: _check_dmesg would
# count them as new kernel messages and fail the test).
#
# Run inside the VM as root:
#   sudo N=20 bash /vagrant/tests/xfstests/repro-269.sh
# Output: /xfstests/269-loop-results.txt (host: xfstests-dev/269-loop-results.txt),
#         per-iteration check logs in /xfstests/269-logs/.
set -uo pipefail
N="${N:-20}"
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOST_OPTIONS=configs/briefs.config FSCK_ENABLED=0 SKIP_TESTS=""
OUT=/xfstests/269-loop-results.txt
LOGS=/xfstests/269-logs
mkdir -p "$LOGS"
: > "$OUT"

cd /xfstests || { echo "no /xfstests" > "$OUT"; exit 2; }

stopflag=/tmp/269-loop-stop
rm -f "$stopflag"

# Pre-check: D-state persisting ~30s at launch means a PREVIOUS wedge; capture
# immediately instead of running.  A single D-state sighting is NOT a wedge --
# it can be a transient fsync/NFS I/O (the first launch aborted spuriously
# seconds after an rsync+build+insmod burst left a process momentarily in D),
# so wait for it to clear before starting the loop.
pre_d=0
for _ in $(seq 1 15); do
  n=$(ps -eo stat | grep -c '^D')
  if [ "$n" -gt 0 ]; then
    pre_d=$((pre_d+1))
    sleep 2
  else
    pre_d=0
    break
  fi
done
if [ "$pre_d" -gt 0 ]; then
  echo "D-state persisted ~${pre_d}0s at launch -> capturing, NOT running" >> "$OUT"
  bash /vagrant/tests/xfstests/capture-hang.sh
  exit 3
fi
# NO live D-state watcher: on the 455-fix build a PASSING generic/269 keeps
# ~110 of 128 fsstress threads in killable-mutex D ('Dl') for 16+ s under
# the serialized journal (10-02 16:38Z and 16:41Z false positives, both
# iterations finished rc=0 clean).  The wedge detector is the per-iteration
# `timeout 300` on ./check: rc=124 means the test never finished -- capture
# stacks then and only then.

for i in $(seq 1 "$N"); do
  umount /mnt/briefs-scratch 2>/dev/null || true
  umount /mnt/briefs-test 2>/dev/null || true
  rm -rf /xfstests/results/briefs/
  base_lines=$(dmesg | wc -l)
  echo "=== iter $i start $(date -u +%H:%M:%S) ===" >> "$OUT"
  HOST_OPTIONS=configs/briefs.config timeout 300 ./check -s briefs generic/269 \
    > "$LOGS/iter-$i.log" 2>&1
  rc=$?
  slice=$(dmesg | tail -n +$((base_lines + 1)))
  cls=clean
  if [ "$rc" -eq 124 ]; then
    cls=TIMEOUT
  elif echo "$slice" | grep -qE 'WARNING|BUG|lockdep|Call Trace'; then
    cls=SPLAT
  elif [ "$rc" -ne 0 ]; then
    cls=FAIL
  fi
  if [ "$cls" != clean ] && [ -n "$slice" ]; then
    echo "$slice" > "$LOGS/iter-$i.dmesg"
  fi
  echo "iter $i: rc=$rc $cls $(date -u +%H:%M:%S)" >> "$OUT"
  if [ "$cls" = TIMEOUT ]; then
    echo "  WEDGE: test never finished -> capture-hang, VM will need reboot" >> "$OUT"
    bash /vagrant/tests/xfstests/capture-hang.sh
    break
  fi
  [ "$cls" != clean ] && echo "  splat/fail class: $cls (see $LOGS/iter-$i.dmesg)" >> "$OUT"
done

echo "=== loop done $(date -u +%H:%M:%S) ===" >> "$OUT"
echo "LAUNCHER-DONE"