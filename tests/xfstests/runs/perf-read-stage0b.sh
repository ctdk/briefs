#!/bin/bash
# perf-read-stage0b.sh — fast-vs-slow mode profiles for the W9b flip.
#
# Stage 0 (stage0-20260928-170639) found BrieFS W9b (io_uring DIO 4k
# randread qd64) runs at a PERFECTLY UNIFORM mode per invocation: the
# first run on a freshly-prepared filesystem sustains ~107k IOPS for its
# whole 120 s, every later invocation pins at ~32k IOPS (p50 ~2 ms),
# warm or cold cache alike, for 25+ minutes, with zero dmesg trace.  The
# earlier perf profile (captured post-flip) shows reads executing on
# io_uring io-wq workers, ~72% of samples inside unresolved briefs code
# (module was loaded before kptr_restrict=0, so kallsyms zeroed it).
#
# This script, inside the perf VM as root:
#   A. reload the module with kptr_restrict=0 so perf resolves symbols;
#   B. fresh mkfs + 32G prep, then three 30 s W9b probes WITHOUT perf:
#        probe1 — expect FAST (~107k) if the fresh-mount effect reproduces
#        probe2 — expect SLOW (~33k)
#        probe3 — control, confirm the mode is sticky
#   C. perf record a 30 s W9b (the slow mode's symbol-resolved profile);
#   D. umount + mount the SAME filesystem (no re-prep), probe4 — if fast,
#      the slow state is per-mount in-memory; if slow, it is on-disk (or
#      module-global);
#   E. rmmod + insmod + mount, probe5 — if fast, the state is not
#      module-global either (points at per-mount inode state).
# Each probe's JSON lands in the run dir; prints "STAGE0B DONE <runid>".
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
MKFS_BRIEFS="${MKFS_BRIEFS:-/go/bin/mkfs.briefs}"
BLD="${BLD:-/tmp/briefssrc}"
BIG="${BIG:-32G}"

RUNID="stage0b-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
LOG="$RES/run.log"
mkdir -p "$RES"

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; echo "STAGE0B ABORTED $RUNID" | tee -a "$LOG"; exit 1; }
drop_caches() { sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -f "$BLD/briefs_fs.ko" ] || die "no module at $BLD/briefs_fs.ko (run stage0 first)"
sysctl -qw kernel.kptr_restrict=0 2>/dev/null || die "cannot set kptr_restrict"
sysctl -qw kernel.perf_event_paranoid=-1 2>/dev/null || true

probe() {
    # probe <name> <seconds> — one W9b-shaped run, JSON logged, rate echoed.
    local name="$1" rt="$2" rate
    drop_caches
    timeout 600 fio --name="$name" --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime="$rt" \
        --filename="$MNT/w34.bin" --output-format=json \
        --output="$RES/$name.json" > "$RES/$name.fio.out" 2>&1
    rate=$(python3 - "$RES/$name.json" "$name" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1]))
r = d["jobs"][0]["read"]
print(f"iops={r['iops']:.0f} p50={r['clat_ns']['percentile']['50.000000']/1000:.1f}us")
EOF
)
    log "  $name $rate"
}

profile() {
    # profile <name> — perf-recorded 30 s W9b + symbol-resolved report.
    local name="$1"
    drop_caches
    timeout 300 perf record -g -o "$RES/$name.perf.data" -- \
        fio --name="$name" --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime=30 \
        --filename="$MNT/w34.bin" > "$RES/$name.perf.fio.out" 2>&1 \
        || log "  $name perf record exited nonzero"
    perf report -i "$RES/$name.perf.data" --stdio --no-children \
        > "$RES/$name.report.txt" 2>"$RES/$name.report.err"
    perf report -i "$RES/$name.perf.data" --stdio \
        > "$RES/$name.report-children.txt" 2>/dev/null || true
    log "  $name profile saved"
}

# ---- A. module reload with kallsyms visible ----------------------------------
log "reloading module (kptr_restrict=0)"
timeout 120 umount "$MNT" 2>/dev/null || true
rmmod briefs_fs 2>/dev/null || true
insmod "$BLD/briefs_fs.ko" || die "insmod failed"
grep briefs_fs /proc/modules || die "module not loaded"
awk '$3=="briefs_fs" {print "kallsyms addr: "$1}' /proc/kallsyms || true
mkdir -p "$MNT"

# ---- B. fresh fs + probes ------------------------------------------------------
log "fresh mkfs + prep"
wipefs -a "$PERF_DEV" >> "$RES/mkfs.log" 2>&1
sleep 1
"$MKFS_BRIEFS" -f "$PERF_DEV" >> "$RES/mkfs.log" 2>&1 || die "mkfs.briefs failed"
mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount failed"
drop_caches
log "preparing $BIG span file (untimed)"
timeout 7200 fio --name=prep --rw=write --bs=1M --size="$BIG" --direct=1 \
    --iodepth=1 --filename="$MNT/w34.bin" > "$RES/prep.fio.out" 2>&1 \
    || log "prep FAILED/TIMEOUT"

probe probe1 30
probe probe2 30
probe probe3 30

# ---- C. slow-mode profile -------------------------------------------------------
profile slowmode

# ---- D. umount + mount the same fs --------------------------------------------
log "umount + remount SAME filesystem (no re-prep)"
timeout 300 umount "$MNT" || die "umount timed out"
mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "remount failed"
probe probe4 30

# ---- E. module reload + mount --------------------------------------------------
log "rmmod + insmod + mount"
timeout 300 umount "$MNT" || umount -l "$MNT" 2>/dev/null || true
rmmod briefs_fs 2>/dev/null || log "rmmod failed"
insmod "$BLD/briefs_fs.ko" || die "insmod(2) failed"
mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount(2) failed"
probe probe5 30

# ---- F. fast-mode profile (only reachable if probe5 flipped back) ---------------
if python3 -c 'import json,sys; sys.exit(0 if json.load(open(sys.argv[1]))["jobs"][0]["read"]["iops"] > 60000 else 1)' "$RES/probe5.json"; then
    log "probe5 is FAST — profiling fastmode"
    profile fastmode
else
    log "probe5 still slow — no fastmode profile possible without a fresh mkfs"
fi

timeout 300 umount "$MNT" 2>/dev/null || true
dmesg > "$RES/dmesg-end.txt"
log "stage0b $RUNID complete"
echo "STAGE0B DONE $RUNID" | tee -a "$LOG"