#!/bin/bash
# BrieFS xfstests per-test runner.
#
# xfstests mounts TEST_DEV once per ./check invocation and does NOT reformat it
# between tests; SCRATCH_DEV is reformatted by _scratch_mkfs, but TEST_DIR
# accumulates files/inodes from tests that leave residue or fail cleanup.  That
# poisons bulk-run tallies (e.g. generic/001 exhausting the fixed inode table
# and causing an ENOSPC cascade for hundreds of later tests).
#
# This script runs each requested test in isolation: unmount both devices,
# mkfs.briefs them, mount TEST_DEV, and run exactly one xfstest.  It is the
# standard way to get trustworthy BrieFS pass/fail numbers.
#
# Usage (inside the VM, as root or via sudo):
#   bash /vagrant/tests/xfstests/run-suite.sh generic/001 generic/003 ...
#   bash /vagrant/tests/xfstests/run-suite.sh $(awk '/^[^#]/ {print "generic/"$1}' /xfstests/tests/generic/group.list)
#
# The script uses /xfstests/configs/briefs.config written by setup-vm.sh.
set -uo pipefail

: "${XFSTESTS_DIR:=/xfstests}"
: "${MKFS_BRIEFS_PROG:=/go/bin/mkfs.briefs}"
: "${FSCK_BRIEFS_PROG:=/go/bin/fsck.briefs}"
: "${HOST_OPTIONS:=configs/briefs.config}"
# Set FSCK_ENABLED=1 to run fsck after each test (slower, catches on-disk bugs)
: "${FSCK_ENABLED:=0}"
# Log directory - use /var/tmp so logs survive reboots
: "${LOG_DIR:=/var/tmp/xfstests-logs}"
# Mount command: defaults to the kernel mount; set MOUNT_CMD=fuse-briefs-mount
# (via run-suite-fuse.sh) to test against the Go FUSE bridge instead.
: "${MOUNT_CMD:=mount -t briefs}"
: "${UMOUNT_CMD:=umount}"
# Per-run archive directory (defaults to <this script's dir>/runs).  After
# each run that processes at least ARCHIVE_MIN tests, run-suite.sh writes a
# snapshot of the pass/fail/not-run/skipped totals plus per-test lists there
# so full-suite results persist in the repo and can be diffed across runs.
# Set ARCHIVE_FORCE=1 to archive a shorter run; ARCHIVE_MIN=1 (default)
# archives every run so no result is lost.
: "${RESULTS_DIR:=}"
: "${ARCHIVE_MIN:=1}"
: "${ARCHIVE_FORCE:=0}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
[ -n "$RESULTS_DIR" ] || RESULTS_DIR="$SCRIPT_DIR/runs"

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/sbin:/usr/bin:/bin:${PATH}"
export HOST_OPTIONS

# Create log directory and clean old logs (>7 days old)
mkdir -p "$LOG_DIR"
find "$LOG_DIR" -type f -mtime +7 -delete 2>/dev/null || true

cd "$XFSTESTS_DIR"

# xfstests config files contain a [briefs] section header; source only the
# simple export lines we need and ignore the section marker.
eval "$(grep -E "^(export )?(TEST_DEV|SCRATCH_DEV|TEST_DIR|SCRATCH_MNT)=" "$HOST_OPTIONS" | sed "s/^export //")"

TEST_DEV="${TEST_DEV:-/dev/loop0}"
SCRATCH_DEV="${SCRATCH_DEV:-/dev/loop1}"
TEST_MNT="${TEST_DIR:-/mnt/briefs-test}"
SCRATCH_MNT="${SCRATCH_MNT:-/mnt/briefs-scratch}"

# Ensure module is loaded (kernel-mount runs only).  A FUSE run must NOT
# auto-load the kernel module: a silently-loaded module makes accidental
# kernel mounts succeed, which is exactly the failure mode that invalidated
# the 2026-08-06 FUSE results (tests mounting via the kernel instead of the
# fuse.briefs bridge looked like passes).
case "$MOUNT_CMD" in
*fuse*) : ;;
*) modprobe briefs_fs 2>/dev/null || insmod "/lib/modules/$(uname -r)/extra/briefs/briefs_fs.ko" 2>/dev/null || true ;;
esac

# Remove any device-mapper devices that wrap TEST_DEV or SCRATCH_DEV.  Tests
# such as generic/475 create dm-error/dm-thin-pool/dm-log-writes stacks on top
# of the scratch device; if the test is interrupted they stay behind and make
# the underlying loop device busy (mount fails with EBUSY / "Can't open
# blockdev").  Removing them restores the loop device for the next test.
cleanup_dm_for_device() {
    local dev="$1"
    local real_dev
    real_dev="$(realpath -e "$dev" 2>/dev/null)" || return 0

     # Find DM devices that have $dev as a slave and remove them.
    local slave
    slave="$(basename "$real_dev")"
    for slave_dir in /sys/block/dm-*/slaves; do
        [ -d "$slave_dir" ] || continue
        if [ -e "$slave_dir/$slave" ]; then
            local dm_name dm_num
            dm_num="$(basename "${slave_dir%/slaves}")"
            dm_name="$(cat "/sys/block/$dm_num/dm/name" 2>/dev/null)" || continue
            [ -n "$dm_name" ] || continue
            # Best-effort unmount first, then remove the DM device.
            umount "/dev/mapper/$dm_name" 2>/dev/null || true
            umount "/dev/$dm_num" 2>/dev/null || true
            dmsetup remove "$dm_name" >/dev/null 2>&1 || true
        fi
    done
}

PASS=0
FAIL=0
NOTRUN=0
HANG=0
MKFS_FAIL=0
MOUNT_FAIL=0
SKIPPED=0
UNKNOWN=0
FSCK_WARN=0
RESUMED=0
# Default per-test timeout budget (seconds); override in the environment to
# measure tests suspected of exceeding it without editing get_timeout (which
# is the durable fix for a test that legitimately needs longer).
: "${TIMEOUT_SECS:=300}"

# Canonical per-test outcome: each test lands in exactly one category, recorded
# here.  The counters above still drive the printed summary (for continuity with
# older logs); the run archive derives its totals from STATUS, so the archive is
# always internally consistent (categories sum to tests_processed).  Category
# tokens: PASS FAIL NOTRUN SKIPPED HANG MOUNT MKFS UNKNOWN RESUMED.
# The explicit `=()` matters: bash (5.2, set -u) treats a declared-but-
# never-assigned associative array as unbound, so ${#STATUS[@]} in
# write_archive crashed a zero-test run ("STATUS: unbound variable").
declare -A STATUS=()
# fsck warnings are an annotation, not a run category: a test that passes can
# still trip a post-test fsck, so keep these separate from STATUS.
FSCK_WARN_TESTS=()

# Per-test timeout overrides for known long-running soak tests.
# These tests do 1M+ operations and need more than the default 300s.
get_timeout() {
    local testname="$1"
    case "$testname" in
        generic/089) echo 3600 ;;   # bulk fsx + many small files (~65 min)
        generic/127) echo 1200 ;;   # 6x concurrent fsx (mmap variants)
        generic/521) echo 1200 ;;   # 1M-op DIO fsx soak
        generic/522) echo 1200 ;;   # 1M-op buffered fsx soak
        generic/013) echo 1200 ;;   # 3x fsstress-1000; bridge syncs journal +
                                    # fdatasyncs the device per op, so each
                                    # phase is ~4x kernel time; 300s KILLed a
                                    # healthy run mid-phase-1 (2026-09-07)
        generic/017) echo 900 ;;    # 10k nested fcollapse ops (collect/rebuild O(E)/op)
        generic/011) echo 900 ;;    # dirstress (concurrent dir ops)
        generic/475) echo 900 ;;    # dm-error crash-replay
        generic/299) echo 900 ;;    # fio AIO/DIO + falloc stress: test ~114s
                                    # but post-test unmount checkpoint of the
                                    # ENOSPC-full scratch fs adds ~170s; 300s
                                    # was marginal (intermittent HANGs on the
                                    # ddcb7ef baseline too, 2026-09-01)
        generic/074) echo 900 ;;    # fsx 1M-op mmap writeback soak; 442s solo
                                    # (2026-09-10) vs 300s KILL in the 09-12
                                    # full run; solo PASS under the 900s budget
                                    # (run-20260912-120632)
        generic/476) echo 900 ;;    # whole-device fdatasync soak of the barrier
                                    # family; flaky-hang record (run #1 HANG,
                                    # run #2 PASS, run #3 HANG at 293s — just
                                    # under the 300s default); same shape
                                    # 074/642/750 presented before their
                                    # budgets were raised
        generic/642) echo 900 ;;    # same shape as 074; 421s solo (2026-09-10);
                                   # solo PASS under the 900s budget
                                   # (run-20260912-120632)
        generic/750) echo 900 ;;    # fsx 1M-op DIO soak, first measured under
                                    # fix D in the 09-12 full run (300s KILL);
                                    # solo PASS under the 900s budget
                                    # (run-20260912-120632)
        generic/410) echo 900 ;;    # raised after the 09-12 full-run 300s HANG.
                                    # Solo measurement (run-20260912-120632)
                                    # showed the budget was never the issue: the
                                    # test completed but FAILed on the mount
                                    # wrapper scraping its --bind/--make-*
                                    # invocations.  Fixed (bind/propagation
                                    # passthrough in fuse-briefs-mount); solo
                                    # PASS run-20260912-124627.  Kept at 900s —
                                    # it stacks several fsstress rounds on
                                    # multiple simultaneous mounts
        *)           echo "$TIMEOUT_SECS" ;;
    esac
}

# Unmount a mount point aggressively.  Tests can leave daemons, lazy-unmount
# the device themselves, or hold references in other ways; a plain umount is
# not enough for a reliable per-test loop.
force_umount() {
    local mnt="$1"
    local i

    # Nothing to do if it is not currently mounted.
    mountpoint -q "$mnt" 2>/dev/null || return 0

    # Kill any userspace processes still using the mount.
    if command -v fuser >/dev/null 2>&1; then
        fuser -km "$mnt" >/dev/null 2>&1 || true
        sleep 0.5
    fi

    # Normal, lazy, and forced unmount attempts.
    for i in 1 2 3; do
        umount "$mnt" 2>/dev/null && return 0
        umount -l "$mnt" 2>/dev/null && return 0
        umount -f "$mnt" 2>/dev/null && return 0
        sleep 1
    done

    # If it is still mounted, give up; the caller will report the problem.
    return 1
}

# Wait for any fuse.briefs process servicing @mnt to finish its checkpoint
# and exit.  The fuse-briefs-mount wrapper records the PID in a file; if the
# mount was killed (crash test) or the PID file is missing, this is a no-op.
wait_fuse_exit() {
    local mnt="$1"
    local pidfile="/tmp/fuse-briefs-$(echo "$mnt" | tr / _).pid"
    [ -f "$pidfile" ] || return 0
    local pid
    pid=$(cat "$pidfile" 2>/dev/null || true)
    if [ -n "$pid" ]; then
        local i
        for i in $(seq 1 50); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -9 "$pid" 2>/dev/null || true
    fi
    rm -f "$pidfile"
}

# Write a per-run archive to $RESULTS_DIR (defaults to <this script's
# dir>/runs) so full-suite pass/fail/skip results persist in the repo and can
# be diffed across runs to track progress and regressions.  One file per run:
# run-<timestamp>-<kernel|fuse>.txt.  Each test is in exactly one STATUS
# category, so the archive totals always sum to tests_processed.
write_archive() {
    local processed=${#STATUS[@]}
    if [ "${ARCHIVE_FORCE:-0}" != "1" ] && [ "$processed" -lt "${ARCHIVE_MIN:-1}" ]; then
        echo "  (no archive written: $processed tests processed < ARCHIVE_MIN=${ARCHIVE_MIN:-1})"
        return 0
    fi

    local mtype=kernel
    case "$MOUNT_CMD" in *fuse*) mtype=fuse ;; esac

    mkdir -p "$RESULTS_DIR"
    local out="$RESULTS_DIR/run-${RUN_TIMESTAMP}-${mtype}.txt"
    local seq=1
    while [ -e "$out" ]; do
        out="$RESULTS_DIR/run-${RUN_TIMESTAMP}-${mtype}-${seq}.txt"
        seq=$((seq + 1))
    done

    local kver branch commit
    kver="$(uname -r)"
    branch="$(git -C "$SCRIPT_DIR" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
    commit="$(git -C "$SCRIPT_DIR" rev-parse --short HEAD 2>/dev/null || echo unknown)"

    # Count tests in a STATUS category.
    count_cat() {
        local n=0 t
        for t in "${!STATUS[@]}"; do [ "${STATUS[$t]}" = "$1" ] && n=$((n + 1)); done
        echo "$n"
    }
    # Print a grouped, space-wrapped list of test numbers for one category.
    emit_group() {
        local nums=() t
        for t in "${!STATUS[@]}"; do [ "${STATUS[$t]}" = "$1" ] && nums+=("$t"); done
        printf '# %s (%d)\n' "$2" "${#nums[@]}"
        if [ "${#nums[@]}" -gt 0 ]; then
            printf '%s\n' "${nums[@]}" | sort -n | xargs -n 16
        fi
        echo
    }

    local pass fail notrun skipped hang mfail kfail unk resumed fwarn
    pass=$(count_cat PASS); fail=$(count_cat FAIL); notrun=$(count_cat NOTRUN)
    skipped=$(count_cat SKIPPED); hang=$(count_cat HANG); mfail=$(count_cat MOUNT)
    kfail=$(count_cat MKFS); unk=$(count_cat UNKNOWN); resumed=$(count_cat RESUMED)
    fwarn=${#FSCK_WARN_TESTS[@]}

    if {
        echo "# BrieFS xfstests run archive"
        echo "# Auto-generated by tests/xfstests/run-suite.sh.  Commit full-run"
        echo "# archives to track progress/regressions; compare with diff-runs.sh."
        echo ""
        echo "run_id:           $RUN_TIMESTAMP"
        echo "date:             $(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"
        echo "kernel:           $kver"
        echo "branch:           $branch"
        echo "commit:           $commit"
        echo "mount:            $mtype"
        echo "fstype:           briefs"
        echo "mount_cmd:        $MOUNT_CMD"
        echo "test_dev:         $TEST_DEV"
        echo "scratch_dev:      $SCRATCH_DEV"
        echo "fsck_enabled:     $FSCK_ENABLED"
        echo "fsck_warn:        $fwarn   # annotation, not a category (overlaps the above)"
        echo "tests_processed: $processed"
        echo "skip_list:        ${SKIP_TESTS:-}"
        echo ""
        echo "# Totals (mutually-exclusive categories; sum to tests_processed)"
        printf 'pass: %d\nfail: %d\nnot_run: %d\nskipped: %d\nhang: %d\nmount_fail: %d\nmkfs_fail: %d\nunknown: %d\nresumed: %d\n' \
            "$pass" "$fail" "$notrun" "$skipped" "$hang" "$mfail" "$kfail" "$unk" "$resumed"
        echo ""
        emit_group PASS    "PASS"
        emit_group FAIL    "FAIL"
        emit_group NOTRUN  "NOT RUN"
        emit_group SKIPPED "SKIPPED"
        emit_group HANG    "HANG"
        emit_group MOUNT   "MOUNT FAIL"
        emit_group MKFS    "MKFS FAIL"
        emit_group UNKNOWN "UNKNOWN"
        printf '# FSCK WARN (%d)\n' "$fwarn"
        if [ "$fwarn" -gt 0 ]; then
            printf '%s\n' "${FSCK_WARN_TESTS[@]}" | sort -n | xargs -n 16
        fi
        echo
        echo "# Per-test status (diff-friendly: <testnum> <category>, sorted by number)"
        local t
        for t in "${!STATUS[@]}"; do printf '%s %s\n' "$t" "${STATUS[$t]}"; done | sort -n
    } > "$out"; then
        echo "  -> archive written: $out"
    else
        rm -f "$out"
        echo "  -> archive WRITE FAILED: $out"
    fi
}

# List of tests to skip due to known hangs or unsupported features.
# These tests either wedge the filesystem or test features BrieFS doesn't implement.
# generic/070, generic/224, generic/619: previously skipped for intermittent
# hangs (notes from 2026-08-02/08-04, before Phase 1). Re-verified 2026-08-13
# on current (Phase 1, 134d4a4) code: 40x/20x/20x loop iterations on both the
# stock and lockdep kernels, zero hangs - un-skipped.
# generic/068/074/464/476: documented as fixed in memory and re-verified
# 2026-08-18 at full-suite scale (4/4 PASS, 0 hang, TIMEOUT_SECS=900) -
# un-skipped. 068 = xfs_freeze hang (FIFREEZE/FITHAW c274292); 074 = mmap
# writeback extent leak + AB-BA deadlock (fb649e8 + truncate_setsize fix);
# 464 = trie_iter_grow double-free (4ef6ccb); 476 = all-writes fsstress.
# generic/051/461/753: previously skipped for hangs (notes from 2026-08-04,
# pre-Phase-1). Re-verified 2026-08-18 on current code (3x iterations each,
# SKIP_TESTS="" TIMEOUT_SECS=1800): 051 3/3, 461 3/3, 753 3/3 PASS, zero
# hangs - un-skipped. 051 = xfs_fsr/fsstress + shutdown (FIFREEZE);
# 461 = fsstress + shutdown; 753 = dm-error writeback WARN (fixed 73a0d1d).
# generic/410: mount namespace / propagation test - PASSES (pure VFS shared-
# subtree machinery; BrieFS needs no special support). Un-skipped after
# isolated reproduction confirmed PASS.
# generic/411: mount namespace + concurrent fsstress - PASSES. Was a
# memory-safety bug, not a feature gap: the journal record bounds check in
# __briefs_journal_write_record_locked and the record memcpy were not atomic
# because the back-pressure checkpoint releases j->write_lock, so a concurrent
# writer advanced j->write_offset past the checked value and the memcpy
# overran the 4096-byte cur_block (kmalloc-4k slab-out-of-bounds, pinpointed via
# KASAN: __briefs_journal_write_record_locked+0x263). On a normal kernel the
# overflow smashed the adjacent struct briefs_sb_info -> vfree(garbage) oops ->
# umount died holding s_umount -> every later mount wedged. Fixed by
# re-validating the record bounds after the lock-releasing checkpoint; see
# briefs_journal_flush_cur_block_locked() and the re-check loop in journal.c.
# generic/720: stress-exchange test whose SETUP was the blocker, not the
# exchange.  punch-alternating punches every other block of a 100000-block file
# (50000 holes); BrieFS punch recomputed i_blocks with a full O(E) tree walk on
# every punch, making the setup O(E^2) (~2e13 ops, hours).  Fixed by threading
# the exact blocks-freed count out of briefs_btree_delete_range and decrementing
# i_blocks in O(1).  That exposed a second bug: the internal-node split paths
# (btree_maybe_split_child and btree_ensure_root_room) read idx[mid].high_key
# AFTER zeroing the tail, pushing a 0 separator up and corrupting the tree
# (fsck "separator high_key not strictly ascending" + exchange -EEXIST).  Both
# fixed; 720 now passes (58s) and is un-skipped.
# generic/492: online filesystem label set/get ioctls.  BrieFS implements
# FS_IOC_{GET,SET}FSLABEL correctly - every xfs_io label operation in the test
# passes (set/get/clear/persist-after-remount/max-length/too-long).  The ONLY
# failing lines are the two `blkid -s LABEL $SCRATCH_DEV` invocations, which
# emit nothing because libblkid (util-linux) has no BrieFS filesystem probe and
# so cannot identify BrieFS or read its on-disk label.  The label IS correctly
# on disk (the kernel ioctl reads it back); this is purely a userspace
# recognition gap, not a BrieFS defect, and no kernel change can fix it.  Fix =
# add a libblkid superblocks probe (magic 0x504C434E "PLCN" at dev off 0; uuid
# at sb off 152; label[64] at sb off 312) to util-linux and install it in the VM
# - deferred as out-of-tree-upstream-unlikely work in a different project.
# Tests to skip due to known hangs or unsupported features.  Overridable via the
# environment (e.g. run-fuse-subset.sh exports SKIP_TESTS="" to run the FUSE
# subset, which includes generic/475, in full).  Use the "+set" test so an
# explicitly empty SKIP_TESTS is honored (a plain := would re-apply this default
# to an empty value).
[ -n "${SKIP_TESTS+set}" ] || SKIP_TESTS="generic/475 generic/492"

should_skip() {
    local test="$1"
    local skip
    for skip in $SKIP_TESTS; do
        [ "$test" = "$skip" ] && return 0
    done
    return 1
}

# Timestamp for this run
RUN_TIMESTAMP="$(date +%Y%m%d-%H%M%S)"
# Ensure log directory exists
mkdir -p "$LOG_DIR"

# Resume support: skip tests that have already been run
RESUME_FROM="${RESUME_FROM:-}"
SKIP_UNTIL_DONE=false

for testname in "$@"; do
    testbase="${testname##*/}"

    # Resume support: skip until we find the test we left off at
    if [ -n "$RESUME_FROM" ] && [ "$SKIP_UNTIL_DONE" = true ]; then
        if [ "$testname" = "$RESUME_FROM" ]; then
            SKIP_UNTIL_DONE=false
        else
            echo "  -> SKIPPED (resume from $RESUME_FROM)"
            RESUMED=$((RESUMED + 1))
            STATUS["$testbase"]=RESUMED
            continue
        fi
    fi

    # Check if this test was already run (log exists)
    if ls "$LOG_DIR"/check-${testbase}-*.log >/dev/null 2>&1; then
        echo "  -> SKIPPED (already run)"
        RESUMED=$((RESUMED + 1))
        STATUS["$testbase"]=RESUMED
        continue
    fi

    echo "========================================"
    echo "  $testname"
    echo "========================================"

    # Skip known hanging/unsupported tests.
    if should_skip "$testname"; then
        echo "  -> SKIPPED (known hang/unsupported)"
        SKIPPED=$((SKIPPED + 1))
        STATUS["$testbase"]=SKIPPED
        continue
    fi

    # Maximum aggression cleanup BEFORE the test to prevent SCRATCH_DEV issues.
    # Tests may leave SCRATCH_DEV mounted, busy, or in RO state.

    # First, kill any processes using the mount points.
    if command -v fuser >/dev/null 2>&1; then
        fuser -km "$TEST_MNT" >/dev/null 2>&1 || true
        fuser -km "$SCRATCH_MNT" >/dev/null 2>&1 || true
        sleep 1
    fi

    # Force unmount everything, multiple times with increasing aggression.
    for i in 1 2 3 4 5; do
        umount "$TEST_MNT" 2>/dev/null && break
        umount -l "$TEST_MNT" 2>/dev/null && break
        umount -f "$TEST_MNT" 2>/dev/null && break
        umount -f -l "$TEST_MNT" 2>/dev/null && break
        umount -R "$TEST_MNT" 2>/dev/null && break
        sleep 1
    done

    for i in 1 2 3 4 5; do
        umount "$SCRATCH_MNT" 2>/dev/null && break
        umount -l "$SCRATCH_MNT" 2>/dev/null && break
        umount -f "$SCRATCH_MNT" 2>/dev/null && break
        umount -f -l "$SCRATCH_MNT" 2>/dev/null && break
        umount -R "$SCRATCH_MNT" 2>/dev/null && break
        sleep 1
    done

    cleanup_dm_for_device "$TEST_DEV"
    cleanup_dm_for_device "$SCRATCH_DEV"

    # Extra cleanup: remove any leftover files from interrupted tests.
    if mountpoint -q "$TEST_MNT" 2>/dev/null; then
        rm -rf "${TEST_MNT:?}"/* 2>/dev/null || true
        rm -rf "${TEST_MNT:?}"/.* 2>/dev/null || true
    fi
    if mountpoint -q "$SCRATCH_MNT" 2>/dev/null; then
        rm -rf "${SCRATCH_MNT:?}"/* 2>/dev/null || true
        rm -rf "${SCRATCH_MNT:?}"/.* 2>/dev/null || true
    fi

    # Force sync and wait for pending writes to complete.
    # This prevents "device RO" issues from prior test's writeback.
    sync
    sleep 2

    # Final check: if SCRATCH_DEV still appears mounted anywhere, force detach.
    # Check by both device and mount point.
    if grep -qE "$SCRATCH_DEV|$SCRATCH_MNT" /proc/mounts 2>/dev/null; then
        umount -f -l -R "$SCRATCH_MNT" 2>/dev/null || true
        sleep 2
    fi
    if grep -qE "$TEST_DEV|$TEST_MNT" /proc/mounts 2>/dev/null; then
        umount -f -l -R "$TEST_MNT" 2>/dev/null || true
        sleep 2
    fi

    # Ensure any fuse.briefs process from the previous test has exited (its
    # journal checkpoint) before mkfs.briefs reformats the device.  Kill any
    # stray processes first (a crashed test may leave fuse.briefs running with
    # no PID file).
    pkill -9 -f fuse.briefs 2>/dev/null || true
    sleep 0.5
    wait_fuse_exit "$TEST_MNT"
    wait_fuse_exit "$SCRATCH_MNT"

    if ! "$MKFS_BRIEFS_PROG" -f "$TEST_DEV" >/dev/null 2>&1; then
        echo "  -> MKFS TEST FAIL"
        MKFS_FAIL=$((MKFS_FAIL + 1))
        STATUS["$testbase"]=MKFS
        continue
    fi
    if ! "$MKFS_BRIEFS_PROG" -f "$SCRATCH_DEV" >/dev/null 2>&1; then
        echo "  -> MKFS SCRATCH FAIL"
        MKFS_FAIL=$((MKFS_FAIL + 1))
        STATUS["$testbase"]=MKFS
        continue
    fi

    # Mount TEST_DEV; SCRATCH_DEV is mounted by the test itself.
    if ! $MOUNT_CMD "$TEST_DEV" "$TEST_MNT" 2>&1 | tee "$LOG_DIR/mount-err-${testbase}-${RUN_TIMESTAMP}.log"; then
        echo "  -> MOUNT FAIL"
        MOUNT_FAIL=$((MOUNT_FAIL + 1))
        STATUS["$testbase"]=MOUNT
        # Try to leave things as clean as possible for the next test.
        force_umount "$TEST_MNT" || true
        force_umount "$SCRATCH_MNT" || true
        continue
    fi

    status=0
    # Run without -b briefs so result files land in results/generic/ and the
    # existing generic golden outputs are used.
    tsecs=$(get_timeout "$testname")
    # SIGKILL, not the default SIGTERM: bash defers an untrapped SIGTERM while
    # it waits for a foreground child, so a wedged test (e.g. fsstress stuck in
    # D-state on the mount) never dies and the "timeout" runs forever
    # (2026-09-07, generic/013: 300s timeout still alive 65min later).  A KILLed
    # timeout exits 137, handled as HANG below alongside the classic 124.
    timeout -s KILL "$tsecs" ./check "$testname" 2>&1 | tee "$LOG_DIR/check-${testbase}-${RUN_TIMESTAMP}.log"
    status=${PIPESTATUS[0]}

    # Clean up mounts before moving on, best effort.
    # Be aggressive: tests may leave devices mounted or in RO state.
    force_umount "$TEST_MNT" || true
    force_umount "$SCRATCH_MNT" || true

    # Lazy unmount as fallback (detaches mount point immediately).
    umount -l "$TEST_MNT" 2>/dev/null || true
    umount -l "$SCRATCH_MNT" 2>/dev/null || true

    cleanup_dm_for_device "$TEST_DEV"
    cleanup_dm_for_device "$SCRATCH_DEV"

    # Force sync to flush any pending writes before next test's mkfs.
    sync
    sleep 1

    # Optional fsck validation (enabled via FSCK_ENABLED=1).
    # Runs after each test to catch on-disk consistency bugs early.
    if [ "$FSCK_ENABLED" = "1" ] && [ "$status" -ne 124 ] && [ "$status" -ne 137 ]; then
        if "$FSCK_BRIEFS_PROG" -n "$TEST_DEV" 2>&1 | tee "$LOG_DIR/fsck-${testbase}-${RUN_TIMESTAMP}.log"; then
            : # fsck clean
        else
            echo "  -> FSCK WARN"
            FSCK_WARN=$((FSCK_WARN + 1))
            FSCK_WARN_TESTS+=("$testbase")
        fi
    fi

    # 124 = SIGTERM'd by timeout; 137 = SIGKILL'd (see the -s KILL note above).
    if [ "$status" -eq 124 ] || [ "$status" -eq 137 ]; then
        echo "  -> HANG (timeout)"
        HANG=$((HANG + 1))
        STATUS["$testbase"]=HANG
        continue
    fi

    # Parse xfstests' own summary lines.  They are more reliable than guessing
    # from result-file existence, and they correctly distinguish a test that
    # passed from one that was entirely not-run.
    TEST_LOG="$LOG_DIR/check-${testbase}-${RUN_TIMESTAMP}.log"
    if [ -f "$TEST_LOG" ]; then
        if grep -qE "^Failures: (generic/)?${testbase}(\s|$)" "$TEST_LOG"; then
            echo "  -> FAIL"
            FAIL=$((FAIL + 1))
            STATUS["$testbase"]=FAIL
        elif grep -qE "^Not run: (generic/)?${testbase}(\s|$)" "$TEST_LOG"; then
            echo "  -> NOT RUN"
            NOTRUN=$((NOTRUN + 1))
            STATUS["$testbase"]=NOTRUN
        elif grep -qE "Passed all [1-9][0-9]* tests" "$TEST_LOG"; then
            echo "  -> PASS"
            PASS=$((PASS + 1))
            STATUS["$testbase"]=PASS
        elif grep -qE "Passed all 0 tests" "$TEST_LOG"; then
            # Test was interrupted or could not run (e.g. mount failure inside
            # ./check itself); count it as a failure, not a pass.
            echo "  -> FAIL (interrupted, 0 tests passed)"
            FAIL=$((FAIL + 1))
            STATUS["$testbase"]=FAIL
        else
            # Ambiguous result (e.g. ./check aborted before printing a summary).
            echo "  -> UNKNOWN (exit $status)"
            UNKNOWN=$((UNKNOWN + 1))
            STATUS["$testbase"]=UNKNOWN
        fi
    else
        # No log file - test hung or crashed before completing
        echo "  -> HANG (no output)"
        HANG=$((HANG + 1))
        STATUS["$testbase"]=HANG
    fi
done

echo ""
echo "========================================"
echo "  BrieFS per-test run complete"
echo "  PASS:       $PASS"
echo "  FAIL:       $FAIL"
echo "  NOT RUN:    $NOTRUN"
echo "  SKIPPED:    $SKIPPED"
echo "  HANG:       $HANG"
echo "  MKFS FAIL:  $MKFS_FAIL"
echo "  MOUNT FAIL: $MOUNT_FAIL"
echo "  UNKNOWN:    $UNKNOWN"
echo "  FSCK WARN:  $FSCK_WARN"
echo "  RESUMED:    $RESUMED"
if [ "$FSCK_ENABLED" = "1" ]; then
    echo "  (fsck validation enabled)"
fi
echo "========================================"

write_archive
