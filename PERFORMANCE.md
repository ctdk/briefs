# BrieFS performance comparison

How BrieFS measures up against ext2, ext4, XFS, btrfs, and JFS, as
measured by a full fio comparison round on the development VM.  The
point is honest external reference points, not a contest: BrieFS is a
hobby/research filesystem and trails mature filesystems in places;
every number below is VM-relative (virtio disk, 16 GiB guest RAM) and
says nothing about bare-metal performance.

## Round record

**run-20260925-201047** (2026-09-25/26, ~15h20m total): all six
filesystems, full workload matrix, **zero failed or timed-out fio
invocations, no BrieFS dmesg splats** (module loaded and mounted
cleanly for every rep; journal clean on every mount).

* Kernel: stock Debian **6.12.107+deb13-amd64** — one kernel for
  every filesystem, no lockdep.  (The self-built 7.3-rc4 test kernels
  have no XFS, so they cannot host this comparison.)
* Module: briefs-compat branch at `a25e5c1` (v0.9.7), built against
  the stock headers.
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
| W1a write, 1M, direct     |  625 |  614 |  706 |  639 |   445 |  638 |
| W1b write, 1M, buffered   |  908 |  791 |  898 |  905 |   719 |  939 |
| W2a read, 1M, direct      | 1140 | 1190 | 1310 | 1290 |  1030 | 1180 |
| W2b read, 1M, buffered    | 2110 | 1890 | 2120 | 2300 |  2070 | 2110 |

### Random I/O

| IOPS                | briefs | ext2 |  ext4 |  xfs | btrfs |   jfs |
|---------------------|-------:|-----:|------:|-----:|------:|------:|
| W3a rand read 4k, qd1  |  6,252 |  9,208 |  9,574 | 7,903 |  8,957 |  9,300 |
| W3b rand read 4k, qd32 |  6,237 |  9,696 |  9,854 | 7,945 |  8,959 |  8,456 |
| W9a mmap rand read 4k  |  6,135 |  8,904 |  9,306 | 7,536 |  8,701 |  9,065 |
| W4 rand write 4k, qd32 |  9,019 |  9,580 | 10,941 | 12,127* | 8,293 | 13,894 |
| W9b io_uring DIO read 4k, qd64 | 42,321 | 142,794 | 165,744 | 174,819 | 96,212 | 181,408 |

(*The XFS W4 number was noisy; spreads for the buffered-read and some
write jobs run high across all filesystems.)

### Sync write

|                     | briefs | ext2 | ext4 | xfs  | btrfs | jfs   |
|---------------------|-------:|-----:|-----:|-----:|------:|------:|
| W5a 4k write + fdatasync (IOPS) |   77 |  106 |   99 |   99 |    48 | 5,808 |
| W5b 1M write + fdatasync (MB/s) | 66.6 | 77.1 | 78.6 | 70.7 |  42.6 | 416.1 |
| W8 4k append, fdatasync/100 (IOPS) | 5,255 | 8,715 | 4,076 | 4,629 | 4,502 | 93,910 |

### Small files and parallelism

|                              | briefs |  ext2 |   ext4 |  xfs | btrfs |   jfs |
|------------------------------|-------:|------:|-------:|-----:|------:|------:|
| W6 create 50k x 4k files (s) |  670  |   533 |  1,096 | 1041 |  1061 |    10 |
| W7 untar kernel tree (s)     |  6.3  |   3.0 |    2.0 |  4.0 |   3.8 |   3.5 |
| W7 traverse (s)              |  2.6  |   4.0 |   0.09 |  0.8 |  0.07 |   2.5 |
| W7 rm -rf (s)                |  5.5  |   2.0 |    1.5 |  3.1 |   1.2 |   3.5 |
| W10 create 20k files, 4 jobs (files/s) |   98 |   118 |    118 |  117 |   117 | 6,633 |
| W10 create 20k files, 8 jobs (files/s) |  118 |   136 |    268 |  141 |   199 | 7,456 |
| W10 rand write 4k, 4 jobs (MB/s) | 18.0 |  22.5 |   25.3 |  6.0 |  27.3 |  35.0 |
| W10 rand write 4k, 8 jobs (MB/s) |  9.7 |  11.3 |    8.3 |  2.9 |   9.3 |  23.5 |

## What the numbers say

* **Streaming I/O: BrieFS is at parity.**  Sequential writes and reads
  (direct and buffered) land inside the pack everywhere.  The
  2026-09 write-perf campaign's fix series (per-op journal
  checkpointing aside, the O(E²) chain-scan and per-block chain-sync
  items) appears to have closed what used to be the biggest gap.
* **Random reads are per-IO-bound, not concurrency-bound.**  6.2k
  IOPS at queue depth 1 *and* 32 — no scaling with depth, ~65% of
  ext4, same shape under mmap.  That is the signature of a fixed
  per-read cost in the BrieFS path, not of a lock or queue limit.
* **Random writes are respectable:** 9.0k IOPS vs ext4's 10.9k, ahead
  of btrfs.
* **fdatasync-per-op — the known weak spot — is mid-pack, not last.**
  77 IOPS at 4k vs ext2 106 / ext4 99 / XFS 99 / btrfs 48.  Per-op
  journal checkpointing costs less here than btrfs's CoW.  At 1M the
  gap nearly closes (66.6 vs 78.6 MB/s).
* **Append-log writes beat the journaled pack:** 5.3k IOPS vs ext4
  4.1k / XFS 4.6k / btrfs 4.5k — the fdatasync-every-100 shape suits
  the journal.
* **io_uring DIO at qd64 is the biggest gap in the round:** 42k IOPS
  vs ext4's 166k, and bimodal across reps (some runs much faster).
  Whatever limits the high-depth async read path is the top
  performance lead to chase next.
* **The metadata create/lookup path (W7) is BrieFS's real weak spot.**
  Untar runs at ~13.7k files/s vs ext4's 43k — about half the slowest
  peer — and directory traversal is ~30x slower than ext4/btrfs.
* **Parallel scaling confirms the journal serialization diagnosis.**
  At 4 create jobs all filesystems but JFS sit at ~117-118 files/s;
  at 8 jobs ext4 reaches 268 and btrfs 199 while BrieFS stays flat at
  118 — the per-journal `write_lock` is exactly the serialization
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
fio JSONs, phase timings, and dmesg snapshots from
run-20260925-201047 are archived by the author outside the repo.