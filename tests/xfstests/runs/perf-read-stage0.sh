#!/bin/bash
# perf-read-stage0.sh — Stage 0 of the io_uring qd64 DIO-read campaign.
#
# Baseline measurement + attribution for the W9b gap (42k IOPS vs ext4's
# 166k at io_uring DIO randread 4k qd64, run-20260925-201047).  Everything
# the fix stages are gated on lives here:
#
#   1. reproduce:  W9b x5 + W3a x5, identical job specs to the round
#      (drop_caches before every invocation, JSON + per-second IOPS logs,
#      so the rep-bimodality shape is captured, not just medians);
#   2. profile:    perf record -g on a 30 s W9b run — the gate.  The
#      working theory (plan: io_uring qd64) predicts briefs_crc32c +
#      btree_read_node + sb_bread dominate the single fio submitter
#      thread's kernel time.  If they don't, stop and re-diagnose.
#   3. geometry:   filefrag -v w34.bin — expect ~32k extents of 256
#      blocks (1 MiB) each, tree height 3.
#   4. bimodality: 60 s reps x6 with NO drop_caches between reps vs x6
#      harness-style (drop every rep), briefs and ext4 interleaved, with
#      vmstat running — attributes the 87k/33k/42k rep spread to either
#      cold-start verification cost (drops with the cache) or host noise
#      (ext4 spreads the same way).
#
# Usage (inside the perf VM, as root, detached):
#   sudo setsid nohup bash /vagrant/tests/xfstests/runs/perf-read-stage0.sh \
#       > /tmp/stage0-launch.log 2>&1 &
# Prints "STAGE0 DONE <runid>" (or "STAGE0 ABORTED") at the very end.
#
# Environment: stock Debian 6.12.107+deb13-amd64, /dev/vdd 64 GiB raw
# cache=none, 16 GiB RAM / 8 vCPU.  Builds the module from /tmp/briefssrc
# (rsync of /vagrant, .git excluded) like the perf round did.
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
MKFS_BRIEFS="${MKFS_BRIEFS:-/go/bin/mkfs.briefs}"
SRC="${SRC:-/vagrant}"
BLD="${BLD:-/tmp/briefssrc}"
BIG="${BIG:-32G}"
W9_RUNTIME="${W9_RUNTIME:-120}"
W3A_RUNTIME="${W3A_RUNTIME:-120}"
BIMODAL_RUNTIME="${BIMODAL_RUNTIME:-60}"
REPS="${REPS:-5}"
BIMODAL_REPS="${BIMODAL_REPS:-6}"

RUNID="stage0-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
LOG="$RES/run.log"
mkdir -p "$RES"
ABORTED=0

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; ABORTED=1; exit 1; }

drop_caches() {
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -b "$PERF_DEV" ] || die "$PERF_DEV is not a block device"
[ -x "$MKFS_BRIEFS" ] || die "mkfs.briefs missing at $MKFS_BRIEFS"
[ "$(uname -r)" = "6.12.107+deb13-amd64" ] || die "unexpected kernel $(uname -r) — this baseline must match the round's stock 6.12.107"

{
    echo "date: $(date -Is)"
    echo "uname: $(uname -a)"
    echo "cpus: $(nproc)"
    free -h
    echo "fio: $(fio --version)"
    echo "perf: $(perf --version 2>&1 || echo MISSING)"
    echo "device: $(lsblk -nd -o NAME,SIZE,TYPE,ROTA "$PERF_DEV")"
    echo "big: $BIG  device: $PERF_DEV"
} > "$RES/environment.txt"

# ---- 0. build the module -----------------------------------------------------
log "building module from $SRC -> $BLD"
rsync -a --delete --exclude .git "$SRC/" "$BLD/" || die "rsync failed"
( cd "$BLD" && make clean >/dev/null 2>&1; make -j"$(nproc)" ) >> "$RES/build.log" 2>&1 \
    || die "module build failed (see $RES/build.log)"
[ -f "$BLD/briefs_fs.ko" ] || die "build produced no briefs_fs.ko"
BRIEFS_MODULE="$BLD/briefs_fs.ko"
log "module built: $(/usr/sbin/modinfo -F version "$BRIEFS_MODULE") rev=$(/usr/sbin/modinfo -F description "$BRIEFS_MODULE" 2>/dev/null)"

rmmod briefs_fs 2>/dev/null || true
insmod "$BRIEFS_MODULE" || die "insmod failed"
mkdir -p "$MNT"

setup_briefs() {
    timeout 120 umount "$MNT" 2>/dev/null || true
    wipefs -a "$PERF_DEV" >> "$RES/mkfs.log" 2>&1
    sleep 1
    "$MKFS_BRIEFS" -f "$PERF_DEV" >> "$RES/mkfs.log" 2>&1 || die "mkfs.briefs failed"
    mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount -t briefs failed"
}

setup_ext4() {
    timeout 120 umount "$MNT" 2>/dev/null || true
    wipefs -a "$PERF_DEV" >> "$RES/mkfs.log" 2>&1
    sleep 1
    mkfs.ext4 -F "$PERF_DEV" >> "$RES/mkfs.log" 2>&1 || die "mkfs.ext4 failed"
    mount -t ext4 -o noatime "$PERF_DEV" "$MNT" || die "mount -t ext4 failed"
}

# prep <fsname> — the untimed 32G span-file write, exactly as the round's
# w3_w4_w9_random() prepared it (perf-vs-other-fs.sh:142-148).
prep_span() {
    local fs="$1"
    drop_caches
    log "  [$fs] preparing $BIG span file w34.bin (untimed)"
    timeout 7200 fio --name=prep --rw=write --bs=1M \
        --size="$BIG" --direct=1 --iodepth=1 --filename="$MNT/w34.bin" \
        > "$RES/$fs/prep.fio.out" 2>&1 || log "  [$fs] span-file prep FAILED/TIMEOUT"
}

start_vmstat() { vmstat 1 > "$RES/vmstat.$1.log" & VMSTAT_PID=$!; }
stop_vmstat() { kill "$VMSTAT_PID" 2>/dev/null || true; }

# w9b <fsname> <rep> <runtime> <keep-cache> — one W9b-shaped invocation.
w9b() {
    # NB: out= must be a separate local statement — all RHS expansions in one
    # local happen before any assignment, so referencing $fs/$rep there trips
    # set -u with the variable still unbound.
    local fs="$1" rep="$2" rt="$3" keep="$4"
    local out="$RES/$fs/W9b.$rep"
    [ "$keep" = 0 ] && drop_caches
    timeout 7200 fio --name=w9b --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime="$rt" \
        --filename="$MNT/w34.bin" --output-format=json --output="$out.json" \
        --write_iops_log="$out.iops" > "$out.fio.out" 2>&1
    local iops
    iops=$(awk -F'"' '/"iops":/ {v=$3; gsub(/[^0-9.]/, "", v); print v; exit}' "$out.json" 2>/dev/null)
    log "  [$fs] W9b rep=$rep runtime=${rt}s drop_cache=$((1-keep)) iops=$iops"
}

w3a() {
    local fs="$1" rep="$2"
    local out="$RES/$fs/W3a.$rep"
    drop_caches
    timeout 7200 fio --name=w3a --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=1 --time_based \
        --runtime="$W3A_RUNTIME" --filename="$MNT/w34.bin" \
        --output-format=json --output="$out.json" \
        --write_iops_log="$out.iops" > "$out.fio.out" 2>&1
    local iops
    iops=$(awk -F'"' '/"iops":/ {v=$3; gsub(/[^0-9.]/, "", v); print v; exit}' "$out.json" 2>/dev/null)
    log "  [$fs] W3a rep=$rep iops=$iops"
}

# ---- 1. baseline: briefs W9b x5 + W3a x5, harness-style ---------------------
mkdir -p "$RES/briefs"
setup_briefs
prep_span briefs

log "geometry: filefrag w34.bin"
filefrag -v "$MNT/w34.bin" > "$RES/briefs/filefrag.txt" 2>&1 || true
EXTENTS=$(awk '$1 ~ /^[0-9]+:$/ { n++ } END { print n+0 }' "$RES/briefs/filefrag.txt")
log "  w34.bin extents=$EXTENTS (expect ~32768 @ 256 blocks)"

start_vmstat baseline
for r in $(seq "$REPS"); do w9b briefs "$r" "$W9_RUNTIME" 0; done
for r in $(seq "$REPS"); do w3a briefs "$r"; done
stop_vmstat

# ---- 2. profile: perf record on a 30 s W9b run (the gate) --------------------
if ! command -v perf >/dev/null 2>&1; then
    log "perf missing — installing linux-perf"
    apt-get install -y linux-perf >> "$RES/build.log" 2>&1 \
        || die "cannot install perf"
fi
sysctl -qw kernel.perf_event_paranoid=-1 2>/dev/null || true
sysctl -qw kernel.kptr_restrict=0 2>/dev/null || true
log "profiling: perf record -g, 30 s W9b run"
drop_caches
timeout 300 perf record -g -o "$RES/briefs/perf.data" -- \
    fio --name=w9bprof --ioengine=io_uring --rw=randread --bs=4k \
    --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime=30 \
    --filename="$MNT/w34.bin" > "$RES/briefs/perf.fio.out" 2>&1 \
    || log "  perf record exited nonzero (see perf.fio.out)"
perf report -i "$RES/briefs/perf.data" --stdio --no-children \
    > "$RES/briefs/perf-report.txt" 2>/dev/null || true
perf report -i "$RES/briefs/perf.data" --stdio \
    > "$RES/briefs/perf-report-children.txt" 2>/dev/null || true
log "  perf report saved (perf-report.txt); gate: briefs_crc32c + btree_read_node should dominate"

# ---- 3. bimodality attribution ------------------------------------------------
# briefs: 6 reps keeping the cache warm (no drop_caches between reps),
# then 6 harness-style reps (drop every rep), one span file throughout.
log "bimodality: briefs, ${BIMODAL_RUNTIME}s reps, warm cache (no drops)"
for r in $(seq "$BIMODAL_REPS"); do w9b briefs "warm.$r" "$BIMODAL_RUNTIME" 1; done
log "bimodality: briefs, ${BIMODAL_RUNTIME}s reps, drop_caches every rep"
for r in $(seq "$BIMODAL_REPS"); do w9b briefs "cold.$r" "$BIMODAL_RUNTIME" 0; done

rm -f "$MNT/w34.bin"
timeout 300 umount "$MNT" || umount -l "$MNT" 2>/dev/null || true

# ext4: same two modes, fresh fs + span file.
mkdir -p "$RES/ext4"
setup_ext4
prep_span ext4
log "bimodality: ext4, ${BIMODAL_RUNTIME}s reps, warm cache (no drops)"
for r in $(seq "$BIMODAL_REPS"); do w9b ext4 "warm.$r" "$BIMODAL_RUNTIME" 1; done
log "bimodality: ext4, ${BIMODAL_RUNTIME}s reps, drop_caches every rep"
for r in $(seq "$BIMODAL_REPS"); do w9b ext4 "cold.$r" "$BIMODAL_RUNTIME" 0; done

rm -f "$MNT/w34.bin"
timeout 300 umount "$MNT" || umount -l "$MNT" 2>/dev/null || true
rmmod briefs_fs 2>/dev/null || true

dmesg > "$RES/dmesg-end.txt"
log "stage0 $RUNID complete"
if [ "$ABORTED" = 1 ]; then
    echo "STAGE0 ABORTED $RUNID" | tee -a "$LOG"
else
    echo "STAGE0 DONE $RUNID" | tee -a "$LOG"
fi