#!/bin/bash
# perf-vs-other-fs.sh — BrieFS vs ext2/ext4/xfs/btrfs/jfs comparison round.
#
# Plan: ~/src/briefs-notes/briefs-perf-vs-other-fs-plan.md (drafted 2026-09-24).
# Run INSIDE the perf VM as root, detached, when the round is green-lit:
#
#   sudo setsid nohup bash /vagrant/tests/xfstests/runs/perf-vs-other-fs.sh \
#       > /tmp/perf-launch.log 2>&1 &
#
# The script writes its own log under $RESULTS/<runid>/run.log and prints the
# sentinel "PERF DONE <runid>" (or "PERF ABORTED") at the very end.
#
# Environment:
#   stock Debian 6.12.107+deb13-amd64, 16 GiB RAM / 8 vCPUs.
#   PERF_DEV  (/dev/vdd)      64 GiB raw libvirt volume, cache=none — the disk
#                             every filesystem is mkfs'd onto in turn.
#   STAGE     (/mnt/stage)    /dev/vdc1 ext4 — W7 tarball staging.
#   RESULTS   (/xfstests/perf-results)  NFS to the host, raw fio JSONs land here.
#
# Methodology (from the plan):
#   - defaults + noatime everywhere, no tuning games; every mkfs/mount logged;
#   - sync + drop_caches before every measurement;
#   - fio in this VM (3.39) has no seed option; default randrepeat=1 makes
#     every invocation with the same job spec draw the IDENTICAL random
#     sequence, so all filesystems and all reps see the same offsets
#     (stronger comparability, at the cost of independent draws per rep);
#   - every fio call capped by $FIO_TIMEOUT so a wedge only costs one data
#     point, not the round.
#
# Deviations from the plan's original workload table, forced by the 16 GiB
# upsizing (recorded here so the write-up can cite them):
#   - W3/W4/W5/W8 are runtime-bound (time_based) instead of size-bound: at 32G
#     spans, size-bound 4k jobs are 64M Is per rep and unbounded in wall time
#     across filesystems with very different rates. Runtime-bound gives each
#     filesystem the same wall time and fio reports steady-state IOPS/MB/s.
#   - W10 parallel-create uses W10_FILES (20000) files total — the scaling
#     shape, not the absolute rate, is the point — and 2 reps.
#
# Env knobs (defaults in parens): FS_LIST(ext2 ext4 xfs btrfs jfs briefs),
# REPS(3), MICRO_REPS(5), W10_REPS(2), W6_FILES(50000), W10_FILES(20000),
# BIG(32G), FIO_TIMEOUT(7200), W3A_RUNTIME(120), W34_RUNTIME(300),
# W5_RUNTIME(60), W8_RUNTIME(120), W9_RUNTIME(120).

set -u

export PATH=/usr/sbin:/sbin:/usr/local/sbin:/usr/bin:/bin:/usr/local/bin

PERF_DEV="${PERF_DEV:-/dev/vdd}"
STAGE="${STAGE:-/mnt/stage}"
MNT="${MNT:-/mnt/perf}"
RESULTS="${RESULTS:-/xfstests/perf-results}"
FS_LIST="${FS_LIST:-ext2 ext4 xfs btrfs jfs briefs}"
REPS="${REPS:-3}"
MICRO_REPS="${MICRO_REPS:-5}"
W10_REPS="${W10_REPS:-2}"
FIO_TIMEOUT="${FIO_TIMEOUT:-7200}"
W3A_RUNTIME="${W3A_RUNTIME:-120}"
W34_RUNTIME="${W34_RUNTIME:-300}"
W5_RUNTIME="${W5_RUNTIME:-60}"
W8_RUNTIME="${W8_RUNTIME:-120}"
W9_RUNTIME="${W9_RUNTIME:-120}"
W6_FILES="${W6_FILES:-50000}"
W10_FILES="${W10_FILES:-20000}"
BRIEFS_MODULE="${BRIEFS_MODULE:-/tmp/briefssrc/briefs_fs.ko}"
MKFS_BRIEFS="${MKFS_BRIEFS:-/go/bin/mkfs.briefs}"
TARBALL="$STAGE/linux-source-6.12.tar.xz"
BIG="${BIG:-32G}"      # >= 2x the 16 GiB RAM, per the plan's working-set rule

log() { echo "[$(date '+%F %T')] $*" >> "$LOG"; }

die() { echo "FATAL: $*" >> "$LOG"; echo "FATAL: $*" >&2; ABORTED=1; exit 1; }

drop_caches() {
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

# run_fio <label> <rep> <extra fio args...>  — one measured fio invocation.
run_fio() {
    local label="$1" rep="$2"; shift 2
    drop_caches
    log "  fio $label rep=$rep starting"
    if timeout "$FIO_TIMEOUT" fio --output-format=json \
            --output="$RES/$FS/$label.$rep.json" "$@" \
            >> "$RES/$FS/$label.$rep.fio.out" 2>&1; then
        log "  fio $label rep=$rep done"
    else
        log "  fio $label rep=$rep FAILED/TIMEOUT (exit $?), continuing"
    fi
}

# timed_phase <text-result-file> <label> <command...> — wall-clock a shell
# phase (W6/W7/W10); seconds and the command are appended to the file.
timed_phase() {
    local txtf="$1" label="$2"; shift 2
    local t0 t1 rc
    drop_caches
    t0=$(date +%s.%N)
    "$@" >> "$RES/$FS/$label.fio.out" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    echo "$label seconds=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}') rc=$rc" \
        >> "$txtf"
    log "  $label rc=$rc ($(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.1f", b-a}')s)"
    return $rc
}

mkfs_one() {
    case "$FS" in
        ext2)   mkfs.ext2 -F "$PERF_DEV" ;;
        ext4)   mkfs.ext4 -F "$PERF_DEV" ;;
        xfs)    mkfs.xfs -f "$PERF_DEV" ;;
        btrfs)  mkfs.btrfs -f "$PERF_DEV" ;;
        jfs)    yes | mkfs.jfs -q "$PERF_DEV" ;;
        briefs) "$MKFS_BRIEFS" -f "$PERF_DEV" ;;
        *)      return 1 ;;
    esac
}

# ---- workloads --------------------------------------------------------------
# Each takes no arguments and reads $MNT, $RES/$FS, $FS, $BIG.

w1_w2_large_file() {
    for r in $(seq "$REPS"); do
        # W1a sequential write 32G, direct
        run_fio W1a.direct_write "$r" --name=w1a --rw=write --bs=1M \
            --size="$BIG" --direct=1 --iodepth=1 --filename="$MNT/w1.bin"
        # W2a sequential read, direct
        run_fio W2a.direct_read "$r" --name=w2a --rw=read --bs=1M \
            --size="$BIG" --direct=1 --iodepth=1 --filename="$MNT/w1.bin"
        # W2b sequential read, buffered (drop_caches inside run_fio)
        run_fio W2b.buffered_read "$r" --name=w2b --rw=read --bs=1M \
            --size="$BIG" --direct=0 --iodepth=1 --filename="$MNT/w1.bin"
        # W1b sequential write 32G, buffered
        run_fio W1b.buffered_write "$r" --name=w1b --rw=write --bs=1M \
            --size="$BIG" --direct=0 --iodepth=1 --filename="$MNT/w1.bin"
    done
    rm -f "$MNT/w1.bin"
    drop_caches
}

w3_w4_w9_random() {
    # Span file: 32G, written once per FS, untimed.
    drop_caches
    log "  preparing $BIG span file w34.bin (untimed)"
    timeout "$FIO_TIMEOUT" fio --name=prep --rw=write --bs=1M \
        --size="$BIG" --direct=1 --iodepth=1 --filename="$MNT/w34.bin" \
        > /dev/null 2>&1 || log "  span-file prep FAILED/TIMEOUT"
    for r in $(seq "$REPS"); do
        # W3 random read 4k, qd1 then qd32
        run_fio W3a.randread_4k_qd1 "$r" --name=w3a --rw=randread --bs=4k \
            --size="$BIG" --direct=1 --iodepth=1 \
            --time_based --runtime="$W3A_RUNTIME" --filename="$MNT/w34.bin"
        run_fio W3b.randread_4k_qd32 "$r" --name=w3b --rw=randread --bs=4k \
            --size="$BIG" --direct=1 --iodepth=32 \
            --time_based --runtime="$W34_RUNTIME" --filename="$MNT/w34.bin"
        # W9 optional extras, same span file
        run_fio W9a.mmap_randread "$r" --name=w9a --ioengine=mmap \
            --rw=randread --bs=4k --size="$BIG" --iodepth=1 \
            --time_based --runtime="$W9_RUNTIME" --filename="$MNT/w34.bin"
        run_fio W9b.iouring_dio_qd64 "$r" --name=w9b --ioengine=io_uring \
            --rw=randread --bs=4k --size="$BIG" --direct=1 --iodepth=64 \
            --time_based --runtime="$W9_RUNTIME" --filename="$MNT/w34.bin"
        # W4 random write 4k qd32 — last in each rep, it dirties the span file
        run_fio W4.randwrite_4k_qd32 "$r" --name=w4 --rw=randwrite --bs=4k \
            --size="$BIG" --direct=1 --iodepth=32 \
            --time_based --runtime="$W34_RUNTIME" --filename="$MNT/w34.bin"
    done
    rm -f "$MNT/w34.bin"
    drop_caches
}

w5_sync_write() {
    for r in $(seq "$MICRO_REPS"); do
        # W5a 4k buffered write, fdatasync every op — the known-weak spot
        run_fio W5a.fdatasync_4k "$r" --name=w5a --rw=write --bs=4k \
            --size="$BIG" --fdatasync=1 --direct=0 \
            --time_based --runtime="$W5_RUNTIME" --filename="$MNT/w5.bin"
        # W5b 1M buffered write, fdatasync every op
        run_fio W5b.fdatasync_1m "$r" --name=w5b --rw=write --bs=1M \
            --size="$BIG" --fdatasync=1 --direct=0 \
            --time_based --runtime="$W5_RUNTIME" --filename="$MNT/w5.bin"
    done
    rm -f "$MNT/w5.bin"
    drop_caches
}

w8_append_log() {
    for r in $(seq "$REPS"); do
        rm -f "$MNT/w8.bin"
        # sequential 4k writes, fdatasync every 100 blocks (journal shape)
        run_fio W8.logwrite_4k "$r" --name=w8 --rw=write --bs=4k \
            --size="$BIG" --fdatasync=100 --direct=0 \
            --time_based --runtime="$W8_RUNTIME" --filename="$MNT/w8.bin"
    done
    rm -f "$MNT/w8.bin"
    drop_caches
}

w6_small_files() {
    local txtf="$RES/$FS/W6.small_files.txt"
    : > "$txtf"
    for r in $(seq "$MICRO_REPS"); do
        local d="$MNT/w6.$r"
        rm -rf "$d"; mkdir -p "$d"
        timed_phase "$txtf" "W6.create.$r" \
            fio --name=create --directory="$d" --nrfiles="$W6_FILES" \
                --filesize=4k --create_only=1
        echo "W6.create.$r files=$(find "$d" -type f | wc -l) target=$W6_FILES" \
            >> "$txtf"
        timed_phase "$txtf" "W6.rm.$r" rm -rf "$d"
    done
}

w10_parallel() {
    local txtf="$RES/$FS/W10.parallel.txt"
    : > "$txtf"
    local total="$W10_FILES"
    for r in $(seq "$W10_REPS"); do
        for J in 4 8; do
            local d="$MNT/w10.$r.$J" per=$((total / J)) t0 t1 k
            rm -rf "$d"; mkdir -p "$d"
            for k in $(seq "$J"); do mkdir -p "$d/j$k"; done
            drop_caches
            t0=$(date +%s.%N)
            for k in $(seq "$J"); do
                fio --name=create --directory="$d/j$k" --nrfiles="$per" \
                    --filesize=4k --create_only=1 \
                    > "$RES/$FS/W10.create.j$J.$r.job$k.fio.out" 2>&1 &
            done
            wait
            t1=$(date +%s.%N)
            echo "W10.create.$r jobs=$J files=$(find "$d" -type f | wc -l) \
target=$total seconds=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')" \
                >> "$txtf"
            rm -rf "$d"
            # parallel 4k random DIO write: J single-job fio processes, one
            # file per job (with numjobs fio would share one file across
            # jobs, conflating per-inode contention with FS scaling)
            drop_caches
            t0=$(date +%s.%N)
            for k in $(seq "$J"); do
                fio --name=w10w --rw=randwrite --bs=4k --direct=1 \
                    --iodepth=8 --size=4G --time_based \
                    --runtime="$W9_RUNTIME" \
                    --filename="$MNT/w10w.$r.$J.$k" \
                    --output-format=json \
                    --output="$RES/$FS/W10.randwrite_4k_j$J.$r.job$k.json" \
                    > "$RES/$FS/W10.randwrite.fio.out" 2>&1 &
            done
            wait
            t1=$(date +%s.%N)
            rm -f "$MNT/w10w."*
            echo "W10.randwrite.$r jobs=$J seconds=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')" \
                >> "$txtf"
            log "  W10 randwrite j$J rep=$r done"
            drop_caches
        done
    done
}

w7_untar() {
    local txtf="$RES/$FS/W7.untar.txt"
    : > "$txtf"
    local d
    for r in $(seq "$REPS"); do
        timed_phase "$txtf" "W7.untar.$r" \
            tar -xJf "$TARBALL" -C "$MNT"
        d="$MNT/linux-source-6.12"
        echo "W7.untar.$r files=$(find "$d" -type f | wc -l)" >> "$txtf"
        timed_phase "$txtf" "W7.traverse.$r" \
            bash -c "find '$d' -type f | wc -l > /dev/null"
        timed_phase "$txtf" "W7.rm.$r" rm -rf "$d"
    done
}

# ---- main -------------------------------------------------------------------

RUNID="run-$(date +%Y%m%d-%H%M%S)"
RES="$RESULTS/$RUNID"
mkdir -p "$RES"
LOG="$RES/run.log"
ABORTED=0

log "perf-vs-other-fs round $RUNID starting"

[ "$(id -u)" -eq 0 ] || die "must run as root"
[ -b "$PERF_DEV" ] || die "$PERF_DEV is not a block device"
[ -f "$TARBALL" ] || die "W7 tarball $TARBALL missing"
[ -f "$BRIEFS_MODULE" ] || die "BrieFS module $BRIEFS_MODULE missing"
mkdir -p "$MNT"

# Environment snapshot — what exactly these numbers describe.
{
    echo "date: $(date -Is)"
    echo "uname: $(uname -a)"
    echo "cpus: $(nproc)"
    free -h
    echo "fio: $(fio --version)"
    echo "device: $(lsblk -nd -o NAME,SIZE,TYPE,ROTA "$PERF_DEV")"
    echo "results-root: $RES"
    echo "fs-list: $FS_LIST"
    echo "reps: $REPS micro=$MICRO_REPS w10=$W10_REPS"
    echo "big: $BIG  device: $PERF_DEV"
    /usr/sbin/modinfo "$BRIEFS_MODULE" | grep -E '^(version|vermagic)'
    for t in ext2 ext4 xfs btrfs jfs; do
        printf '%s: %s\n' "$t" \
            "$(mkfs.$t --version 2>&1 | tr '\n' ' ' || true)"
    done
    printf 'briefs mkfs: %s\n' "$("$MKFS_BRIEFS" --version 2>&1 | head -1)"
} > "$RES/environment.txt" 2>&1
dmesg > "$RES/dmesg-start.txt"

log "loading filesystem modules"
for m in ext4 ext2 xfs btrfs jfs; do modprobe "$m" 2>/dev/null || true; done
insmod "$BRIEFS_MODULE" || die "insmod $BRIEFS_MODULE failed"
log "briefs module loaded: $(/usr/sbin/modinfo -F version "$BRIEFS_MODULE")"

for FS in $FS_LIST; do
    log "=== filesystem $FS starting"
    mkdir -p "$RES/$FS"
    dmesg > "$RES/$FS/dmesg-fs-start.txt"

    timeout 120 umount "$MNT" 2>/dev/null || true
    log "wipefs -a $PERF_DEV"
    wipefs -a "$PERF_DEV" >> "$RES/$FS/mkfs.log" 2>&1
    sleep 1

    log "mkfs: $FS on $PERF_DEV"
    if ! mkfs_one >> "$RES/$FS/mkfs.log" 2>&1; then
        log "mkfs $FS FAILED, skipping filesystem"
        dmesg > "$RES/$FS/dmesg-fs-end.txt"
        continue
    fi

    log "mount: -t $FS -o noatime $PERF_DEV $MNT"
    if ! timeout 120 mount -t "$FS" -o noatime "$PERF_DEV" "$MNT"; then
        log "mount $FS FAILED, skipping filesystem"
        dmesg > "$RES/$FS/dmesg-fs-end.txt"
        continue
    fi

    w1_w2_large_file
    w3_w4_w9_random
    w5_sync_write
    w8_append_log
    w6_small_files
    w10_parallel
    w7_untar

    rm -f "$MNT"/w*.bin
    if ! timeout 300 umount "$MNT"; then
        log "umount $FS timed out — lazy unmount"
        umount -l "$MNT" 2>/dev/null || true
    else
        log "umount $FS clean"
    fi
    dmesg > "$RES/$FS/dmesg-fs-end.txt"
    log "=== filesystem $FS done"
done

rmmod briefs_fs 2>/dev/null || log "rmmod briefs_fs FAILED (module busy?)"
dmesg > "$RES/dmesg-end.txt"
log "round $RUNID complete"
echo "PERF DONE $RUNID" | tee -a "$LOG"