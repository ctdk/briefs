#!/bin/bash
# guards-stage1.sh — Stage 1 (BH_Verified memoization of btree node reads)
# functional guard set, per the io_uring-perf plan.
#
# Runs each test in isolation via run-suite.sh (mkfs + fresh mount per test)
# and parses the runner's per-test status line (run-suite.sh itself always
# exits from write_archive, so the "-> PASS/FAIL/..." output is the signal):
#   generic/299  x5   torn-read defense (relies on will_modify wait_on_buffer)
#   generic/112  x10  corpse-buffer / dangling next_leaf history
#   generic/340  x10  flaky tier, must not get worse
#   generic/346  x10  flaky tier, must not get worse
#   generic/538  x10  RWF_NOWAIT semantics (Stage 3 pre-state)
#   generic/551  x20  DIO overlap coherence
#   generic/209  x3   DIO readahead (Stage 3 pre-state)
#   spot set once: 053 074 092 263 300 363 471 522 563 616 617
# Prints "GUARDS1 DONE pass=<n> fail=<n> notrun=<n> other=<n> FAILED=<list>"
# at the end.
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin
RUNNER=/vagrant/tests/xfstests/run-suite.sh
LOG="${LOG:-/tmp/guards-stage1.log}"
# keep per-invocation archives out of the repo tree
export RESULTS_DIR="${RESULTS_DIR:-/tmp/guards-stage1-archives}"

pass=0; fail=0; notrun=0; other=0
failed=""

LIST=""
i=0; while [ $i -lt 5 ];  do LIST="$LIST generic/299";  i=$((i+1)); done
i=0; while [ $i -lt 10 ]; do LIST="$LIST generic/112"; i=$((i+1)); done
i=0; while [ $i -lt 10 ]; do LIST="$LIST generic/340"; i=$((i+1)); done
i=0; while [ $i -lt 10 ]; do LIST="$LIST generic/346"; i=$((i+1)); done
i=0; while [ $i -lt 10 ]; do LIST="$LIST generic/538"; i=$((i+1)); done
i=0; while [ $i -lt 20 ]; do LIST="$LIST generic/551"; i=$((i+1)); done
i=0; while [ $i -lt 3 ];  do LIST="$LIST generic/209";  i=$((i+1)); done
for t in 053 074 092 263 300 363 471 522 563 616 617; do
    LIST="$LIST generic/$t"
done

: > "$LOG"
total=$(echo $LIST | wc -w)
echo "total invocations: $total"
n=0
for t in $LIST; do
    n=$((n + 1))
    # Unique LOG_DIR per invocation: run-suite.sh skips any test that has
    # a check-<test>-*.log in its LOG_DIR (the resume feature), which would
    # dedup the deliberate x5/x10/x20 repetitions down to one run.
    export LOG_DIR="/tmp/guards-stage1-logs/inv-$n"
    out=$(bash "$RUNNER" "$t" 2>&1)
    printf '%s\n' "$out" >> "$LOG"
    status=$(printf '%s\n' "$out" | grep -oE '\-> (PASS|FAIL|FAIL \(interrupted, 0 tests passed\)|HANG \(timeout\)|HANG \(no output\)|NOT RUN|UNKNOWN \(exit [0-9]+\)|MKFS FAIL|MOUNT FAIL)' | tail -1)
    case "$status" in
        *PASS)       pass=$((pass + 1));;
        *SKIPPED*)   other=$((other + 1)); failed="$failed $t:skipped";;
        *"NOT RUN")  notrun=$((notrun + 1)); failed="$failed $t:notrun";;
        *)            fail=$((fail + 1)); failed="$failed $t";
                      printf '%s\n' "$out" | grep -E '\-> |FAILURES|Failures' | tail -5;;
    esac
    echo "[$n/$total] $t:$status pass=$pass fail=$fail"
done

echo "GUARDS1 DONE pass=$pass fail=$fail notrun=$notrun FAILED=$failed"