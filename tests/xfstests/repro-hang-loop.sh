#!/bin/bash
# Loop a hanging xfstest N times under the D-state watcher, to catch an
# INTERMITTENT hang (these passes pass single runs but wedge at other times,
# cf. generic/127). Stops at the first iteration that does NOT pass (rc!=0,
# i.e. timeout-kill/hang or a real test failure) and points at the capture.
#
# Usage:  sudo bash /vagrant/tests/xfstests/repro-hang-loop.sh <test> <iters> [per_timeout]
#   test        e.g. generic/224
#   iters      max iterations (default 20)
#   per_timeout  per-iteration wall-clock cap (default 300)
#
# Run as root in the VM.  Per-iter capture: /xfstests/hang-capture-<test>.txt
# (host: .../xfstests-dev/hang-capture-<test>.txt).  A real hang leaves a large
# stack dump there; a clean pass leaves at most the transient-D-state header.
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

TEST="${1:?usage: repro-hang-loop.sh <test> <iters> [per_timeout]}"
ITERS="${2:-20}"
PTO="${3:-300}"
TAG="${TEST//\//-}"
CAP=/xfstests/hang-capture-$TAG.txt
DONE=/xfstests/$TAG.done
LOG=/xfstests/$TAG.log

echo "=== looping $TEST up to $ITERS times (per-iter timeout ${PTO}s) ==="
i=0
while [ "$i" -lt "$ITERS" ]; do
  i=$((i+1))
  rm -f "$CAP" "$DONE" "$LOG" 2>/dev/null || true
  # Run one iteration (self-contained: watcher + check + done marker).
  bash /vagrant/tests/xfstests/repro-hang.sh "$TEST" "$PTO" > /dev/null 2>&1
  rc=$(sed -n 's/^'"$TEST"' exit: //p' "$DONE" 2>/dev/null | head -1)
  rc=${rc:-unknown}
  pass=$(grep -c "Passed all 1 tests" "$LOG" 2>/dev/null || echo 0)
  cap_sz=$(stat -c%s "$CAP" 2>/dev/null || echo 0)
  echo "iter $i/$ITERS  rc=$rc  pass=$pass  capture=${cap_sz}B"
  if [ "$pass" -ge 1 ]; then
    continue   # clean pass, next iteration
  fi
  # Did NOT pass -> timeout-kill/hang or a real failure. Stop + point at capture.
  echo ">>> STOP at iter $i: $TEST did not pass (rc=$rc). Capture: $CAP"
  if [ "$cap_sz" -gt 500 ]; then
    echo ">>> capture is ${cap_sz}B -- likely a real D-state stack dump. Examine $CAP"
  else
    echo ">>> capture is only ${cap_sz}B -- may be a test failure, not a hang. Examine $LOG"
  fi
  break
done
echo "=== loop done: $i iteration(s) ==="