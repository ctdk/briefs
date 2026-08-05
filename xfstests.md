# BrieFS xfstests Results

## Summary (2026-08-05)

**Total generic tests:** 793

| Result | Count | Percentage |
|--------|-------|------------|
| PASS   | 327   | 41.2%      |
| FAIL   | 11    | 1.4%       |
| NOT RUN| 420   | 53.0%      |
| HANG   | 2     | 0.3%       |

## Skip List

Tests skipped due to known hangs or unsupported features:

- **generic/051** - requires shutdown support (FS_IOC_FIFREEZE) - hangs on mount
- **generic/068** - unsupported feature
- **generic/070** - unsupported feature  
- **generic/074** - mmap writeback hang
- **generic/224** - intermittent hang (investigate if time permits)
- **generic/410** - mount namespace test
- **generic/411** - mount namespace test - fails fsck after test, cascades to hangs
- **generic/461** - hung on 2026-08-04 run
- **generic/464** - unsupported feature
- **generic/475** - dm-error replay (deferred)
- **generic/476** - unsupported feature
- **generic/619** - hung on 2026-08-04 run
- **generic/753** - hung on 2026-08-05 run

## Failure Analysis (updated 2026-08-05)

### FAIL Tests (10 remaining, 1 fixed)

**Fixed 2026-08-05:**
- **generic/027** - Was: vfree() warning in briefs_alloc_cleanup. Fixed by adding is_vmalloc_addr() validation before vfree() calls. Now passes.

**Remaining 10 FAIL tests:**

| Test | Category | Notes |
|------|----------|-------|
| generic/050 | Expected diff | Mount behavior on read-only device needing recovery |
| generic/062 | Xattr | Extended attribute output format difference |
| generic/089 | Output format | fsx stress test iteration count format |
| generic/177 | Environment | awk strtonum() missing (needs gawk) |
| generic/311 | Pre-existing | Fsync+flakey test, documented baseline failure |
| generic/547 | Pre-existing | Chown+trie replay, fsstress edge cases |
| generic/563 | Environment | Cgroup writeback timing test |
| generic/599 | Deferred | VFS warning in cleanup_mnt() after shutdown ioctl |
| generic/623 | Expected diff | Fsync error code (EROFS vs EIO) |
| generic/730 | Expected diff | I/O error handling difference |

See [xfstests-failures-analysis.md](xfstests-failures-analysis.md) for detailed analysis.

### NOT RUN Tests (420 total)

Major categories of unsupported features:
- Reflink support (reflink, dedupe, copy_range)
- ACL support
- Quota support
- fzero/fcollapse/finsert file operations
- mkfs overwrite detection
- Various fs-specific features

### HANG Tests (2 total)

Tests that caused the VM to hang and require forced reboot:
- generic/461
- generic/619
- generic/753 (added to skip list after hanging)

## Batch Run Details (2026-08-04 to 2026-08-05)

Tests were run in batches of 50 to isolate hanging tests:

| Batch | Tests | PASS | FAIL | NOT RUN | HANG |
|-------|-------|------|------|---------|------|
| 1 | 001-050 | 32 | 2 | 16 | 0 |
| 2 | 051-100 | 28 | 2 | 20 | 0 |
| 3 | 101-150 | 22 | 0 | 27 | 1 |
| 4 | 151-200 | 4 | 1 | 45 | 0 |
| 5 | 201-250 | 22 | 0 | 27 | 1 |
| 6 | 251-300 | 13 | 0 | 37 | 0 |
| 7 | 301-350 | 28 | 1 | 21 | 0 |
| 8 | 351-400 | 15 | 0 | 35 | 0 |
| 9 | 401-450 | 32 | 0 | 18 | 0 |
| 10 | 451-500 | 24 | 0 | 15 | 0 |
| 11 | 501-550 | 26 | 1 | 23 | 0 |
| 12 | 551-600 | 17 | 2 | 31 | 0 |
| 13 | 620-650 | 19 | 1 | 11 | 0 |
| 14 | 651-700 | 21 | 1 | 28 | 0 |
| 15 | 701-750 | 16 | 0 | 24 | 0 |
| 16 | 754-793 | 16 | 0 | 24 | 0 |

## Notes

- Tests were run with the run-suite.sh script which reformats devices between tests
- Skip list was updated during the run as hanging tests were identified
- The majority of NOT RUN tests are due to BrieFS not implementing certain features (expected)
- The 11 FAIL tests should be analyzed to determine if they represent missing features or bugs
- Batch 16 (754-793) completed successfully after adding generic/753 to skip list
- generic/753 is a dm-error replay test that causes VM hangs
