#!/bin/bash
# perf-read-stage0e.sh — clean validation of the W9b mode-flip mechanism,
# with NO instrumentation (rates are the signal).
#
# Established mechanism (stage0b/0c/0d kprobes):
#   BrieFS files lack FMODE_NOWAIT -> every io_uring DIO read punts to
#   io-wq (never inline).  io-wq worker count N is set at ramp-up and
#   rate ~= N x ~30k IOPS (per-IO worker CPU ~35 us, ~29 us of it
#   briefs_crc32c).  N ratchets up only when a worker SLEEPS with work
#   pending; on a fresh superblock the first async DIO read blocks in
#   sb_init_dio_done_wq (lazy per-sb workqueue creation), seeding 2-3
#   workers; every later invocation finds the wq already built -> one
#   worker -> ~33k.
#
# Predictions this script tests (each 15 s W9b, no kprobes, no perf):
#   A. aged module, fresh mount, first invocation     -> 2-3 workers, 57-121k
#   B. second invocation on same mount                -> 1 worker, ~33k
#   C. rmmod+insmod+mount, ONE 4K DIO read first (dd), then invocation
#        -> sb_init_dio_done_wq pre-built -> 1 worker, ~33k  [KEY TEST]
#   D. rmmod+insmod+mount, no dd, invocation          -> 2-3 workers, 57-121k
# Prints "STAGE0E DONE <runid>".
set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
BLD="${BLD:-/tmp/briefssrc}"
BIG="${BIG:-32G}"
RUNID="stage0e-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
LOG="$RES/run.log"
mkdir -p "$RES"

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }
die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; echo "STAGE0E ABORTED $RUNID" | tee -a "$LOG"; exit 1; }

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
    local label="$1"
    local out="$RES/$label.fio.out"
    fio --name="$label" --ioengine=io_uring --rw=randread --bs=4k \
        --size="$BIG" --direct=1 --iodepth=64 --time_based --runtime=15 \
        --filename="$MNT/w34.bin" > "$out" 2>&1
    log "  $label: iops=$(rate_of "$out")"
}

preread() {
    # one synchronous 4K DIO read: initializes sb->s_dio_done_wq before any
    # io-wq read can block on it (sync kiocb -> wait_for_completion path,
    # the same one prep's writes took).
    dd if="$MNT/w34.bin" of=/dev/null bs=4k count=1 iflag=direct \
        >> "$RES/dd.log" 2>&1 || die "preread dd failed"
}

# ---- A. aged module, fresh mount, first invocation ---------------------------
log "A: aged module, fresh mount, first invocation (expect 57-121k)"
timeout 120 umount "$MNT" 2>/dev/null || true
mount -t briefs -o noatime "$PERF_DEV" "$MNT" || die "mount failed"
probe A-fresh-mount

# ---- B. second invocation -----------------------------------------------------
log "B: second invocation (expect ~33k)"
probe B-second

# ---- C. fresh module + preread (KEY: expect ~33k despite fresh mount) ---------
log "C: rmmod+insmod+mount + 4K DIO preread + invocation (expect ~33k)"
reload_module
preread
probe C-preread

# ---- D. fresh module, no preread (expect 57-121k) ------------------------------
log "D: rmmod+insmod+mount, no preread (expect 57-121k)"
reload_module
probe D-no-preread

# ---- E. immediate second invocation on the same fresh mount -------------------
log "E: second invocation after D (expect ~33k)"
probe E-second

timeout 300 umount "$MNT" 2>/dev/null || true
log "stage0e $RUNID complete"
echo "STAGE0E DONE $RUNID" | tee -a "$LOG"