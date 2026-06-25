#!/bin/bash
# Run generic/475 N times, report rc + replay summary per run, detect wedges.
N="${1:-10}"
LOG=/tmp/475_loop.log
: > "$LOG"
for i in $(seq 1 "$N"); do
  echo "===== RUN $i/$N =====" | tee -a "$LOG"
  sudo dmesg -C >/dev/null
  sudo timeout 30 umount /mnt/briefs-scratch 2>/dev/null
  sudo timeout 30 umount /mnt/briefs-test 2>/dev/null
  sudo /go/bin/mkfs.briefs -f /dev/loop0 >/dev/null 2>&1
  sudo /go/bin/mkfs.briefs -f /dev/loop1 >/dev/null 2>&1
  # outer guard: if the check+mount wedges, kill it after 330s
  sudo env HOST_OPTIONS=configs/briefs.config timeout 300 ./check -s briefs generic/475 >/tmp/run_$i.out 2>&1
  rc=$?
  echo "RUN $i: rc=$rc" | tee -a "$LOG"
  # wedge check: any briefs mount in D/R state?
  mp=$(pgrep -f 'mount.*briefs-scratch|mount.*briefs-test' 2>/dev/null | head -1)
  if [ -n "$mp" ]; then
    st=$(ps -o stat= -p "$mp" 2>/dev/null)
    echo "RUN $i: WEDGE mount pid=$mp state=$st" | tee -a "$LOG"
  fi
  sudo dmesg | grep -iE "replay error|ENOSPC|fill_super returning|trie pool|trie collected|cap .* hit|replay complete" | tail -6 | sed "s/^/RUN $i dmesg: /" | tee -a "$LOG"
  # cleanup any leftover mount
  sudo timeout 20 umount /mnt/briefs-scratch 2>/dev/null
  sudo timeout 20 umount /mnt/briefs-test 2>/dev/null
done
echo "===== SUMMARY =====" | tee -a "$LOG"
grep -E "^RUN [0-9]+: rc=|WEDGE" "$LOG"