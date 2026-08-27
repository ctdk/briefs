#!/bin/bash
# Run generic/475 N times to gauge the Phase 2 pass rate.  Each iteration runs
# the per-test runner in isolation (mkfs both devs, mount, ./check 475).  The
# resume log is cleared between iterations so run-suite does not skip.  For each
# run we record the outcome (PASS/FAIL/HANG) and classify the dmesg failure
# mode for that iteration only (sliced by dmesg line count -- we do NOT write a
# /dev/kmsg marker, which _check_dmesg would count as a new kernel message and
# fail the test): WARN (mark_buffer_dirty !uptodate), bad-magic-0x00000000,
# journal init/replay failed (mount could not read superblock), btree checksum,
# journal-tail persist failure, or clean.
#
# A wedge (D-state) aborts the loop -- Phase 2 is expected to eliminate wedges;
# if one appears, the VM needs a reboot and the remaining iterations are skipped.
#
# Run inside the VM as root:
#   sudo N=8 bash /vagrant/tests/xfstests/run-475-repeat.sh
set -uo pipefail
N="${N:-8}"
export HOST_OPTIONS=configs/briefs.config FSCK_ENABLED=0 SKIP_TESTS=""

classify() {
  local slice="$1"
  local cls=""
  if echo "$slice" | /usr/bin/grep -q 'mark_buffer_dirty'; then cls="$cls,WARN"; fi
  if echo "$slice" | /usr/bin/grep -q 'bad magic 0x00000000'; then cls="$cls,badmagic0"; fi
  # Non-zero bad magic = data-fill (freed metadata block reused as a DATA
  # extent, on-disk parent still points to it) -- mode-2 reuse, the residual
  # Phase 2 does NOT fix (it only fixed 0x00000000 zeroed/drift).  Patterns like
  # 0x2a2a2a2a/0xd9d9d9d9 (fsstress data bytes) start with a nonzero nibble.
  if echo "$slice" | /usr/bin/grep -qE 'bad magic 0x[1-9a-f]'; then cls="$cls,badmagicdata"; fi
  if echo "$slice" | /usr/bin/grep -q 'journal init failed\|journal replay failed\|replay FAILED'; then cls="$cls,jinitfail"; fi
  if echo "$slice" | /usr/bin/grep -q 'checksum mismatch'; then cls="$cls,checksum"; fi
  if echo "$slice" | /usr/bin/grep -q 'failed to persist journal tail'; then cls="$cls,jtailfail"; fi
  if echo "$slice" | /usr/bin/grep -q "can't read superblock"; then cls="$cls,nosb"; fi
  echo "${cls#,}"
}

run_one() {
  local iter="$1"
  # Abort the whole loop if the kernel has D-state tasks (a wedge).
  local d
  d=$(ps -eo stat | /usr/bin/grep -c '^D' || true)
  if [ "$d" -gt 0 ]; then
    echo "iter $iter: ABORT (D-state=$d -- wedge; VM needs reboot), skipping remaining"
    echo "ABORT wedge" >> /tmp/475-repeat-results.txt
    return 2
  fi
  rm -f /var/tmp/xfstests-logs/check-475-*.log /var/tmp/xfstests-logs/mount-err-475-*.log
  rm -rf /xfstests/results/briefs/
  local start_lines
  start_lines=$(dmesg 2>/dev/null | wc -l)
  local out
  out=$(timeout 1000 bash /vagrant/tests/xfstests/run-suite.sh generic/475 2>&1)
  local rc=$?
  local outcome="UNKNOWN"
  if echo "$out" | /usr/bin/grep -qE 'PASS: +[1-9]'; then outcome=PASS; fi
  if echo "$out" | /usr/bin/grep -qE 'FAIL: +[1-9]'; then outcome=FAIL; fi
  if echo "$out" | /usr/bin/grep -qE 'HANG: +[1-9]'; then outcome=HANG; fi
  local slice cls
  slice=$(dmesg 2>/dev/null | tail -n +$((start_lines + 1)))
  cls=$(classify "$slice")
  echo "iter $iter: $outcome  [dmesg: $cls]  (rc=$rc)"
  echo "$outcome $cls" >> /tmp/475-repeat-results.txt
  # Archive full detail for non-PASS iters so failures can be categorized
  # post-hoc (the next iteration clears /xfstests/results/briefs/ first thing).
  if [ "$outcome" != "PASS" ]; then
    local arc=/tmp/475-archives/iter-$iter
    mkdir -p "$arc"
    printf '%s\n' "$out" >"$arc/run-suite.out"
    printf '%s\n' "$slice" >"$arc/dmesg.slice"
    { echo "$outcome $cls"; echo "rc=$rc"; } >"$arc/summary"
    cp /xfstests/results/briefs/generic/475.full "$arc/475.full" 2>/dev/null || true
    cp /xfstests/results/briefs/generic/475.out "$arc/475.out" 2>/dev/null || true
    cp /xfstests/results/briefs/generic/475.dmesg "$arc/475.dmesg" 2>/dev/null || true
    cp /xfstests/results/briefs/generic/475.bad "$arc/475.bad" 2>/dev/null || true
    cp /var/tmp/xfstests-logs/mount-err-475-*.log "$arc/" 2>/dev/null || true
  fi
  return 0
}

rm -f /tmp/475-repeat-results.txt
rm -rf /tmp/475-archives
echo "=== generic/475 x$N (Phase 2) $(date) ==="
for i in $(seq 1 "$N"); do
  if ! run_one "$i"; then break; fi
done
echo "=== summary ==="
PASS=$(/usr/bin/grep -c '^PASS' /tmp/475-repeat-results.txt 2>/dev/null || echo 0)
FAIL=$(/usr/bin/grep -c '^FAIL' /tmp/475-repeat-results.txt 2>/dev/null || echo 0)
HANG=$(/usr/bin/grep -c '^HANG' /tmp/475-repeat-results.txt 2>/dev/null || echo 0)
ABORT=$(/usr/bin/grep -c '^ABORT' /tmp/475-repeat-results.txt 2>/dev/null || echo 0)
echo "PASS=$PASS FAIL=$FAIL HANG=$HANG ABORT=$ABORT of $N"
echo "=== per-iter ==="
cat /tmp/475-repeat-results.txt 2>/dev/null