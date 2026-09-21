#!/bin/bash
# loop-617.sh — N solo generic/617 runs to re-verify the June fix
# (0cd7062 punch root_empty straddler orphan) at the current tree
# (P2 + the e5c6242 inode_dio_wait drain).  The original flake was
# ~6.7% per ./check exposure; a clean N=50 streak makes "the bug
# survived" a <=3% event, the same evidentiary bar used for generic/299.
#
# Usage (inside the VM, as root):   RUNS=50 bash /vagrant/tests/xfstests/loop-617.sh
#   RUNS=N        iteration count (default 50)
# Stops on the first FAIL, preserving that run's LOG_DIR and output.
: "${RUNS:=50}"

BASE=/vagrant/tests/xfstests/logs-617-loop
ATTEMPTS="$BASE/attempts.txt"
mkdir -p "$BASE"

for i in $(seq 1 "$RUNS"); do
	LOG="$BASE/run$i"
	mkdir -p "$LOG"
	t0=$(date +%s)
	# Fresh LOG_DIR per run: run-suite auto-resumes tests with existing
	# check logs, so a reused dir would silently skip every test.
	# RESULTS_DIR keeps the per-run archives out of the repo's runs/.
	if ! LOG_DIR="$LOG" RESULTS_DIR="$BASE/archives" \
	     bash /vagrant/tests/xfstests/run-suite.sh generic/617 \
	     > "$BASE/run$i.out" 2>&1; then
		echo "run $i: RUNNER-ERROR" | tee -a "$ATTEMPTS"
		exit 3
	fi
	if grep -q -- "-> FAIL" "$BASE/run$i.out"; then
		echo "run $i: FAIL (evidence in $LOG)" | tee -a "$ATTEMPTS"
		echo "LOOP STOPPED AT FIRST FAIL — run $i LOG_DIR and run$i.out preserved"
		exit 2
	fi
	echo "run $i: PASS ($(( $(date +%s) - t0 ))s)" >> "$ATTEMPTS"
done
echo "ALL $RUNS PASS" | tee -a "$ATTEMPTS"