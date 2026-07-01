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

# Phase 6c: Extended attributes (small inline and large chained values)
echo ""
echo "=== Phase 6c: Extended Attributes ==="

set_xattr() {
  local path="$1" size="$2" name="$3"
  python3 - "$path" "$size" "$name" <<'PY'
import sys, os
path = sys.argv[1]
size = int(sys.argv[2])
name = sys.argv[3].encode()
value = os.urandom(size)
os.setxattr(path, name, value)
ref = path + "." + name.decode().replace(".", "_") + ".xattr_ref"
with open(ref, "wb") as f:
    f.write(value)
PY
}

check_xattr() {
  local path="$1" name="$2"
  python3 - "$path" "$name" <<'PY'
import sys, os
path = sys.argv[1]
name = sys.argv[2].encode()
ref = path + "." + name.decode().replace(".", "_") + ".xattr_ref"
with open(ref, "rb") as f:
    expected = f.read()
got = os.getxattr(path, name)
sys.exit(0 if got == expected else 1)
PY
}

remove_xattr() {
  local path="$1" name="$2"
  python3 - "$path" "$name" <<'PY'
import sys, os
path = sys.argv[1]
name = sys.argv[2].encode()
os.removexattr(path, name)
ref = path + "." + name.decode().replace(".", "_") + ".xattr_ref"
try:
    os.remove(ref)
except FileNotFoundError:
    pass
PY
}

touch "$MNT_POINT/xattr_file" 2>/dev/null || true
if set_xattr "$MNT_POINT/xattr_file" 64 "user.small"; then
  pass "set small inline xattr"
else
  fail "set small inline xattr"
fi
if check_xattr "$MNT_POINT/xattr_file" "user.small"; then
  pass "read small inline xattr"
else
  fail "read small inline xattr"
fi

if set_xattr "$MNT_POINT/xattr_file" 60000 "user.big"; then
  pass "set large chained xattr"
else
  fail "set large chained xattr"
fi
if check_xattr "$MNT_POINT/xattr_file" "user.big"; then
  pass "read large chained xattr"
else
  fail "read large chained xattr"
fi

sync
umount "$MNT_POINT" 2>/dev/null || true
if mount -o loop "$TEST_IMG" "$MNT_POINT" 2>/dev/null; then
  pass "remount after xattr"
else
  fail "remount after xattr"
fi
if check_xattr "$MNT_POINT/xattr_file" "user.small"; then
  pass "small xattr survives replay"
else
  fail "small xattr after replay"
fi
if check_xattr "$MNT_POINT/xattr_file" "user.big"; then
  pass "large xattr survives replay"
else
  fail "large xattr after replay"
fi

if remove_xattr "$MNT_POINT/xattr_file" "user.big"; then
  pass "remove large xattr"
else
  fail "remove large xattr"
fi
if remove_xattr "$MNT_POINT/xattr_file" "user.small"; then
  pass "remove small xattr"
else
  fail "remove small xattr"
fi
attrs_removed() {
  python3 - "$MNT_POINT/xattr_file" <<'PY'
import sys, os
path = sys.argv[1]
attrs = os.listxattr(path)
sys.exit(1 if 'user.small' in attrs or 'user.big' in attrs else 0)
PY
}
if attrs_removed; then pass "xattrs removed"; else fail "xattrs removed"; fi

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

# Phase 11d: fsck --optimize on a populated filesystem.
#
# This is the regression guard for the class of bug where fsck's *writer* and
# the kernel's *reader* disagree on an on-disk format (e.g. the trie name_len
# field: fsck wrote len(name), the kernel read field-2 and truncated every
# directory entry by 2 bytes). Such a bug is invisible to fsck's own re-verify
# (its reader uses the stored length prefix, not the field) and to the rest of
# this suite, which only ever runs fsck read-only on a populated filesystem. The
# only thing that catches it is letting fsck --optimize rewrite the structures
# and then having the *kernel* resolve them afterwards.
#
# The phase uses its own image + mount point so it cannot perturb the main
# $TEST_IMG that Phase 12 checks read-only. It: populates a directory with
# several varied-length names plus a multi-extent (>126-extent, multi-leaf
# B-tree-backed) file; runs --optimize (which rewrites every directory trie and
# walks every extent index); remounts; and verifies via the kernel that every
# entry is present with the correct name and that the multi-extent file's
# content is byte-identical (md5) before/after.
echo ""
echo "=== Phase 11d: fsck --optimize on populated fs ==="
OPT_IMG="${TMPDIR:-/tmp}/briefs-opt-$$.img"
OPT_MNT="/tmp/briefs-opt-mnt-$$"
"$MKBRIEFS" -s 5000 "$OPT_IMG" 2>/dev/null && pass "optimize: mkfs image" || fail "optimize: mkfs"
mkdir -p "$OPT_MNT"
mount -o loop "$OPT_IMG" "$OPT_MNT" 2>/dev/null && pass "optimize: mount" || fail "optimize: mount"

# A multi-extent file: 200 full 4 KiB writes at every-other 8 KiB offset forces
# the kernel to build a multi-leaf B+ tree extent index (>126 extents), which
# --optimize walks (and would rewrite if it found underfull leaves). Each block
# gets random content so an md5 mismatch after --optimize would flag any extent
# remap corruption, not just a size change.
OPT_BIG="$OPT_MNT/bigfile"
: > "$OPT_BIG"
for i in $(seq 0 199); do
  dd if=/dev/urandom of="$OPT_BIG" bs=4096 count=1 seek=$((i*2)) conv=notrunc 2>/dev/null || true
done
BIG_SIZE=$(stat -c%s "$OPT_BIG" 2>/dev/null || echo 0)
[ "$BIG_SIZE" = "$((199*8192+4096))" ] && pass "optimize: multi-extent file size" || fail "optimize: multi-extent file size" "(got $BIG_SIZE)"
BIG_MD5=$(md5sum "$OPT_BIG" 2>/dev/null | awk '{print $1}' || true)

# Several small files with varied-length names to populate the directory trie
# name heap. The name_len regression truncates each entry by 2 bytes, so mix
# short and long names.
OPT_NAMES="a ab abc abcd medium_name yet_another_file z9"
for nm in $OPT_NAMES; do
  echo -n "content-for-$nm" > "$OPT_MNT/$nm"
done
sync
ENTRY_COUNT=$(ls "$OPT_MNT" 2>/dev/null | wc -l)
umount "$OPT_MNT" 2>/dev/null || true

# Run --optimize (rewrites dir tries + compacts extent indexes). Must exit 0.
if "$FSCKBRIEFS" --optimize -y "$OPT_IMG" >/dev/null 2>&1; then
  pass "optimize: fsck --optimize exits 0"
else
  fail "optimize: fsck --optimize failed" "(exit $?)"
fi

# Remount and verify via the KERNEL that every entry survived with the correct
# name and content. The kernel resolves trie names itself, so a name_len field
# bug surfaces here as missing/garbled entries — exactly where fsck's own
# re-verify would have reported clean.
mount -o loop "$OPT_IMG" "$OPT_MNT" 2>/dev/null && pass "optimize: remount" || fail "optimize: remount"
OPT_ENTRY_COUNT=$(ls "$OPT_MNT" 2>/dev/null | wc -l)
[ "$OPT_ENTRY_COUNT" = "$ENTRY_COUNT" ] && pass "optimize: entry count preserved" || fail "optimize: entry count" "(before $ENTRY_COUNT after $OPT_ENTRY_COUNT)"
ALL_NAMES_OK=1
for nm in $OPT_NAMES; do
  if [ ! -e "$OPT_MNT/$nm" ]; then
    fail "optimize: entry '$nm' missing after optimize"
    ALL_NAMES_OK=0
  else
    check_file "optimize: '$nm' content" "$OPT_MNT/$nm" "content-for-$nm"
  fi
done
[ "$ALL_NAMES_OK" = 1 ] && pass "optimize: all named entries present" || true
BIG_MD5_AFTER=$(md5sum "$OPT_MNT/bigfile" 2>/dev/null | awk '{print $1}' || true)
[ -n "$BIG_MD5_AFTER" ] && [ "$BIG_MD5_AFTER" = "$BIG_MD5" ] && pass "optimize: multi-extent file content intact" || fail "optimize: multi-extent file content" "(before ${BIG_MD5:-none} after ${BIG_MD5_AFTER:-missing})"

# Leave the optimize image clean: read-only fsck must still pass after --optimize.
umount "$OPT_MNT" 2>/dev/null || true
"$FSCKBRIEFS" "$OPT_IMG" 2>/dev/null && pass "optimize: fsck clean after --optimize" || fail "optimize: fsck found errors after --optimize"
rm -f "$OPT_IMG"
rmdir "$OPT_MNT" 2>/dev/null || true

# Phase 11e: debugfs/sysfs/proc observability surfaces (-o debug).
#
# Regression guard for the per-superblock observability surfaces: the always-on
# sysfs attributes (/sys/fs/briefs/<s_id>/), the /proc/fs/briefs/mounts index,
# and the per-sb debugfs tree (/sys/kernel/debug/briefs/<s_id>/, -o debug only)
# with its gated stat counters. A regression in mount-option parsing, sysfs/
# proc/debugfs registration or teardown, or the -o debug stat-counter
# instrumentation surfaces here. Uses its own image + mount point so it cannot
# perturb the main $TEST_IMG that Phase 12 checks read-only.
echo ""
echo "=== Phase 11e: observability surfaces (-o debug) ==="
DBG_IMG="${TMPDIR:-/tmp}/briefs-obs-$$.img"
DBG_MNT="/tmp/briefs-obs-mnt-$$"
# debugfs must be mounted for /sys/kernel/debug/briefs to be reachable.
mount -t debugfs none /sys/kernel/debug 2>/dev/null || true
"$MKBRIEFS" -s 5000 "$DBG_IMG" 2>/dev/null && pass "obs: mkfs image" || fail "obs: mkfs"
mkdir -p "$DBG_MNT"
# -o debug enables the per-sb debugfs tree + stat counters; sysfs/proc are on
# for every mount. -t briefs ensures "debug" is parsed by the filesystem, not
# swallowed by mount(8).
mount -o loop,debug -t briefs "$DBG_IMG" "$DBG_MNT" 2>/dev/null && pass "obs: mount -o debug" || fail "obs: mount -o debug"
# The -o debug mount is the only briefs mount with a debugfs dir (the main
# suite mount has no -o debug), so its s_id is the lone dir name there.
DBG_SID=$(ls /sys/kernel/debug/briefs/ 2>/dev/null | head -1 || true)
[ -n "$DBG_SID" ] && pass "obs: per-sb debugfs dir present" || fail "obs: per-sb debugfs dir missing"

# sysfs: the per-sb attribute dir exists with the expected files.
if [ -n "$DBG_SID" ] && [ -f "/sys/fs/briefs/$DBG_SID/version" ]; then
  pass "obs: sysfs attr dir present"
else
  fail "obs: sysfs attr dir missing"
fi
# A representative attr reads a sane value (mkfs writes a 0.9.x on-disk version).
DBG_VER=$(cat "/sys/fs/briefs/$DBG_SID/version" 2>/dev/null || true)
case "$DBG_VER" in 0.9.*) pass "obs: sysfs version sane ($DBG_VER)";; *) fail "obs: sysfs version" "(got '$DBG_VER')";; esac

# /proc/fs/briefs/mounts lists this mount (one line per mounted instance).
if grep -q "^$DBG_SID" /proc/fs/briefs/mounts 2>/dev/null; then
  pass "obs: /proc/fs/briefs/mounts lists mount"
else
  fail "obs: /proc/fs/briefs/mounts missing entry"
fi

# The seven debugfs files exist.
DBG_FILES="mount_info superblock data_alloc inode_alloc journal trie_pool stats"
DBG_ALL_OK=1
for f in $DBG_FILES; do
  if [ ! -f "/sys/kernel/debug/briefs/$DBG_SID/$f" ]; then
    fail "obs: debugfs file '$f' missing"; DBG_ALL_OK=0
  fi
done
[ "$DBG_ALL_OK" = 1 ] && pass "obs: all seven debugfs files present" || true

# Stat counters: snapshot before, exercise the instrumented paths, assert they
# advanced. Creates files (data alloc + inode alloc + dir add + journal records)
# and punches a hole.
DBG_DATA_BEFORE=$(awk -F= '/^data_alloc_calls=/{print $2}' "/sys/kernel/debug/briefs/$DBG_SID/stats" 2>/dev/null || echo 0)
for i in 1 2 3 4 5; do dd if=/dev/zero of="$DBG_MNT/f$i" bs=4096 count=4 2>/dev/null || true; done
fallocate -p -l 8192 "$DBG_MNT/f1" 2>/dev/null || true
sync
DBG_DATA_AFTER=$(awk -F= '/^data_alloc_calls=/{print $2}' "/sys/kernel/debug/briefs/$DBG_SID/stats" 2>/dev/null || echo 0)
[ "$DBG_DATA_AFTER" -gt "$DBG_DATA_BEFORE" ] && pass "obs: stats data_alloc_calls advanced ($DBG_DATA_BEFORE -> $DBG_DATA_AFTER)" || fail "obs: stats data_alloc_calls did not advance" "(before $DBG_DATA_BEFORE after $DBG_DATA_AFTER)"
DBG_PUNCH=$(awk -F= '/^punch_holes=/{print $2}' "/sys/kernel/debug/briefs/$DBG_SID/stats" 2>/dev/null || echo 0)
[ "$DBG_PUNCH" -ge 1 ] && pass "obs: stats punch_holes counted ($DBG_PUNCH)" || fail "obs: stats punch_holes not counted" "(got $DBG_PUNCH)"

# sysfs free_blocks is the authoritative live allocator count; after writes it
# must be positive and below the total data-block count (5000-block image).
DBG_FREE=$(cat "/sys/fs/briefs/$DBG_SID/free_blocks" 2>/dev/null || echo 0)
[ "$DBG_FREE" -gt 0 ] && [ "$DBG_FREE" -lt 5000 ] && pass "obs: sysfs free_blocks sane ($DBG_FREE)" || fail "obs: sysfs free_blocks" "(got $DBG_FREE)"

# Teardown: unmount and confirm every surface entry for this sb is gone.
umount "$DBG_MNT" 2>/dev/null || true
if [ -n "$DBG_SID" ] && [ -e "/sys/fs/briefs/$DBG_SID" ]; then
  fail "obs: sysfs dir survived unmount"
else
  pass "obs: sysfs dir removed on unmount"
fi
if [ -n "$DBG_SID" ] && [ -e "/sys/kernel/debug/briefs/$DBG_SID" ]; then
  fail "obs: debugfs dir survived unmount"
else
  pass "obs: debugfs dir removed on unmount"
fi
if grep -q "^$DBG_SID" /proc/fs/briefs/mounts 2>/dev/null; then
  fail "obs: /proc entry survived unmount"
else
  pass "obs: /proc entry removed on unmount"
fi
rm -f "$DBG_IMG"
rmdir "$DBG_MNT" 2>/dev/null || true

# Phase 11f: NFS export_operations (generation-based file handles). Exercises
# encode_fh / fh_to_dentry directly via name_to_handle_at + open_by_handle_at
# (no nfsd required) and confirms stale handles are rejected. Runs against the
# live $MNT_POINT (still mounted from earlier phases); it leaves hsub/inner
# behind, which Phase 12 fscks without issue.
echo ""
echo "=== Phase 11f: export_operations (file handles) ==="
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HANDLE_SRC="$SCRIPT_DIR/handle_test.c"
HANDLE_BIN="${TMPDIR:-/tmp}/briefs-handle-test-$$"
HANDLE_OUT="${TMPDIR:-/tmp}/briefs-handle-out-$$"
if ! command -v gcc >/dev/null 2>&1; then
  echo "  SKIP: gcc unavailable — handle test skipped"
elif [ ! -f "$HANDLE_SRC" ]; then
  fail "handle test source missing" "($HANDLE_SRC)"
else
  if gcc -O2 -o "$HANDLE_BIN" "$HANDLE_SRC" 2>/dev/null; then
    pass "compile handle test"
  else
    fail "compile handle test"
  fi
  if [ -x "$HANDLE_BIN" ]; then
    if "$HANDLE_BIN" "$MNT_POINT" >"$HANDLE_OUT" 2>&1; then
      pass "handle test: encode/fh_to_dentry/stale reject"
    else
      fail "handle test: encode/fh_to_dentry/stale reject" "(see $HANDLE_OUT)"
      sed 's/^/    /' "$HANDLE_OUT" 2>/dev/null || true
    fi
  fi
  rm -f "$HANDLE_BIN" "$HANDLE_OUT"
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