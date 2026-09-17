#!/bin/bash
# perf-write.sh — large-file write-path benchmark.
#
# The regression gate for the write-performance campaign: four write
# shapes over a fresh filesystem, run on BrieFS and ext4 on the same
# device, reported as MiB/s plus (for BrieFS) the -o debug stat-counter
# deltas. Results are appended to tests/perf-write-results.txt; the
# ratio-to-ext4 column is the number later perf work is judged on.
#
# Workloads (all over 1 MiB records unless noted):
#   seq_buffered    dd of=/dev/zero to one file, then fsync the file
#                   (both inside the timed window) — the plain
#                   sequential buffered-write shape.
#   seq_fsync       same file shape, one fsync per 1 MiB record — the
#                   per-op journal surface (dd conv=fsync writes
#                   records then syncs; a per-record fsync is the
#                   linker/O_DSYNC-ish worst case this campaign targets).
#   seq_osync       dd oflag=sync — O_SYNC per 1 MiB record.
#   ld_shape        500 x 1 MiB files, each written then fsynced (a
#                   kernel-build linker's object-file pattern), then
#                   one full readback pass.
#
# Usage (inside the VM, as root):
#   bash /vagrant/tests/perf-write.sh [device] [size_mib] [results_file]
#
#   device        defaults to /dev/vdc1
#   size_mib      workloads 1-3 write this many MiB, default 10240 (10 GiB);
#                 ld_shape is fixed at 500 MiB.  Use ~512 for a smoke run.
#   results_file  appended to, default /vagrant/tests/perf-write-results.txt
#
# The script assumes /vagrant/briefs_fs.ko (or a loaded module) and
# /go/bin/{mkfs,fsck}.briefs.  ext4 comparison uses mke2fs defaults.
set -u

DEV="${1:-/dev/vdc1}"
SIZE_MIB="${2:-10240}"
RESULTS="${3:-/vagrant/tests/perf-write-results.txt}"
MNT=/mnt/perfwrite
MOD=/vagrant/briefs_fs.ko
BS_MIB=1
LD_FILES=500

[ "$(id -u)" = 0 ] || { echo "must run as root" >&2; exit 1; }

fail() { echo "FAIL: $*" >&2; exit 1; }

timed() {
	# timed VAR -- cmd... : wall seconds of cmd into VAR (fd 1 survives).
	local __var=$1 __t0 __t1
	shift
	__t0=$(date +%s.%N)
	"$@"
	__t1=$(date +%s.%N)
	eval "$__var=\$(awk -v a=\"$__t0\" -v b=\"$__t1\" 'BEGIN{printf \"%.3f\", b-a}')"
}

mib_s() {
	# mib_s MIB SECONDS
	awk -v m="$1" -v s="$2" 'BEGIN{if (s+0 <= 0) printf "%s", "n/a"; else printf "%.1f", m/s}'
}

drop_caches() {
	sync
	echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true
}

prepare_fs() {
	# prepare_fs briefs|ext4: fresh mkfs + mount, echo the debugfs path
	# (briefs) or "" (ext4) for stat dumping.
	local type=$1 sid
	umount "$MNT" 2>/dev/null || true
	mkdir -p "$MNT"
	case $type in
	briefs)
		if ! grep -qw briefs /proc/filesystems; then
			[ -f "$MOD" ] || fail "no $MOD and module not loaded"
			insmod "$MOD" || fail "insmod"
		fi
		"$MKFS" -f "$DEV" >/dev/null 2>&1 || fail "mkfs.briefs $DEV"
		mount -t briefs -o debug "$DEV" "$MNT" || fail "mount briefs"
		sid=${DEV#/dev/}
		echo "/sys/kernel/debug/briefs/$sid/stats"
		;;
	ext4)
		mke2fs -q -F "$DEV" >/dev/null 2>&1 || fail "mkfs.ext4 $DEV"
		mount -t ext4 "$DEV" "$MNT" || fail "mount ext4"
		echo ""
		;;
	*) fail "unknown fs type $type" ;;
	esac
}

read_stats() {
	local stats_file=$1
	[ -n "$stats_file" ] && cat "$stats_file" 2>/dev/null
}

run_workloads() {
	local type=$1 tag=$2 stats_file t out
	stats_file=$(prepare_fs "$type")
	echo "### $tag (device $DEV, ${SIZE_MIB} MiB, $(date -u +%FT%TZ))" | tee -a "$RESULTS"
	[ -n "$stats_file" ] && { echo "--- stats after mount" | tee -a "$RESULTS"; read_stats "$stats_file" | tee -a "$RESULTS"; }

	# --- seq_buffered ---------------------------------------------------
	drop_caches
	if [ "$type" = briefs ]; then
		read_stats "$stats_file" > /tmp/perf-stats-before
	fi
	timed t bash -c "dd if=/dev/zero of=$MNT/seq bs=${BS_MIB}M count=$SIZE_MIB status=none && sync"
	{ printf 'seq_buffered: %s s  %s MiB/s\n' "$t" "$(mib_s "$SIZE_MIB" "$t")"; } | tee -a "$RESULTS"
	[ "$type" = briefs ] && { echo "--- stats after seq_buffered (delta from mount)" | tee -a "$RESULTS"; diff /tmp/perf-stats-before <(read_stats "$stats_file") | grep '^>' | tee -a "$RESULTS" || true; read_stats "$stats_file" > /tmp/perf-stats-before; }
	rm -f "$MNT/seq"

	# --- seq_fsync (fsync per 1 MiB record) ------------------------------
	drop_caches
	timed t python3 - "$MNT/seq" "$SIZE_MIB" <<'PY'
import os, sys
mnt, mib = sys.argv[1], int(sys.argv[2])
fd = os.open(mnt, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
chunk = b"\x5a" * (1 << 20)
for i in range(mib):
    os.pwrite(fd, chunk, i << 20)
    os.fsync(fd)
os.close(fd)
PY
	{ printf 'seq_fsync:    %s s  %s MiB/s\n' "$t" "$(mib_s "$SIZE_MIB" "$t")"; } | tee -a "$RESULTS"
	[ "$type" = briefs ] && { echo "--- stats after seq_fsync (delta)" | tee -a "$RESULTS"; diff /tmp/perf-stats-before <(read_stats "$stats_file") | grep '^>' | tee -a "$RESULTS" || true; read_stats "$stats_file" > /tmp/perf-stats-before; }
	rm -f "$MNT/seq"

	# --- seq_osync -------------------------------------------------------
	drop_caches
	timed t bash -c "dd if=/dev/zero of=$MNT/seq bs=${BS_MIB}M count=$SIZE_MIB oflag=sync status=none"
	{ printf 'seq_osync:    %s s  %s MiB/s\n' "$t" "$(mib_s "$SIZE_MIB" "$t")"; } | tee -a "$RESULTS"
	[ "$type" = briefs ] && { echo "--- stats after seq_osync (delta)" | tee -a "$RESULTS"; diff /tmp/perf-stats-before <(read_stats "$stats_file") | grep '^>' | tee -a "$RESULTS" || true; read_stats "$stats_file" > /tmp/perf-stats-before; }
	rm -f "$MNT/seq"

	# --- ld_shape (500 x 1 MiB files, fsync each; then readback) ---------
	drop_caches
	timed t python3 - "$MNT" "$LD_FILES" <<'PY'
import os, sys
mnt, n = sys.argv[1], int(sys.argv[2])
chunk = b"\xa5" * (1 << 20)
for i in range(n):
    fd = os.open(f"{mnt}/obj.{i}", os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    os.write(fd, chunk)
    os.fsync(fd)
    os.close(fd)
PY
	{ printf 'ld_shape:     %s s  %s MiB/s  (%s files)\n' "$t" "$(mib_s "$((LD_FILES * BS_MIB))" "$t")" "$LD_FILES"; } | tee -a "$RESULTS"
	[ "$type" = briefs ] && { echo "--- stats after ld_shape (delta)" | tee -a "$RESULTS"; diff /tmp/perf-stats-before <(read_stats "$stats_file") | grep '^>' | tee -a "$RESULTS" || true; read_stats "$stats_file" > /tmp/perf-stats-before; }

	drop_caches
	timed t python3 - "$MNT" "$LD_FILES" <<'PY'
import os, sys
mnt, n = sys.argv[1], int(sys.argv[2])
for i in range(n):
    fd = os.open(f"{mnt}/obj.{i}", os.O_RDONLY)
    buf = os.read(fd, 1 << 20)
    assert len(buf) == (1 << 20) and buf[0] == 0xA5
    os.close(fd)
PY
	{ printf 'ld_readback:  %s s  %s MiB/s\n' "$t" "$(mib_s "$((LD_FILES * BS_MIB))" "$t")"; } | tee -a "$RESULTS"
	[ "$type" = briefs ] && { echo "--- stats after ld_readback (delta)" | tee -a "$RESULTS"; diff /tmp/perf-stats-before <(read_stats "$stats_file") | grep '^>' | tee -a "$RESULTS" || true; }
	rm -f "$MNT"/obj.*

	umount "$MNT" || fail "umount after $tag"
	echo | tee -a "$RESULTS"
}

echo "== briefs perf-write: device $DEV, ${SIZE_MIB} MiB" | tee -a "$RESULTS"
MKFS=/go/bin/mkfs.briefs
[ -x "$MKFS" ] || fail "no $MKFS"
run_workloads briefs briefs
run_workloads ext4 ext4
echo "== done $(date -u +%FT%TZ)" | tee -a "$RESULTS"