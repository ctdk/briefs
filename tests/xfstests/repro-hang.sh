#!/bin/bash
# Reproduce a hanging xfstest with automatic kernel stack capture, saving the
# blocked call trace before the (often necessary) VM reboot. Generalizes
# repro-074.sh to any test.
#
# Usage:  sudo bash /vagrant/tests/xfstests/repro-hang.sh <test> [timeout] [d_persist]
#   test       e.g. generic/070
#   timeout    per-test wall-clock cap, seconds (default 600; a D-state hang
#              won't honor it, but a non-hang run returns normally)
#   d_persist  D-state observations (~2s each) that must persist before the first
#              capture, so transient I/O waits don't trigger it (default 2 = ~4s)
#
# Run as root in the VM.  Output:
#   /xfstests/hang-capture-<test>.txt   (host: .../xfstests-dev/hang-capture-<test>.txt)
#   /xfstests/<test>.log               (./check stdout/stderr)
#   /xfstests/<test>.done              (exit marker; watcher exits when it appears)
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

TEST="${1:?usage: repro-hang.sh <test> [timeout] [d_persist]}"
TIMEOUT="${2:-600}"
D_PERSIST="${3:-2}"
TAG="${TEST//\//-}"                       # generic/070 -> generic-070
CAP=/xfstests/hang-capture-$TAG.txt
LOG=/xfstests/$TAG.log
DONE=/xfstests/$TAG.done

cd /xfstests || { echo "no /xfstests"; exit 2; }
rm -f "$CAP" "$LOG" "$DONE" 2>/dev/null || true

# Pre-clean: drop any stale briefs mounts and DM targets off the test devs so
# _scratch_mkfs isn't fighting a wedged mount.
for m in /mnt/briefs-scratch /mnt/briefs-test; do umount -f "$m" 2>/dev/null || true; done
for dev in vdb vdc; do
  for t in $(ls /dev/mapper/ 2>/dev/null | grep -v control); do
    dmsetup info "$t" 2>/dev/null | grep -q "/dev/$dev" && dmsetup remove -f "$t" 2>/dev/null
  done
done

# Watcher: capture the instant D-state persists ~D_PERSIST*2s, then re-capture
# every ~15s (up to 8 times) so a late-appearing kworker (cf. 074's flush-7:1)
# is caught too. Exits cleanly if the test writes the done marker.
(
  d_seen=0; caps=0
  while true; do
    [ -f "$DONE" ] && break
    n=$(ps -eo stat 2>/dev/null | grep -c '^D')
    if [ "$n" -gt 0 ]; then
      d_seen=$((d_seen+1))
      if [ "$d_seen" -ge "$D_PERSIST" ]; then
        caps=$((caps+1))
        echo ">>> capture #$caps at $(date -u +%H:%M:%S)  D-state count=$n" >> "$CAP"
        # capture-hang.sh writes its stack dump to its own hardcoded
        # $OUT=/xfstests/hang-capture.txt, NOT to stdout -- so append that
        # file (the real D-state + sysrq-w + dmesg dump) to the per-test CAP.
        bash /vagrant/tests/xfstests/capture-hang.sh
        cat /xfstests/hang-capture.txt >> "$CAP" 2>/dev/null
        if [ "$caps" -ge 8 ]; then break; fi
        sleep 15
        d_seen=0   # require it to re-persist before the next capture
      else
        sleep 2
      fi
    else
      d_seen=0
      sleep 2
    fi
  done
) &

echo "=== launching $TEST $(date -u +%H:%M:%S)  timeout=${TIMEOUT}s ==="
HOST_OPTIONS=configs/briefs.config timeout "$TIMEOUT" ./check -s briefs "$TEST" > "$LOG" 2>&1
rc=$?
echo "$TEST exit: $rc" > "$DONE"
echo "$TEST returned rc=$rc (no persistent hang this run)" | tee -a "$DONE"
echo "capture: $CAP   log: $LOG"