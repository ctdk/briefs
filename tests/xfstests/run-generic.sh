#!/bin/bash
# Run an xfstests group against BrieFS in the VM. Run as:
#   sudo bash /vagrant/tests/xfstests/run-generic.sh [group] [timeout_secs] [exclude_group]
# Default: the "auto" group (every test tagged 'auto' across all test dirs —
# the standard xfstests broad run; excludes dangerous/soak/broken tags),
# 2 hour hard cap, with the "mmap" group excluded (BrieFS mmap+writeback
# deadlocks — see briefs-mmap-writeback-deadlock memory; a D-state hang wedges
# the mount and only a VM reboot recovers). Log goes to the NFS-shared
# /xfstests tree so it is readable from the host at
# /home/jeremy/src/xfstests-dev/run-generic.log.
#
# NOTE: `generic` is a test DIRECTORY, not a group tag — `./check -g generic`
# fails with "Group generic is empty or not defined". Use `-g auto` for the
# broad automated slice, or pass explicit paths like `generic/005`.
set -uo pipefail
GROUP="${1:-auto}"
CAP="${2:-7200}"
# ${3-mmap} (not ${3:-mmap}) so an explicit empty $3 disables the exclude:
# `run-generic.sh mmap 1800 ""` runs `-g mmap` with NO -x, whereas omitting $3
# keeps the default mmap exclusion.  (: treats empty as unset and would turn
# "" back into "mmap", making `-g mmap -x mmap` -> 0 tests.)
EXCLUDE="${3-mmap}"

# `vagrant ssh -c` (non-login) hands us a minimal PATH without /usr/sbin, so
# `mkfs`/`mount`/`fsck` (all in /usr/sbin) aren't found. sudo's secure_path
# would fix this, but be explicit so it also works when invoked directly.
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin

cd /xfstests || { echo "no /xfstests" >&2; exit 2; }

# Pre-clean stale mounts/loops so a previous crashed run can't poison this one.
umount /mnt/briefs-scratch 2>/dev/null || true
umount /mnt/briefs-test    2>/dev/null || true

: > /xfstests/run-generic.log
XARG=()
[ -n "$EXCLUDE" ] && XARG=(-x "$EXCLUDE")
{
  echo "=== xfstests briefs -g $GROUP ${XARG[*]} (cap ${CAP}s) $(date) ==="
  HOST_OPTIONS=configs/briefs.config timeout "$CAP" ./check -s briefs -g "$GROUP" "${XARG[@]}"
  echo "CHECK_EXIT=$?"
  echo "DONE"
} > /xfstests/run-generic.log 2>&1