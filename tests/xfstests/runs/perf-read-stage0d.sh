#!/bin/bash
# perf-read-stage0d.sh — io-wq worker-spawn instrumentation for the W9b
# mode flip.
#
# Established so far (stage0/0b/0c):
#   - both modes: reads punt to io-wq workers; per-IO kernel CPU ~10 us,
#     ~82% of it in briefs_crc32c.  Same cost in both modes.
#   - SLOW mode = exactly ONE worker, CPU-bound, queue never drains;
#     io-wq never spawns more (enqueue spawns only at nr_running==0, and
#     the sleep-path spawn needs a worker to block with work pending).
#   - FAST mode = 3 workers (~31% CPU each), i.e. 3x parallelism, and
#     every observed fast invocation ran on a freshly inserted module
#     (after rmmod+insmod) or right after mkfs+prep.
#
# This script, as root in the perf VM, kprobes the spawn and sleep paths
# with stack traces and runs the flip sequence:
#   1. aged module (current state): mount + probe        -> expect SLOW
#   2. rmmod + insmod + mount + probe                    -> expect FAST
#   3. second probe on the fresh module                   -> expect SLOW
# recording for each: io_wq_create_worker spawns (count + stacks),
# io_wq_worker_sleeping events (count + stacks), and buffer_head slab
# residency before/after.  Prints "STAGE0D DONE <runid>".
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
BLD="${BLD:-/tmp/briefssrc}"
BIG="${BIG:-32G}"
RUNID="stage0d-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
LOG="$RES/run.log"
mkdir -p "$RES"

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; echo "STAGE0D ABORTED $RUNID" | tee -a "$LOG"; exit 1; }
snap_bh() { grep "^buffer_head" /proc/slabinfo | awk '{print $2}' | tee -a "$RES/bh-slab-$1.txt" >> "$LOG"; }

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -f "$BLD/briefs_fs.ko" ] || die "no module at $BLD/briefs_fs.ko"

reload_module() {
    timeout 120 umount "$MNT" 2>/dev/null || true
    rmmod briefs_fs 2>/dev/null || true
    insmod "$BLD/briefs_fs.ko" || die "insmod failed"
    mkdir -p "$MNT"
    mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount failed"
}

rate_of() {
    awk '/IOPS=/ { s=$0; sub(/.*IOPS=/, "", s); mult=1; if (s ~ /^[0-9.]+k/) mult=1000; sub(/[kK,].*/, "", s); printf "%.0f\n", s * mult; exit }' "$1"
}

probe() {
    # probe <label> <runtime> — one W9b-shaped invocation with the kprobes
    # live; then dump spawn/sleep counts and stacks per phase.
    local label="$1" rt="$2"
    local out="$RES/$label.fio.out"
    snap_bh "$label.pre"
    echo > /sys/kernel/tracing/trace
    echo 1 > /sys/kernel/tracing/tracing_on
    fio --name="$label" --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime="$rt" \
        --filename="$MNT/w34.bin" > "$out" 2>&1
    echo 0 > /sys/kernel/tracing/tracing_on
    snap_bh "$label.post"
    cp /sys/kernel/tracing/trace "$RES/$label.trace"
    local spawns sleeps decs
    spawns=$(grep -c "wspawn:" "$RES/$label.trace" || true)
    sleeps=$(grep -c "wsleep:" "$RES/$label.trace" || true)
    decs=$(grep -c "wdec:" "$RES/$label.trace" || true)
    # condense the stacks: function frames only, one block per event
    awk '/^ +[a-z_]+-[0-9]+\/[0-9]+\] .*:/ { next }
         { print }' "$RES/$label.trace" > "$RES/$label.trace.condensed" || true
    log "  $label: iops=$(rate_of "$out") spawns=$spawns sleeps=$sleeps decs=$decs bh=$(tail -1 "$RES/bh-$label.pre.txt")->$(tail -1 "$RES/bh-$label.post.txt")"
    log "    spawn stacks:"; grep -A16 "wspawn:" "$RES/$label.trace" | head -40 >> "$LOG"
    log "    sleep stacks (first 3 events):"; grep -A16 "wsleep:" "$RES/$label.trace" | head -60 >> "$LOG"
}

# ---- kprobes: worker spawns and worker sleeps, with stacks -------------------
# NB: io_wq_create_worker is inlined away in this build; create_io_thread is
# the actual spawner (only io-wq creates threads during the probes).
T=/sys/kernel/tracing
echo 16384 > "$T/buffer_size_kb" || die "cannot size trace buffer"
echo > "$T/kprobe_events"
echo 'p:wspawn create_io_thread' > "$T/kprobe_events" || die "kprobe wspawn failed"
echo 'p:wsleep io_wq_worker_sleeping' >> "$T/kprobe_events" || die "kprobe wsleep failed"
echo 'p:wdec io_wq_dec_running' >> "$T/kprobe_events" || die "kprobe wdec failed"
echo 'stacktrace' > "$T/events/kprobes/wspawn/trigger" 2>/dev/null || log "no stack trigger for wspawn"
echo 'stacktrace' > "$T/events/kprobes/wsleep/trigger" 2>/dev/null || log "no stack trigger for wsleep"
echo 'stacktrace' > "$T/events/kprobes/wdec/trigger" 2>/dev/null || log "no stack trigger for wdec"
echo 1 > "$T/events/kprobes/wspawn/enable"
echo 1 > "$T/events/kprobes/wsleep/enable"
echo 1 > "$T/events/kprobes/wdec/enable"
grep -q briefs_fs /proc/modules || die "module not loaded"

# ---- 1. aged module, fresh mount: expect SLOW --------------------------------
log "1: aged module, mount + probe (expect SLOW)"
timeout 120 umount "$MNT" 2>/dev/null || true
mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount failed"
probe aged 15

# ---- 2. rmmod + insmod: expect FAST -------------------------------------------
log "2: rmmod + insmod + mount + probe (expect FAST)"
reload_module
probe fresh1 15

# ---- 3. second probe on the fresh module: expect SLOW --------------------------
log "3: second probe (expect SLOW)"
probe fresh2 15

# ---- cleanup -------------------------------------------------------------------
echo 0 > "$T/events/kprobes/wspawn/enable"
echo 0 > "$T/events/kprobes/wsleep/enable"
echo 0 > "$T/events/kprobes/wdec/enable"
echo > "$T/kprobe_events"
echo 1024 > "$T/buffer_size_kb" 2>/dev/null || true
timeout 300 umount "$MNT" 2>/dev/null || true
dmesg > "$RES/dmesg-end.txt"
log "stage0d $RUNID complete"
echo "STAGE0D DONE $RUNID" | tee -a "$LOG"