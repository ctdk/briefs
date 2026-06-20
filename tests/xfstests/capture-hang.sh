#!/bin/bash
# Capture kernel state the moment BrieFS writeback wedges in D-state, so the
# exact blocked call trace survives the inevitable VM reboot. Writes to the
# NFS-shared /xfstests tree (host: /home/jeremy/src/xfstests-dev/hang-capture.txt).
# Run as root inside the VM:  sudo bash /vagrant/tests/xfstests/capture-hang.sh
set -u
OUT=/xfstests/hang-capture.txt
{
  echo "=== briefs hang capture $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
  echo
  echo "=== D-state (uninterruptible) processes ==="
  ps -eo pid,stat,etimes,cmd | awk 'NR==1 || $2 ~ /^D/'
  echo
  echo "=== /proc/<pid>/stack + wchan for every D-state pid ==="
  for pid in $(ps -eo pid,stat | awk '$2 ~ /^D/ {print $1}'); do
    comm=$(cat /proc/$pid/comm 2>/dev/null)
    echo "----- pid $pid  ($comm) -----"
    cat /proc/$pid/stack 2>/dev/null
    echo "  wchan: $(cat /proc/$pid/wchan 2>/dev/null)"
    echo "  syscall: $(cat /proc/$pid/syscall 2>/dev/null | cut -d' ' -f1)"
    echo
  done
  echo "=== all stacks mentioning briefs_ / submit_bio / loop_ / journal ==="
  for pid in /proc/[0-9]*; do
    if grep -qiE "briefs_|submit_bio|loop_|do_sync|journal|io_schedule|get_page_from|__block_" "$pid/stack" 2>/dev/null; then
      echo "----- $pid ($(cat $pid/comm 2>/dev/null)) -----"
      cat "$pid/stack" 2>/dev/null
      echo
    fi
  done
  echo "=== sysrq 'w' blocked-task dump (full call traces) -> dmesg ==="
  echo 1 > /proc/sys/kernel/sysrq 2>/dev/null
  echo w > /proc/sysrq-trigger 2>/dev/null
  sleep 2
  echo
  echo "=== dmesg tail (last 120 lines) ==="
  dmesg 2>/dev/null | tail -120
  echo
  echo "=== DONE $(date -u +%H:%M:%S) ==="
} > "$OUT" 2>&1
echo "captured to $OUT (host: /home/jeremy/src/xfstests-dev/hang-capture.txt)"