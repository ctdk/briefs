#!/bin/bash
# Loop a single xfstest N times through run-suite.sh for wedge-class
# verification.  run-suite's bar for un-skipping the silent-wedge tests
# (generic/068, generic/127) is >= 8 clean looped solo runs: both pass
# most single exposures and wedge at a flake rate of roughly 1 per 3-5
# runs (see the SKIP_TESTS history in run-suite.sh).  This generalizes
# the repro-269.sh loop but drives run-suite.sh itself, so each
# iteration gets the full per-test isolation (fuser/umount/mkfs/mount)
# and the TIMEOUT_SECS machinery instead of open-coding them here.
#
# The wedge family this targets can take the WHOLE VM down (network
# dead, console blank, reboot-only).  When that happens this loop dies
# with the VM: from the host, detect it as the results file going silent
# mid-iteration, then capture-hang.sh if the VM still answers ssh, or
# follow the virsh-dump forensics playbook if not.
#
# dmesg is sliced per iteration to catch lockdep/splat output even on
# PASS (no /dev/kmsg markers: _check_dmesg would count them as new
# kernel messages and fail the test).
#
# Run inside the VM as root:
#   sudo TEST=generic/068 N=8 bash /vagrant/tests/xfstests/solo-loop.sh
#   sudo TEST=generic/127 N=8 TIMEOUT=1200 bash /vagrant/tests/xfstests/solo-loop.sh
# Output: /xfstests/loop-results-<test>.txt (host: xfstests-dev/),
#         per-iteration run-suite logs in /xfstests/loop-logs-<test>/.
set -uo pipefail
TEST="${TEST:-generic/068}"
N="${N:-8}"
TIMEOUT="${TIMEOUT:-1200}"
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export HOST_OPTIONS=configs/briefs.config FSCK_ENABLED=0 SKIP_TESTS=""
SLUG=$(echo "$TEST" | tr / -)
OUT=/xfstests/loop-results-$SLUG.txt
LOGS=/xfstests/loop-logs-$SLUG
mkdir -p "$LOGS"
: > "$OUT"

for i in $(seq 1 "$N"); do
    iterdir="$LOGS/iter-$i"
    rm -rf "$iterdir"
    mkdir -p "$iterdir"
    base_lines=$(dmesg | wc -l)
    echo "=== iter $i start $(date -u +%H:%M:%S) ===" >> "$OUT"
    # Fresh LOG_DIR every iteration: run-suite resumes on a stale
    # check-<test>-*.log and would skip the test as already run.
    LOG_DIR="$iterdir" TIMEOUT_SECS="$TIMEOUT" RESULTS_DIR=/tmp/loop-archives \
        bash /vagrant/tests/xfstests/run-suite.sh "$TEST" \
        > "$iterdir/run-suite.log" 2>&1
    rc=$?
    slice=$(dmesg | tail -n +$((base_lines + 1)))
    res=$(grep -oE '^  -> (PASS|FAIL|HANG|NOT RUN|MKFS FAIL|MOUNT FAIL|UNKNOWN)' \
        "$iterdir/run-suite.log" | tail -1 | sed 's/^  -> //')
    [ -n "$res" ] || res="no-result(rc=$rc)"
    cls=clean
    if [ "$res" = "HANG" ] || [ "$res" = "no-result(rc=0)" ]; then
        cls=TIMEOUT
    elif echo "$slice" | grep -qE 'WARNING|BUG|lockdep|Call Trace'; then
        cls=SPLAT
    elif [ "$res" != PASS ]; then
        cls=FAIL
    fi
    if [ "$cls" != clean ] && [ -n "$slice" ]; then
        echo "$slice" > "$iterdir/dmesg.slice"
    fi
    echo "iter $i: $res $cls $(date -u +%H:%M:%S)" >> "$OUT"
    if [ "$cls" = TIMEOUT ]; then
        echo "  WEDGE: test never finished -> capture-hang, VM will need reboot" >> "$OUT"
        bash /vagrant/tests/xfstests/capture-hang.sh
        break
    fi
    [ "$cls" != clean ] && echo "  splat/fail class: $cls (see $iterdir/)" >> "$OUT"
done

echo "=== loop done $(date -u +%H:%M:%S) ===" >> "$OUT"
echo "LAUNCHER-DONE" >> "$OUT"