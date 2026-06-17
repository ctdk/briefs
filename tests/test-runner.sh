#!/bin/bash
# BrieFS Test Runner
# Run from host: ./tests/test-runner.sh [vm-ssh-command]
# Run from VM:   sudo ./tests/test-runner.sh
#
# Tests are compiled into a single script and executed inside the VM.
# Returns 0 on all-pass, 1 on any failure.

set -euo pipefail

SSH_CMD="${1:-}"
TEST_IMG="${TMPDIR:-/tmp}/briefs-test-$$.img"
MNT_POINT="/tmp/briefs-mnt-$$"
KERNEL_MODULE="${BRIEFS_MODULE:-/home/jeremy/src/briefs/briefs_fs.ko}"
MKBRIEFS="${BRIEFS_MKFS:-/home/jeremy/go/bin/mkfs.briefs}"
FSCKBRIEFS="${BRIEFS_FSCK:-/home/jeremy/go/bin/fsck.briefs}"
LOSETUP="${BRIEFS_LOSETUP:-$(command -v losetup || echo /sbin/losetup)}"
LOSETUP_DISCOVER="${BRIEFS_LOSETUP_DISCOVER:-true}"

# If SSH_CMD given, wrap everything in SSH
if [ -n "$SSH_CMD" ]; then
  exec $SSH_CMD "bash -s" < "$0"
  exit 1
fi

# ---- VM-side only from here ----
PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { local extra="${2:-}"; echo "  FAIL: $1 $extra"; FAIL=$((FAIL + 1)); }
check_cmd() {
  local desc="$1" expected="$2"
  shift 2
  if "$@" 2>/dev/null; then actual=0; else actual=$?; fi
  if [ "$actual" = "$expected" ]; then pass "$desc"; else fail "$desc" "(expected $expected, got $actual)"; fi
}
check_str() {
  local desc="$1" expected="$2"
  shift 2
  local actual
  actual="$("$@" 2>/dev/null || true)"
  if [ "$actual" = "$expected" ]; then pass "$desc"; else fail "$desc" "(expected '$expected', got '$actual')"; fi
}
check_file() {
  local desc="$1" path="$2" expected="$3"
  local actual
  actual="$(cat "$path" 2>/dev/null || true)"
  if [ "$actual" = "$expected" ]; then pass "$desc"; else fail "$desc" "(expected '$expected', got '$actual')"; fi
}

cleanup() {
  umount "$MNT_POINT" 2>/dev/null || true
  if [ -n "${BRIEFS_LOOP_DEV:-}" ]; then
    $LOSETUP -d "$BRIEFS_LOOP_DEV" 2>/dev/null || true
    unset BRIEFS_LOOP_DEV
  fi
  rmmod briefs_fs 2>/dev/null || true
  rm -f "$TEST_IMG"
  rmdir "$MNT_POINT" 2>/dev/null || true
}
trap cleanup EXIT

# Build mkfs if needed (no go in VM — use pre-built from host)
if [ ! -x "$MKBRIEFS" ]; then
  # Try to use cached mkfs from /go
  cd /go/src/github.com/ctdk/briefs-utils || fail "No briefs-utils found"
  go build -o "$MKBRIEFS" ./cmd/mkfs 2>/dev/null || fail "Cannot build mkfs.briefs"
fi

# Build fsck if needed
if [ ! -x "$FSCKBRIEFS" ]; then
  cd /go/src/github.com/ctdk/briefs-utils || fail "No briefs-utils found"
  go build -o "$FSCKBRIEFS" ./cmd/fsck 2>/dev/null || fail "Cannot build fsck.briefs"
fi

echo "=== BrieFS Test Suite ==="
echo "Kernel: $(uname -r)"
date

# Phase 0: Module load
echo ""
echo "=== Phase 0: Module Setup ==="
# Force-reload the module to get the latest build
# First: clean up ALL leftover briefs mounts and loop devices
MOUNTS=$(cat /proc/mounts 2>/dev/null | grep briefs | awk '{print $1 " " $2}' || true)
if [ -n "$MOUNTS" ]; then
  echo "$MOUNTS" | while read dev mnt; do
    fuser -km "$mnt" 2>/dev/null || true
    umount "$mnt" 2>/dev/null || true
  done
fi
$LOSETUP -D 2>/dev/null || true
sleep 1
rmmod briefs_fs 2>/dev/null || true
insmod "$KERNEL_MODULE" && pass "briefs_fs module loaded" || fail "module load failed"

# Phase 1: mkfs
echo ""
echo "=== Phase 1: mkfs ==="
SIZE=8192
"$MKBRIEFS" -s "$SIZE" "$TEST_IMG" 2>/dev/null && pass "mkfs.briefs creates image" || fail "mkfs failed"
[ -f "$TEST_IMG" ] && pass "image file exists" || fail "image file missing"

# Phase 2: Mount
echo ""
echo "=== Phase 2: Mount ==="
mkdir -p "$MNT_POINT"
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "mount succeeded" || fail "mount failed"

# Phase 3: Basic file operations
echo ""
echo "=== Phase 3: File Operations ==="

# Write and read back
echo -n "HelloWorld" > "$MNT_POINT/hello"
check_file "write+read 'HelloWorld'" "$MNT_POINT/hello" "HelloWorld"

# Append
echo -n "Appended" >> "$MNT_POINT/hello"
check_file "append" "$MNT_POINT/hello" "HelloWorldAppended"

# Hole read (read from sparse file offset — should be zeros)
dd if=/dev/zero bs=4096 count=1 of="$MNT_POINT/sparse" 2>/dev/null
SPARSE_SIZE=$(stat -c%s "$MNT_POINT/sparse" 2>/dev/null || echo 0)
[ "$SPARSE_SIZE" = "4096" ] && pass "sparse file size" || fail "sparse file size" "(expected 4096, got $SPARSE_SIZE)"
if od -An -tx1 -N4096 "/sparse" 2>/dev/null | tr -d ' 
' | tr -d '0' | grep -q '.'; then
  fail "sparse file all zeros"
else
  pass "sparse file all zeros"
fi

# Truncate
truncate -s 5 "$MNT_POINT/hello" 2>/dev/null && pass "truncate to 5" || fail "truncate"
check_file "truncated content" "$MNT_POINT/hello" "Hello"

# Larger file (multi-block, requires extent chain)
dd if=/dev/zero bs=4096 count=20 of="$MNT_POINT/large" 2>/dev/null
FSIZE=$(stat -c%s "$MNT_POINT/large" 2>/dev/null || echo 0)
[ "$FSIZE" = 81920 ] && pass "large file (20 blocks, $FSIZE bytes)" || fail "large file size" "(expected 81920, got $FSIZE)"

# Regression test: force overflow to a chain block by writing 9 non-contiguous
# single-block extents, then extend one of them and remount. This exercises
# the append path on an inode that has chain blocks, which is where the
# checksum-mismatch race was observed.
CHAIN_TEST="$MNT_POINT/chain_extents"
for chain_off in 0 2 4 6 8 10 12 14 16; do
  dd if=/dev/urandom bs=4096 count=1 of="$CHAIN_TEST" seek="$chain_off" conv=notrunc 2>/dev/null || true
done
FSIZE=$(stat -c%s "$CHAIN_TEST" 2>/dev/null || echo 0)
[ "$FSIZE" = 69632 ] && pass "chain-block file size" || fail "chain-block file size" "(expected 69632, got $FSIZE)"
# Append a block just past the last sparse offset to exercise chain-block append.
echo -n "tail-block" >> "$CHAIN_TEST"
FSIZE=$(stat -c%s "$CHAIN_TEST" 2>/dev/null || echo 0)
[ "$FSIZE" = 69642 ] && pass "chain-block append size" || fail "chain-block append size" "(expected 69642, got $FSIZE)"
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount for chain-block replay" || fail "remount for chain-block replay"
FSIZE=$(stat -c%s "$CHAIN_TEST" 2>/dev/null || echo 0)
[ "$FSIZE" = 69642 ] && pass "chain-block file survives replay" || fail "chain-block file survives replay" "(got $FSIZE)"

# Inline data tests
SMALL="$(printf 'x%.0s' $(seq 1 100))"
echo -n "$SMALL" > "$MNT_POINT/inline_small"
[ "$(stat -c%s "$MNT_POINT/inline_small")" -eq 100 ] && pass "inline file size 100" || fail "inline file size" "(got $(stat -c%s "$MNT_POINT/inline_small"))"
check_file "inline file readback" "$MNT_POINT/inline_small" "$SMALL"

# Append within the inline region
echo -n " tail" >> "$MNT_POINT/inline_small"
[ "$(stat -c%s "$MNT_POINT/inline_small")" -eq 105 ] && pass "inline append size" || fail "inline append size" "(got $(stat -c%s "$MNT_POINT/inline_small"))"
check_file "inline append readback" "$MNT_POINT/inline_small" "${SMALL} tail"

# Truncate an inline file to a smaller inline size and to zero
truncate -s 50 "$MNT_POINT/inline_small"
[ "$(stat -c%s "$MNT_POINT/inline_small")" -eq 50 ] && pass "inline truncate to 50" || fail "inline truncate to 50" "(got $(stat -c%s "$MNT_POINT/inline_small"))"
check_file "inline truncate readback" "$MNT_POINT/inline_small" "$(printf 'x%.0s' $(seq 1 50))"

truncate -s 0 "$MNT_POINT/inline_small"
[ "$(stat -c%s "$MNT_POINT/inline_small")" -eq 0 ] && pass "inline truncate to zero" || fail "inline truncate to zero" "(got $(stat -c%s "$MNT_POINT/inline_small"))"
echo -n "reborn" > "$MNT_POINT/inline_small"
check_file "inline file after rewrite" "$MNT_POINT/inline_small" "reborn"

LONGER="$(printf 'y%.0s' $(seq 1 260))"
echo -n "$LONGER" > "$MNT_POINT/inline_promote"
[ "$(stat -c%s "$MNT_POINT/inline_promote")" -eq 260 ] && pass "promoted inline file size 260" || fail "promoted inline file size" "(got $(stat -c%s "$MNT_POINT/inline_promote"))"
check_file "promoted inline file readback" "$MNT_POINT/inline_promote" "$LONGER"

# Promotion via append: start small and append past the 256-byte threshold
APPEND_BASE="$(printf 'z%.0s' $(seq 1 200))"
echo -n "$APPEND_BASE" > "$MNT_POINT/inline_append_promote"
echo -n "$(printf 'z%.0s' $(seq 1 100))" >> "$MNT_POINT/inline_append_promote"
[ "$(stat -c%s "$MNT_POINT/inline_append_promote")" -eq 300 ] && pass "append-promote file size 300" || fail "append-promote file size" "(got $(stat -c%s "$MNT_POINT/inline_append_promote"))"
check_file "append-promote readback" "$MNT_POINT/inline_append_promote" "$(printf 'z%.0s' $(seq 1 300))"

# Truncate an inline file past the inline threshold: should promote and zero-fill.
TRUNC_BASE="$(printf 'w%.0s' $(seq 1 200))"
echo -n "$TRUNC_BASE" > "$MNT_POINT/inline_trunc_promote"
truncate -s 300 "$MNT_POINT/inline_trunc_promote"
[ "$(stat -c%s "$MNT_POINT/inline_trunc_promote")" -eq 300 ] && pass "truncate-promote size 300" || fail "truncate-promote size" "(got $(stat -c%s "$MNT_POINT/inline_trunc_promote"))"
printf '%s' "$TRUNC_BASE" > "$MNT_POINT/expected_trunc"
dd if=/dev/zero bs=1 count=100 >> "$MNT_POINT/expected_trunc" 2>/dev/null
cmp "$MNT_POINT/expected_trunc" "$MNT_POINT/inline_trunc_promote" && pass "truncate-promote content" || fail "truncate-promote content"

sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount for inline replay" || fail "remount for inline replay"
check_file "inline file after replay" "$MNT_POINT/inline_small" "reborn"
check_file "promoted inline file after replay" "$MNT_POINT/inline_promote" "$LONGER"
check_file "append-promote file after replay" "$MNT_POINT/inline_append_promote" "$(printf 'z%.0s' $(seq 1 300))"

# Phase 4: Directory operations
echo ""
echo "=== Phase 4: Directory Operations ==="

mkdir "$MNT_POINT/subdir" && pass "mkdir" || fail "mkdir"
[ -d "$MNT_POINT/subdir" ] && pass "directory exists" || fail "directory missing"
echo -n "nested" > "$MNT_POINT/subdir/file" && pass "write file in subdir" || fail "write in subdir"
check_file "read file in subdir" "$MNT_POINT/subdir/file" "nested"

# Phase 5: Rename
echo ""
echo "=== Phase 5: Rename ==="
mv "$MNT_POINT/hello" "$MNT_POINT/goodbye" 2>/dev/null && pass "rename file" || fail "rename"
[ ! -f "$MNT_POINT/hello" ] && [ -f "$MNT_POINT/goodbye" ] && pass "rename target exists, source gone" || fail "rename check"
check_file "renamed content" "$MNT_POINT/goodbye" "Hello"

# Replace-rename: rename over an existing file. This is the case kconfig's
# conf_write hits (rename .config -> .config.old) and the one that used to
# oops in iput() (BUG_ON(i_state & I_CLEAR)) because briefs_rename did a
# stray iput() of the replaced target's inode.
printf 'replace-source' > "$MNT_POINT/repl_src"
printf 'replace-target' > "$MNT_POINT/repl_tgt"
mv "$MNT_POINT/repl_src" "$MNT_POINT/repl_tgt" 2>/dev/null && pass "rename over existing file" || fail "rename over existing file"
[ ! -f "$MNT_POINT/repl_src" ] && pass "replace-rename source gone" || fail "replace-rename source gone"
check_file "replace-rename target has source content" "$MNT_POINT/repl_tgt" "replace-source"

# Phase 6: Unlink and rmdir with remount/fsck
echo ""
echo "=== Phase 6: Unlink ==="
rm "$MNT_POINT/goodbye" 2>/dev/null && pass "unlink file" || fail "unlink"
[ ! -f "$MNT_POINT/goodbye" ] && pass "unlinked file gone" || fail "unlinked file still exists"

mkdir "$MNT_POINT/rmdir_test" && mkdir "$MNT_POINT/rmdir_test/nested" && echo -n "x" > "$MNT_POINT/rmdir_test/nested/file"
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount for rmdir replay" || fail "remount for rmdir"
rm -rf "$MNT_POINT/rmdir_test" 2>/dev/null && pass "rmdir nested directory" || fail "rmdir"
[ ! -d "$MNT_POINT/rmdir_test" ] && pass "rmdir target gone" || fail "rmdir target still exists"
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount after rmdir" || fail "remount after rmdir"
[ ! -d "$MNT_POINT/rmdir_test" ] && pass "rmdir target still gone after remount" || fail "rmdir target resurrected"

# Phase 6b: Symlink content survives replay
echo ""
echo "=== Phase 6b: Symlinks ==="
ln -s "hello-briefs-symlink" "$MNT_POINT/slink" 2>/dev/null && pass "create small inline symlink" || fail "symlink create"
[ "$(readlink "$MNT_POINT/slink" 2>/dev/null || true)" = "hello-briefs-symlink" ] && pass "small inline symlink readback" || fail "symlink readback"

# Max-length inline symlink target (256 bytes)
MAX_INLINE_TARGET="$(printf 't%.0s' $(seq 1 256))"
ln -s "$MAX_INLINE_TARGET" "$MNT_POINT/slink_max" 2>/dev/null && pass "create max inline symlink" || fail "max inline symlink create"
[ "$(readlink "$MNT_POINT/slink_max" 2>/dev/null || true)" = "$MAX_INLINE_TARGET" ] && pass "max inline symlink readback" || fail "max inline symlink readback"

sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount after symlink" || fail "remount after symlink"
[ "$(readlink "$MNT_POINT/slink" 2>/dev/null || true)" = "hello-briefs-symlink" ] && pass "small inline symlink after replay" || fail "symlink after replay"
[ "$(readlink "$MNT_POINT/slink_max" 2>/dev/null || true)" = "$MAX_INLINE_TARGET" ] && pass "max inline symlink after replay" || fail "max inline symlink after replay"

# Phase 7: Executables
echo ""
echo "=== Phase 7: Executables ==="

cp /bin/true "$MNT_POINT/true_test"
check_cmd "exec /bin/true" 0 "$MNT_POINT/true_test"

cp /bin/echo "$MNT_POINT/echo_test"
check_str "exec /bin/echo" "hello briefs" "$MNT_POINT/echo_test" "hello briefs"

cp /bin/seq "$MNT_POINT/seq_test"
SEQ_OUT=$("$MNT_POINT/seq_test" 3 5 2>/dev/null)
[ "$SEQ_OUT" = "$(printf '3\n4\n5')" ] && pass "exec /bin/seq 3 5" || fail "exec seq" "(got '$SEQ_OUT')"

cp /bin/uname "$MNT_POINT/uname_test"
"$MNT_POINT/uname_test" -a >/dev/null 2>&1 && pass "exec /bin/uname -a" || fail "exec uname"

# Check that the ELF binary we copied in is still runnable after umount/remount
sync
umount "$MNT_POINT"
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null
check_cmd "exec after remount" 0 "$MNT_POINT/true_test"
echo "  (also tests journal replay on mount)"

# Phase 8: stress — create many files in a directory
echo ""
echo "=== Phase 8: Many Files ==="
mkdir "$MNT_POINT/many"
COUNT=0
for i in $(seq 1 50); do
  echo -n "x" > "$MNT_POINT/many/file_$i" 2>/dev/null && COUNT=$((COUNT + 1))
done
[ "$COUNT" = 50 ] && pass "create 50 files" || fail "create 50 files" "(created $COUNT)"
FCOUNT=$(ls "$MNT_POINT/many" 2>/dev/null | wc -l)
[ "$FCOUNT" = 50 ] && pass "readdir 50 entries" || fail "readdir 50 entries" "(got $FCOUNT)"

# Phase 8b: allocator consistency after replay
echo ""
echo "=== Phase 8b: Allocator Consistency ==="
mkdir "$MNT_POINT/alloc_test"
for i in $(seq 1 30); do
  echo -n "x" > "$MNT_POINT/alloc_test/file_$i" 2>/dev/null || true
done
rm $(seq 1 10 | sed "s|^|$MNT_POINT/alloc_test/file_|") 2>/dev/null || true
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount for allocator replay" || fail "remount for allocator replay"
for i in $(seq 31 50); do
  echo -n "y" > "$MNT_POINT/alloc_test/file_$i" 2>/dev/null || true
done
LATER_COUNT=$(ls "$MNT_POINT/alloc_test" 2>/dev/null | wc -l)
[ "$LATER_COUNT" -eq 40 ] && pass "create files after allocator replay" || fail "create files after allocator replay" "(got $LATER_COUNT)"

# Phase 9: chown/chmod
echo ""
echo "=== Phase 9: Permissions ==="
echo -n "permtest" > "$MNT_POINT/permtest"
chmod 0644 "$MNT_POINT/permtest" 2>/dev/null
PMODE=$(stat -c%a "$MNT_POINT/permtest" 2>/dev/null || echo "")
[ "$PMODE" = "644" ] && pass "chmod 0644" || fail "chmod" "(got $PMODE)"
chown --reference=/bin/true "$MNT_POINT/permtest" 2>/dev/null && pass "chown (reference)" || fail "chown"

# Phase 10: statfs
echo ""
echo "=== Phase 10: statfs ==="
STAT=$(stat -f "$MNT_POINT" 2>/dev/null)
[ -n "$STAT" ] && pass "statfs returns output" || fail "statfs"

# Phase 10b: superblock free counts after replay
echo ""
echo "=== Phase 10b: Superblock Free Counts ==="
FREE_BEFORE=$(stat -f "$MNT_POINT" 2>/dev/null | grep -o 'Free: [0-9]*' | awk '{print $2}' || echo "")
for i in $(seq 1 10); do
  echo -n "z" > "$MNT_POINT/statfs_test_$i" 2>/dev/null || true
done
rm "$MNT_POINT"/statfs_test_* 2>/dev/null || true
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount for statfs replay" || fail "remount for statfs"
FREE_AFTER=$(stat -f "$MNT_POINT" 2>/dev/null | grep -o 'Free: [0-9]*' | awk '{print $2}' || echo "")
[ "$FREE_BEFORE" = "$FREE_AFTER" ] && pass "free counts consistent after replay" || fail "free counts inconsistent after replay" "(before=$FREE_BEFORE after=$FREE_AFTER)"

# Phase 11: Punch hole (fallocate -p)
echo ""
echo "=== Phase 11: Punch Hole ==="

# Helper to emit @n copies of a single character.
mkblock() {
  local ch="$1"
  local n="$2"
  printf "$ch%.0s" $(seq 1 "$n")
}

# Aligned punch hole in a 4-block file.
PUNCH_FILE="$MNT_POINT/punch_aligned"
{ mkblock A 4096; mkblock B 4096; mkblock C 4096; mkblock D 4096; } > "$PUNCH_FILE"
PUNCH_SIZE=$(stat -c%s "$PUNCH_FILE")
fallocate -p -o 4096 -l 4096 "$PUNCH_FILE" 2>/dev/null && pass "aligned punch hole" || fail "aligned punch hole"
[ "$(stat -c%s "$PUNCH_FILE")" = "$PUNCH_SIZE" ] && pass "size unchanged after aligned punch" || fail "size unchanged after aligned punch"
[ -z "$(head -c 4096 "$PUNCH_FILE" | tr -d 'A')" ] && pass "pre-hole block preserved" || fail "pre-hole block preserved"
[ -z "$(dd if="$PUNCH_FILE" bs=4096 skip=1 count=1 2>/dev/null | tr -d '\0')" ] && pass "punched block is zero" || fail "punched block is zero"
[ -z "$(dd if="$PUNCH_FILE" bs=4096 skip=2 count=1 2>/dev/null | tr -d 'C')" ] && pass "post-hole block preserved" || fail "post-hole block preserved"

# Unaligned punch hole spanning part of two blocks.
PUNCH_FILE2="$MNT_POINT/punch_unaligned"
{ mkblock A 4096; mkblock B 4096; } > "$PUNCH_FILE2"
fallocate -p -o 100 -l 200 "$PUNCH_FILE2" 2>/dev/null && pass "unaligned punch hole" || fail "unaligned punch hole"
[ -z "$(head -c 100 "$PUNCH_FILE2" | tr -d 'A')" ] && pass "unaligned prefix preserved" || fail "unaligned prefix preserved"
[ -z "$(dd if="$PUNCH_FILE2" bs=1 skip=100 count=200 2>/dev/null | tr -d '\0')" ] && pass "unaligned hole is zero" || fail "unaligned hole is zero"
[ -z "$(dd if="$PUNCH_FILE2" bs=1 skip=300 count=3796 2>/dev/null | tr -d 'A')" ] && pass "unaligned suffix preserved" || fail "unaligned suffix preserved"
[ -z "$(tail -c 4096 "$PUNCH_FILE2" | tr -d 'B')" ] && pass "unaligned second block preserved" || fail "unaligned second block preserved"

# Punch hole that removes an entire single-block extent.
PUNCH_WHOLE="$MNT_POINT/punch_whole"
mkblock X 4096 > "$PUNCH_WHOLE"
PUNCH_WHOLE_SIZE=$(stat -c%s "$PUNCH_WHOLE")
fallocate -p -o 0 -l 4096 "$PUNCH_WHOLE" 2>/dev/null && pass "whole block punch hole" || fail "whole block punch hole"
[ "$(stat -c%s "$PUNCH_WHOLE")" = "$PUNCH_WHOLE_SIZE" ] && pass "size unchanged after whole-block punch" || fail "size unchanged after whole-block punch"
[ -z "$(dd if="$PUNCH_WHOLE" bs=4096 count=1 2>/dev/null | tr -d '\0')" ] && pass "whole-block hole is zero" || fail "whole-block hole is zero"

# Punch hole on an inline-data file.
PUNCH_INLINE="$MNT_POINT/punch_inline"
printf 'ABCDEFGHIJ' > "$PUNCH_INLINE"
fallocate -p -o 2 -l 4 "$PUNCH_INLINE" 2>/dev/null && pass "inline punch hole" || fail "inline punch hole"
INLINE_HEX=$(od -An -tx1 -N10 "$PUNCH_INLINE" | tr -d ' \n')
[ "$INLINE_HEX" = "4142000000004748494a" ] && pass "inline hole content" || fail "inline hole content" "(got $INLINE_HEX)"

sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount after punch hole" || fail "remount after punch hole"
[ -z "$(dd if="$PUNCH_FILE" bs=4096 skip=1 count=1 2>/dev/null | tr -d '\0')" ] && pass "aligned punch survives replay" || fail "aligned punch survives replay"
[ -z "$(dd if="$PUNCH_FILE2" bs=1 skip=100 count=200 2>/dev/null | tr -d '\0')" ] && pass "unaligned punch survives replay" || fail "unaligned punch survives replay"
[ -z "$(dd if="$PUNCH_WHOLE" bs=4096 count=1 2>/dev/null | tr -d '\0')" ] && pass "whole-block punch survives replay" || fail "whole-block punch survives replay"
INLINE_HEX2=$(od -An -tx1 -N10 "$PUNCH_INLINE" | tr -d ' \n')
[ "$INLINE_HEX2" = "4142000000004748494a" ] && pass "inline punch survives replay" || fail "inline punch survives replay"

# Phase 11c: fiemap (FS_IOC_FIEMAP) exposes the file extent map. BrieFS has no
# hole extents, so a punched hole shows up as a *gap* between two real extents
# (the fiemap convention). The probe is embedded so the suite stays
# self-contained when piped to the VM over SSH; it is skipped if there is no
# compiler to build it with.
echo ""
echo "=== Phase 11c: fiemap ==="
FIEMAP_C=/tmp/briefs_fiemap_probe_$$.c
FIEMAP_BIN=/tmp/briefs_fiemap_probe_$$
if ! command -v gcc >/dev/null 2>&1; then
  echo "  (skipped: gcc not available to build the fiemap probe)"
else
  cat > "$FIEMAP_C" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>

static void dump_flags(unsigned int f)
{
	if (f & FIEMAP_EXTENT_LAST)        printf(" LAST");
	if (f & FIEMAP_EXTENT_UNKNOWN)     printf(" UNKNOWN");
	if (f & FIEMAP_EXTENT_DATA_INLINE) printf(" DATA_INLINE");
	if (f & FIEMAP_EXTENT_NOT_ALIGNED) printf(" NOT_ALIGNED");
	if (f & FIEMAP_EXTENT_UNWRITTEN)   printf(" UNWRITTEN");
}

int main(int argc, char **argv)
{
	int fd;
	enum { MAXEXT = 512 };
	size_t sz = sizeof(struct fiemap) + MAXEXT * sizeof(struct fiemap_extent);
	struct fiemap *f;
	unsigned int i;
	long got, expect = -1;

	if (argc < 2) { fprintf(stderr, "usage: %s file [expect_count]\n", argv[0]); return 2; }
	if (argc >= 3) expect = strtol(argv[2], NULL, 10);
	fd = open(argv[1], O_RDONLY);
	if (fd < 0) { perror("open"); return 1; }
	f = malloc(sz);
	if (!f) { perror("malloc"); close(fd); return 1; }
	memset(f, 0, sz);
	f->fm_start = 0;
	f->fm_length = FIEMAP_MAX_OFFSET;
	f->fm_flags = 0;
	f->fm_extent_count = MAXEXT;
	if (ioctl(fd, FS_IOC_FIEMAP, f) < 0) { perror("ioctl"); free(f); close(fd); return 1; }
	got = (long)f->fm_mapped_extents;
	printf("%ld extent(s)\n", got);
	for (i = 0; i < f->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &f->fm_extents[i];
		printf("  [%u] logical=%llu physical=%llu length=%llu flags=0x%x",
		       i, (unsigned long long)e->fe_logical,
		       (unsigned long long)e->fe_physical,
		       (unsigned long long)e->fe_length, e->fe_flags);
		dump_flags(e->fe_flags);
		printf("\n");
	}
	free(f);
	close(fd);
	if (expect >= 0 && got != expect) return 2;
	return 0;
}
EOF
  if gcc -O2 -o "$FIEMAP_BIN" "$FIEMAP_C" 2>/dev/null; then
    pass "fiemap probe builds"

    # (a) A contiguous multi-block file is exactly one extent.
    dd if=/dev/zero bs=4096 count=4 of="$MNT_POINT/fiemap_contig" 2>/dev/null
    "$FIEMAP_BIN" "$MNT_POINT/fiemap_contig" 1 >/dev/null 2>&1 \
      && pass "fiemap: contiguous file is 1 extent" \
      || fail "fiemap: contiguous file is 1 extent" "(expected 1)"

    # (b) Punch the middle block of a 4-block file. With no hole extents the
    #     file drops from 1 extent to 2 with a gap at logical 4096..8192; size
    #     is unchanged (KEEP_SIZE).
    dd if=/dev/zero bs=4096 count=4 of="$MNT_POINT/fiemap_punch" 2>/dev/null
    fallocate -p -o 4096 -l 4096 "$MNT_POINT/fiemap_punch" 2>/dev/null
    [ "$(stat -c%s "$MNT_POINT/fiemap_punch" 2>/dev/null)" = "16384" ] \
      && pass "fiemap: punch keeps size (KEEP_SIZE)" \
      || fail "fiemap: punch keeps size" "(got $(stat -c%s "$MNT_POINT/fiemap_punch" 2>/dev/null || echo '?'))"
    "$FIEMAP_BIN" "$MNT_POINT/fiemap_punch" 2 >/dev/null 2>&1 \
      && pass "fiemap: punched file is 2 extents (hole is a gap)" \
      || fail "fiemap: punched file is 2 extents" "(expected 2)"

    # (c) An inline-data file (<256 bytes) reports one DATA_INLINE extent.
    head -c 100 /dev/zero | tr '\0' 'I' > "$MNT_POINT/fiemap_inline" 2>/dev/null
    FIEMAP_OUT="$("$FIEMAP_BIN" "$MNT_POINT/fiemap_inline" 1 2>/dev/null || true)"
    echo "$FIEMAP_OUT" | grep -q "DATA_INLINE" \
      && pass "fiemap: inline-data extent is DATA_INLINE" \
      || fail "fiemap: inline-data extent is DATA_INLINE" "(got: $(echo "$FIEMAP_OUT" | tail -1))"
  else
    fail "fiemap probe build failed"
  fi
  rm -f "$FIEMAP_C" "$FIEMAP_BIN"
fi

# Phase 12: fsck CRC/structure check
echo ""
echo "=== Phase 12: fsck ==="
sync
# Unmount before fsck so the checker sees a quiescent, consistent image.
umount "$MNT_POINT" 2>/dev/null || true
"$FSCKBRIEFS" "$TEST_IMG" 2>/dev/null && pass "fsck reports no errors" || fail "fsck found errors"

# --- Summary ---
echo ""
echo "=== Results ==="
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
TOTAL=$((PASS + FAIL))
echo "  Total: $TOTAL"

if [ "$FAIL" -gt 0 ]; then
  echo "  *** SOME TESTS FAILED ***"
  exit 1
else
  echo "  All tests passed."
  exit 0
fi