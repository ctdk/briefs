#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
#
# Regression repro for the cumulative/intermittent journal-replay corruption
# fixed in briefs_journal.c walk_journal() (skip the reserved checkpoint block).
#
# Bug: the journal ring reserves its last block (journal_end-1) as the
# checkpoint block.  The write path (briefs_journal_write_record /
# briefs_journal_sync) skips it when advancing write_pos, so ordinary records
# never land there -- only a single JRN_CHECKPOINT marker record does, which
# replay already ignores.  walk_journal() used next_block() WITHOUT that skip,
# so a wrapped [log_start, log_end) crossing the checkpoint block read it as an
# ordinary record block and failed when its content was stale (a prior
# lifetime's checkpoint header with a garbage record type) ->
#   invalid record type=22 at block=1048575
#   journal replay failed: -5
#   mount: can't read superblock on /dev/loop0
#
# This script crafts that exact image on a freshly mkfs'd briefs device:
#   - block (cp-1): a valid empty journal block (JOURNAL_MAGIC, record_count=0)
#   - block cp:     the stale checkpoint signature
#                   (CHECKPOINT_MAGIC + block_seq=0 + record_count=1 +
#                    record type=22 at offset 16)
#   - superblock:   journal_log_start=cp-1, journal_log_end=journal_start
#                   (a wrapped range that crosses the checkpoint block)
#
# With the fix, `mount -t briefs` succeeds (replay skips the cp block:
# "journal replay complete (blocks=1, records=0, errors=0)").
# Without the fix, mount fails with the exact signature above.
#
# Usage (on the VM, against a free /dev/loopN):
#   sudo mkfs.briefs /dev/loop0
#   sudo python3 repro-wrapped-checkpoint-skip.py /dev/loop0
#   sudo dmesg -C && sudo mount -t briefs /dev/loop0 /mnt/briefs-test; echo $?
#   sudo dmesg | grep -E 'replaying journal|replay complete|invalid record type|replay failed'
#   sudo umount /mnt/briefs-test
import os, struct, sys

DEV = sys.argv[1] if len(sys.argv) > 1 else "/dev/loop0"
BS = 4096
JMAGIC = 0x4A4E4C5A   # "JNLZ"
CMAGIC = 0x43485053   # "CHPS"
SB_MAGIC = 0x504C434E  # "PLCN"

# superblock field offsets (struct briefs_superblock, all __le64)
OFF_JOFF, OFF_JBLK, OFF_LS, OFF_LE = 240, 248, 264, 272

fd = os.open(DEV, os.O_RDWR)
os.lseek(fd, 0, 0)
sb = bytearray(os.read(fd, BS))
if struct.unpack_from("<Q", sb, 0)[0] != SB_MAGIC:
    sys.exit("not a briefs superblock")
joff = struct.unpack_from("<Q", sb, OFF_JOFF)[0]
jblk = struct.unpack_from("<Q", sb, OFF_JBLK)[0]
cp = joff + jblk - 1
print(f"joff={joff} jblk={jblk} cp={cp}")

# block (cp-1): valid empty journal block
b1 = bytearray(BS)
struct.pack_into("<IIII", b1, 0, JMAGIC, 999, 0, 0)
os.lseek(fd, (cp - 1) * BS, 0); os.write(fd, b1)

# block cp: stale checkpoint signature (header valid, record type=22 garbage)
b2 = bytearray(BS)
struct.pack_into("<IIII", b2, 0, CMAGIC, 0, 1, 0)        # magic,block_seq,record_count,reserved
struct.pack_into("<IIII", b2, 16, 22, 0, 0, 0)          # record: type=22,flags,data_len,checksum
os.lseek(fd, cp * BS, 0); os.write(fd, b2)

# superblock: wrapped range [cp-1, journal_start) crossing the cp block
struct.pack_into("<Q", sb, OFF_LS, cp - 1)
struct.pack_into("<Q", sb, OFF_LE, joff)
os.lseek(fd, 0, 0); os.write(fd, bytes(sb))
os.fsync(fd)
os.close(fd)
print(f"crafted: ls={cp-1} le={joff} (wrapped, crosses cp={cp})")