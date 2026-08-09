# xfstests run archives

Each file here is a snapshot of one `run-suite.sh` invocation: the
pass/fail/not-run/skipped totals plus the per-test lists grouped by category.
`run-suite.sh` writes one automatically at the end of every run (one file per
run, named `run-<timestamp>-<kernel|fuse>.txt`).

Commit the full-run archives (high `tests_processed`) to track progress and
regressions across time. Short ad-hoc runs are archived too by default
(`ARCHIVE_MIN=1`) so no result is lost; raise `ARCHIVE_MIN` or delete the ones
you don't want to keep.

## File format

A metadata header, a totals block, grouped per-category lists, then a
diff-friendly per-test status block:

```
run_id:           20260809-153000
date:             2026-08-09T15:30:00Z
kernel:           6.12.100+deb13-amd64
branch:           refactor-round-1
commit:           c794549
mount:            kernel          # or fuse
mount_cmd:        mount -t briefs
test_dev:         /dev/vdb
scratch_dev:      /dev/vdc1
fsck_enabled:     0
fsck_warn:        0   # annotation, not a category (overlaps the above)
tests_processed: 780
skip_list:        generic/051 generic/068 ...

# Totals (mutually-exclusive categories; sum to tests_processed)
pass: 327
fail: 8
not_run: 413
skipped: 13
hang: 0
mount_fail: 0
mkfs_fail: 0
unknown: 0
resumed: 0

# PASS (327)
001 002 003 005 006 007 011 013 014 015 020 023 024 025 027 028
...

# FAIL (8)
050 089 311 547 563 599 623 730

# NOT RUN (413)
...

# SKIPPED (13)
051 068 070 074 224 410 411 461 464 475 476 619 753

...

# Per-test status (diff-friendly: <testnum> <category>, sorted by number)
001 PASS
002 PASS
004 NOTRUN
050 FAIL
...
```

Each test appears in exactly one category, so the totals always sum to
`tests_processed`.

## Categories

| token   | meaning                                              |
|---------|------------------------------------------------------|
| PASS    | matched golden output                                |
| FAIL    | output mismatch, or interrupted (`Passed all 0`)    |
| NOTRUN  | xfstests `_require_*` gate skipped the test          |
| SKIPPED | in the runner's `SKIP_TESTS` skip list               |
| HANG    | timed out, or produced no output                     |
| MOUNT   | TEST_DEV failed to mount                             |
| MKFS    | `mkfs.briefs` failed                                 |
| UNKNOWN | `./check` aborted with an ambiguous status           |
| RESUMED | skipped via the resume feature (already had a log)  |

`FSCK WARN` is an **annotation**, not a category: tests whose post-test
`fsck.briefs` flagged an inconsistency. Such a test keeps its run category
(often PASS) and is additionally listed under `# FSCK WARN`.

## Diffing runs

Use `../diff-runs.sh` to compare two archives — it prints each test whose
category changed and a transition tally:

```bash
tests/xfstests/diff-runs.sh \
    tests/xfstests/runs/run-20260805-kernel.txt \
    tests/xfstests/runs/run-20260809-kernel.txt
```

```
=== status changes: .../run-20260805-kernel.txt -> .../run-20260809-kernel.txt ===
  224: PASS -> HANG
  464: PASS -> HANG
  475: SKIPPED -> PASS
  ---
  PASS -> HANG: 2
  SKIPPED -> PASS: 1
```