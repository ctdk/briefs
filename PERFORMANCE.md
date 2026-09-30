# BrieFS performance comparison

How BrieFS measures up against ext2, ext4, XFS, btrfs, and JFS, as
measured by a full fio comparison round on the development VM.  The
point is honest external reference points, not a contest: BrieFS is a
hobby/research filesystem and trails mature filesystems in places;
every number below is VM-relative (virtio disk, 16 GiB guest RAM) and
says nothing about bare-metal performance.

## Round record

**run-20260929-034931** (2026-09-29, ~15.5h total): all six
filesystems, full workload matrix, zero failed or timed-out fio
invocations, no BrieFS dmesg splats (module loaded and mounted cleanly
for every rep).  This round re-measured everything after the
**io_uring-perf campaign** (see below); the previous round
**run-20260925-201047** (2026-09-25/26) used the pre-campaign module
and its numbers are quoted where the campaign moved a result.

* Kernel: stock Debian **6.12.107+deb13-amd64** — one kernel for
  every filesystem, no lockdep.  (The self-built 7.3-rc4 test kernels
  have no XFS, so they cannot host this comparison.)
* Module: io_uring-perf branch at `3967361` (master +
  BH_Verified read-path memoization, slicing-by-8 CRC32C over the
  frozen nonstandard polynomial, and FMODE_NOWAIT/IOCB_NOWAIT support),
  built against the stock headers.
* The rest of the setup is unchanged from the first round:
* VM: 16 GiB RAM / 8 vCPUs; dedicated 64 GiB **raw, cache=none**
  virtio disk (mkfs'd per filesystem in turn; cache=none removes the
  host page cache from the data path).  Working sets 32 GiB = 2x RAM,
  so random I/O cannot be served from cache.
* Mount options: defaults + `noatime`, no tuning games.  `sync` +
  `drop_caches` before every measurement; `--direct=1` for the
  IO-bound jobs.
* fio 3.39; 3 reps per data point (5 for short micro tests, 2 for the
  parallel phase), median reported.  fio 3.39 has no `--seed`; its
  default `randrepeat=1` makes every invocation with the same job
  spec draw the **identical random sequence**, so all filesystems see
  the same offsets.  Random jobs are runtime-bound (`time_based`)
  rather than size-bound: at 32 GiB spans a size-bound 4k job would be
  64M Is with unbounded wall time across very different rates.
* Runner: `tests/xfstests/runs/perf-vs-other-fs.sh`,
  summarizer: `tests/xfstests/runs/summarize-perf.py`.  BrieFS ran
  last so a wedge could not poison the other filesystems' numbers.

## Results (medians)

### Streaming I/O — at parity

| MB/s          | briefs | ext2 | ext4 | xfs  | btrfs | jfs  |
|---------------|-------:|-----:|-----:|-----:|------:|-----:|
| W1a write, 1M, direct     |  557 |  431 |  538 |  532 |   318 |  670 |
| W1b write, 1M, buffered   |  913 |  646 |  686 |  839 |   647 |  915 |
| W2a read, 1M, direct      | 1240 | 1100 | 1270 | 1220 |  1120 | 1190 |
| W2b read, 1M, buffered    | 1830 | 1060 | 1880 | 2190 |   697 | 2050 |

(The W1a direct-write job is the noisiest of the round — every
filesystem's spread there runs to hundreds of MB/s.)

### Random I/O

| IOPS                | briefs | ext2 |  ext4 |  xfs | btrfs |   jfs |
|---------------------|-------:|-----:|------:|-----:|------:|------:|
| W3a rand read 4k, qd1  | 10,397 | 10,630 | 10,289 | 7,650 |  8,239 |  7,758 |
| W3b rand read 4k, qd32 | 10,525 | 10,302 | 10,618 | 7,702 |  8,493 |  9,132 |
| W9a mmap rand read 4k  |  9,347 |  9,796 |  8,948 | 6,979 |  7,990 |  7,452 |
| W4 rand write 4k, qd32 | 11,631 | 10,787 | 10,727 | 11,113 |  7,242 | 12,010 |
| W9b io_uring DIO read 4k, qd64 | 172,975 | 166,275 | 175,400 | 175,862 | 89,822 | 163,771 |

(Pre-campaign, run-20260925-201047: W3a 6,252 / W3b 6,237 / W9a 6,135 /
W9b 42,321 — the io_uring-perf round closed all four.)

### Sync write

|                     | briefs | ext2 | ext4 | xfs  | btrfs | jfs   |
|---------------------|-------:|-----:|-----:|-----:|------:|------:|
| W5a 4k write + fdatasync (IOPS) |   81 |   94 |  100 |   98 |    48 | 5,079 |
| W5b 1M write + fdatasync (MB/s) | 67.2 | 76.2 | 73.3 | 66.3 |  43.1 | 246.7 |
| W8 4k append, fdatasync/100 (IOPS) | 7,661 | 8,061 | 4,505 | 4,681 | 4,529 | 91,587 |

### Small files and parallelism

|                              | briefs |  ext2 |   ext4 |  xfs | btrfs |   jfs |
|------------------------------|-------:|------:|-------:|-----:|------:|------:|
| W6 create 50k x 4k files (s) |  639  |   543 |  1,045 | 1045 |  1045 |    11 |
| W7 untar kernel tree (s)     |  8.9  |   3.9 |    2.3 |  3.0 |   3.7 |   4.7 |
| W7 traverse (s)              |  2.3  |   1.7 |   0.09 |  2.0 |  0.07 |   2.8 |
| W7 rm -rf (s)                |  5.9  |   2.0 |    1.8 |  2.4 |   1.2 |   3.4 |
| W10 create 20k files, 4 jobs (files/s) |   97 |   124 |    112 |  115 |   120 | 5,490 |
| W10 create 20k files, 8 jobs (files/s) |  117 |   131 |    210 |  135 |   205 | 6,682 |
| W10 rand write 4k, 4 jobs (MB/s) | 15.5 |  14.9 |   22.6 |  6.1 |  18.5 |  30.6 |
| W10 rand write 4k, 8 jobs (MB/s) |  8.0 |  13.4 |    7.6 |  2.8 |   8.7 |  18.8 |

## What the numbers say

* **Streaming I/O: BrieFS is at parity.**  Sequential writes and reads
  (direct and buffered) land inside the pack everywhere.  The
  2026-09 write-perf campaign's fix series (per-op journal
  checkpointing aside, the O(E²) chain-scan and per-block chain-sync
  items) appears to have closed what used to be the biggest gap.
* **Random reads are at parity since the io_uring-perf campaign
  (2026-09-28/29).**  W3a 10.4k IOPS at qd1 vs ext4's 10.3k, same
  shape at qd32 and under mmap — up from 6.2k (65% of ext4) in the
  pre-campaign round.  The gap had been a fixed per-read cost: every
  btree node in the descent path was magic+CRC re-verified on every
  read (the BH_Verified memoization only ever fired on locked paths),
  and the CRC was a byte-at-a-time table loop.  Memoizing
  verification on the unlocked read path plus a slicing-by-8 CRC
  over the (frozen, nonstandard) polynomial removed it.
* **Random writes lead the non-JFS pack:** 11.6k IOPS vs ext4 10.7k /
  XFS 11.1k, well ahead of btrfs's 7.2k.
* **fdatasync-per-op — the known weak spot — is mid-pack, not last.**
  81 IOPS at 4k vs ext2 94 / ext4 100 / XFS 98 / btrfs 48.  Per-op
  journal checkpointing costs less here than btrfs's CoW.  At 1M the
  gap nearly closes (67.2 vs 73.3 MB/s).
* **Append-log writes beat the journaled pack:** 7.7k IOPS vs ext4
  4.5k / XFS 4.7k / btrfs 4.5k — the fdatasync-every-100 shape suits
  the journal (and the campaign's fast CRC cut the per-record
  checksum that sits directly on that path).
* **io_uring DIO at qd64 — the previous round's biggest gap — is
  closed.**  42k → 173k IOPS, at parity with ext4 (175k) and XFS
  (176k).  Two causes: the same per-read CRC re-verify running
  inline in the single submitter thread, and the lack of
  FMODE_NOWAIT/IOCB_NOWAIT support, which made every io_uring DIO
  read punt to an io-wq worker.  Both fixed on the io_uring-perf
  branch (BH_Verified memoization + slicing-by-8 CRC + NOWAIT
  support with trylocks).
* **The metadata create/lookup path (W7) is BrieFS's remaining weak
  spot.**  Untar runs at ~9.7k files/s vs ext4's 37k, and directory
  traversal is ~25x slower than ext4/btrfs (2.3s vs 0.09s).
* **Parallel scaling confirms the journal serialization diagnosis.**
  At 4 create jobs all filesystems but JFS sit at ~97-124 files/s;
  at 8 jobs ext4 reaches 210 and btrfs 205 while BrieFS stays flat at
  117 — the per-journal `write_lock` is exactly the serialization
  point the write-perf campaign identified.

## Caveats

* **W6 measures sync-create, not plain create.**  fio's `create_only`
  layout path fsyncs every file, so W6 is dominated by per-file sync
  cost (this is why ext4/XFS/btrfs cluster at ~47 f/s and ext2, with
  no journal, is at 94).  The honest async create rate is W7's untar.
* **JFS's sync numbers are a journal-semantics artifact.**  JFS's
  fdatasync on a tiny fresh file is nearly free (lazy metadata-only
  journal), which puts its W5a/W8/W6 numbers far above the pack; they
  are not comparable to the others.  JFS was included as the closest
  architectural peer (extents + B+ tree + metadata-only journal) and
  its numbers sit outside the fairness invariant: curiosity tier.
* Numbers from this VM are **not comparable** to runs on a differently
  sized VM (RAM shifts writeback batching and mkfs auto-tuning); the
  round was sized once, up front, and every filesystem ran on the same
  config.

## Reproducing

Run `tests/xfstests/runs/perf-vs-other-fs.sh` as root in the VM,
detached; it mkfs's one dedicated disk per filesystem in turn and
prints a `PERF DONE <runid>` sentinel when done (see the script header
for the environment it expects and the knobs for trimming the round).
`tests/xfstests/runs/summarize-perf.py <runid-dir>` prints the
per-workload median+spread tables from a round's raw JSONs.  The raw
fio JSONs, phase timings, and dmesg snapshots from both rounds
(run-20260925-201047 and run-20260929-034931) are archived by the
author outside the repo.