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
ln -s "hello-briefs-symlink" "$MNT_POINT/slink" 2>/dev/null && pass "create symlink" || fail "symlink create"
[ "$(readlink "$MNT_POINT/slink" 2>/dev/null || true)" = "hello-briefs-symlink" ] && pass "symlink readback" || fail "symlink readback"
sync
umount "$MNT_POINT" 2>/dev/null || true
mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null && pass "remount after symlink" || fail "remount after symlink"
[ "$(readlink "$MNT_POINT/slink" 2>/dev/null || true)" = "hello-briefs-symlink" ] && pass "symlink after replay" || fail "symlink after replay"

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

# Phase 11: fsck CRC/structure check
echo ""
echo "=== Phase 11: fsck ==="
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