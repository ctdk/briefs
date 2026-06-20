#!/bin/bash
# Reproduce the generic/074 mmap+writeback hang with automatic kernel stack
# capture, so the blocked call trace is saved before the (necessary) VM reboot.
# Run as root in the VM:  sudo bash /vagrant/tests/xfstests/repro-074.sh
# Output: /xfstests/hang-capture.txt (host: .../xfstests-dev/hang-capture.txt),
#         /xfstests/074.log.
set -u
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
cd /xfstests || { echo "no /xfstests"; exit 2; }
rm -f /xfstests/hang-capture.txt /xfstests/074.done 2>/dev/null || true
umount /mnt/briefs-scratch 2>/dev/null || true
umount /mnt/briefs-test 2>/dev/null || true

# Watcher: capture the instant a D-state process persists ~4s, then stop.
(
  d_seen=0
  while true; do
    [ -f /xfstests/074.done ] && break
    n=$(ps -eo stat 2>/dev/null | grep -c '^D')
    if [ "$n" -gt 0 ]; then
      d_seen=$((d_seen+1))
      if [ "$d_seen" -ge 2 ]; then
        echo "D-state persisted ~4s -> capturing stacks"
        bash /vagrant/tests/xfstests/capture-hang.sh
        break
      fi
    else
      d_seen=0
    fi
    sleep 2
  done
) &

# Run the single test. A D-state hang won't honor timeout, but if it does NOT
# hang this returns normally and writes 074.done (watcher then exits cleanly).
# HOST_OPTIONS selects the [briefs] config section (without it check falls back
# to a host-named section, prints "need to define parameters for host trixie",
# and runs nothing -- no hang, no capture).
echo "=== launching generic/074 $(date -u +%H:%M:%S) ==="
HOST_OPTIONS=configs/briefs.config timeout 240 ./check -s briefs generic/074 > /xfstests/074.log 2>&1
echo "074 exit: $?" > /xfstests/074.done
echo "074 returned (no hang this run)" | tee -a /xfstests/074.done