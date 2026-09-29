#!/bin/bash
# guards-stage3.sh — Stage 3 (IOCB_NOWAIT in the DIO read path + FMODE_NOWAIT
# at open + NOWAIT-honest write paths) guard set, per the io_uring-perf plan.
#
# Run-suite invocations, each isolated (mkfs + fresh mount per test; unique
# LOG_DIR per invocation so run-suite.sh's resume dedup cannot collapse the
# repetitions):
#   generic/538  x10  RWF_NOWAIT semantics (the primary Stage-3 guard)
#   generic/551  x20  DIO overlap coherence
#   generic/209  x3   DIO readahead invalidation
#   spot set once (053 074 092 112 263 299 300 363 471 522 563 616 617)
#   Stage-1 regression re-spots: 299 x2, 112 x3, 340 x3, 346 x3
# Prints "GUARDS3 DONE pass=<n> fail=<n> FAILED=<list>".
set -u

export PATH=/go/bin:/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin
RUNNER=/vagrant/tests/xfstests/run-suite.sh
LOG="${LOG:-/tmp/guards-stage3.log}"
export RESULTS_DIR="${RESULTS_DIR:-/tmp/guards-stage3-archives}"

pass=0; fail=0; notrun=0
failed=""

LIST=""
i=0; while [ $i -lt 10 ]; do LIST="$LIST generic/538"; i=$((i+1)); done
i=0; while [ $i -lt 20 ]; do LIST="$LIST generic/551"; i=$((i+1)); done
i=0; while [ $i -lt 3 ];  do LIST="$LIST generic/209"; i=$((i+1)); done
for t in 053 074 092 112 263 299 300 363 471 522 563 616 617; do
    LIST="$LIST generic/$t"
done
i=0; while [ $i -lt 2 ]; do LIST="$LIST generic/299"; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do LIST="$LIST generic/112"; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do LIST="$LIST generic/340"; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do LIST="$LIST generic/346"; i=$((i+1)); done

: > "$LOG"
total=$(echo $LIST | wc -w)
echo "total invocations: $total"
n=0
for t in $LIST; do
    n=$((n + 1))
    export LOG_DIR="/tmp/guards-stage3-logs/inv-$n"
    out=$(bash "$RUNNER" "$t" 2>&1)
    printf '%s\n' "$out" >> "$LOG"
    status=$(printf '%s\n' "$out" | grep -oE '\-> (PASS|FAIL|FAIL \(interrupted, 0 tests passed\)|HANG \(timeout\)|HANG \(no output\)|NOT RUN|UNKNOWN \(exit [0-9]+\)|MKFS FAIL|MOUNT FAIL|SKIPPED \(.*\))' | tail -1)
    case "$status" in
        *PASS)       pass=$((pass + 1));;
        *SKIPPED*)   failed="$failed $t:skipped";;
        *"NOT RUN")  notrun=$((notrun + 1)); failed="$failed $t:notrun";;
        *)            fail=$((fail + 1)); failed="$failed $t";
                      printf '%s\n' "$out" | grep -E '\-> |Failures' | tail -5;;
    esac
    echo "[$n/$total] $t:$status pass=$pass fail=$fail"
done

echo "GUARDS3 DONE pass=$pass fail=$fail notrun=$notrun FAILED=$failed"