#!/bin/bash
# perf-read-stage0c.sh — capture the TRUE fast-mode W9b profile + per-call
# briefs_crc32c durations in both modes.
#
# Stage 0b mislabeled its "fastmode" profile: profile() ran a NEW fio
# invocation, and the mode flip is per-invocation — both of stage 0b's
# perf.data files are slow mode (921k ios / 30 s = ~30.7k IOPS, identical
# 132K samples).  Corrections since:
#   - taskset probe across all 8 vCPUs on an AGED module instance: every
#     vCPU slow (31-34k), even the first invocation on a fresh mount.
#     So the fast state follows the MODULE instance (fresh insmod = fast
#     for its first workload), not the mount, not the CPU.
#
# This script, as root in the perf VM:
#   A. rmmod + insmod + mount (no re-prep; on-disk fs is known-good),
#      then perf record -g DURING the first W9b invocation (TRUE fast
#      profile), and during the second (slow profile).  fio's own rate
#      lands in the .fio.out files so each profile's mode is verifiable.
#   B. fresh module again, then ftrace function_graph on briefs_crc32c
#      during the first (fast) and second (slow) 6 s invocations, the
#      trace streamed live to an awk that computes count/mean/max per
#      call duration.  Ratio of mean durations is the answer to
#      "same CRC calls but 3x slower per call?".
# Prints "STAGE0C DONE <runid>".
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
BLD="${BLD:-/tmp/briefssrc}"
BIG="${BIG:-32G}"
RUNID="stage0c-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
LOG="$RES/run.log"
mkdir -p "$RES"

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; echo "STAGE0C ABORTED $RUNID" | tee -a "$LOG"; exit 1; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -f "$BLD/briefs_fs.ko" ] || die "no module at $BLD/briefs_fs.ko"

reload_module() {
    timeout 120 umount "$MNT" 2>/dev/null || true
    rmmod briefs_fs 2>/dev/null || true
    insmod "$BLD/briefs_fs.ko" || die "insmod failed"
    mkdir -p "$MNT"
    mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount failed"
}

w9b() {
    # w9b <name> <runtime> <outfile>
    fio --name="$1" --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime="$2" \
        --filename="$MNT/w34.bin" > "$3" 2>&1
}

rate_of() {
    awk '/IOPS=/ { s=$0; sub(/.*IOPS=/, "", s); mult=1; if (s ~ /^[0-9.]+k/) mult=1000; sub(/[kK,].*/, "", s); printf "%.0f\n", s * mult; exit }' "$1"
}

# ---- A. true fast + slow perf profiles ---------------------------------------
log "A: fresh module, perf record on invocations 1 (fast) and 2 (slow)"
reload_module
iostat -x 1 "$PERF_DEV" > "$RES/iostat-A.log" 2>&1 &
IOPID=$!
perf record -g -o "$RES/fast.perf.data" -- \
    fio --name=fast --ioengine=io_uring --rw=randread --bs=4k \
    --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime=30 \
    --filename="$MNT/w34.bin" > "$RES/fast.fio.out" 2>&1 \
    || log "  fast perf record exited nonzero"
log "  fast invocation rate: $(rate_of "$RES/fast.fio.out") iops"
perf record -g -o "$RES/slow.perf.data" -- \
    fio --name=slow --ioengine=io_uring --rw=randread --bs=4k \
    --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime=30 \
    --filename="$MNT/w34.bin" > "$RES/slow.fio.out" 2>&1 \
    || log "  slow perf record exited nonzero"
log "  slow invocation rate: $(rate_of "$RES/slow.fio.out") iops"
kill "$IOPID" 2>/dev/null || true

# ---- B. ftrace per-call briefs_crc32c durations ------------------------------
log "B: fresh module, function_graph on briefs_crc32c, invocations 1 and 2"
reload_module
T=/sys/kernel/tracing
echo 16384 > "$T/buffer_size_kb" 2>/dev/null || true
echo function_graph > "$T/current_tracer"
echo briefs_crc32c > "$T/set_graph_function" || die "cannot set graph function"

trace_crc() {
    # trace_crc <label> — dump durations of one 6 s W9b invocation, then
    # summarize.  NB: trace_pipe never EOFs, so the reader is killed and
    # the raw dump post-processed (an awk END over the pipe never fires).
    # NB: out=/raw= must be separate local statements — all RHS expansions
    # in one local happen before any assignment (same gotcha as stage0).
    local label="$1"
    local raw="$RES/ftrace-$label.raw"
    echo 1 > "$T/tracing_on"
    cat "$T/trace_pipe" > "$raw" &
    local cpid=$!
    w9b "ft-$label" 6 "$RES/ftrace-$label.fio.out"
    echo 0 > "$T/tracing_on"
    sleep 2
    kill "$cpid" 2>/dev/null || true
    awk '/briefs_crc32c\(\)/ { d = $2 + 0; u = $3
             if (u == "us") { } else if (u == "ms") { d *= 1000 }
             else if (u == "ns") { d /= 1000 }
             n++; sum += d; if (d > max) max = d }
         END { if (n > 0) printf "calls=%d mean=%.3fus max=%.3fus\n", n, sum / n, max }' \
        "$raw" > "$RES/ftrace-$label.stats"
    log "  $label: $(cat "$RES/ftrace-$label.stats") rate=$(rate_of "$RES/ftrace-$label.fio.out") iops"
}

trace_crc fast
trace_crc slow

echo nop > "$T/current_tracer"
echo > "$T/set_graph_function" 2>/dev/null || true
echo 1024 > "$T/buffer_size_kb" 2>/dev/null || true

# ---- C. module-aging sanity probes -------------------------------------------
# After B the fresh module has served two workloads: a 12 s probe should be
# slow, confirming module aging is what stage0c's taskset result showed.
w9b aged1 12 "$RES/aged1.fio.out"
log "aged (post-workload) rate: $(rate_of "$RES/aged1.fio.out") iops"

# ---- D. one more flip confirmation on a fresh module ---------------------------
reload_module
w9b fresh1 12 "$RES/fresh1.fio.out"
log "fresh module, first invocation rate: $(rate_of "$RES/fresh1.fio.out") iops"

timeout 300 umount "$MNT" 2>/dev/null || true
dmesg > "$RES/dmesg-end.txt"
log "stage0c $RUNID complete"
echo "STAGE0C DONE $RUNID" | tee -a "$LOG"