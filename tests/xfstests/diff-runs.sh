#!/bin/bash
# Diff two BrieFS xfstests run archives by per-test status.
#
# Each archive is produced by run-suite.sh (see runs/README.md).  This script
# extracts the diff-friendly per-test status block from each and prints every
# test whose category changed, plus a transition tally.
#
# Usage: diff-runs.sh <old.txt> <new.txt>
#
# Example output:
#   === status changes: old.txt -> new.txt ===
#     224: PASS -> HANG
#     464: PASS -> HANG
#     475: SKIPPED -> PASS
#     ---
#     PASS -> HANG: 2
#     SKIPPED -> PASS: 1
set -uo pipefail

[ $# -eq 2 ] || { echo "usage: $0 <old.txt> <new.txt>" >&2; exit 2; }
old="$1"; new="$2"
for f in "$old" "$new"; do
    [ -f "$f" ] || { echo "no such file: $f" >&2; exit 2; }
done

ta=$(mktemp); tb=$(mktemp)
trap 'rm -f "$ta" "$tb"' EXIT

# Pull the "NNN CATEGORY" lines from the per-test status section.  Plain sort
# (bytewise) matches numeric order for the zero-padded test numbers and is
# what join expects on its join field.
sed -n '/^# Per-test status/,$p' "$old" | grep -E '^[0-9]+ ' | sort > "$ta"
sed -n '/^# Per-test status/,$p' "$new" | grep -E '^[0-9]+ ' | sort > "$tb"

echo "=== status changes: $old -> $new ==="
join -a1 -a2 -e NONE -o '0 1.2 2.2' "$ta" "$tb" | \
    awk '$2 != $3 {
             printf "  %s: %s -> %s\n", $1, $2, $3
             ch[$2 " -> " $3]++
         }
         END {
             if (length(ch)) {
                 print "  ---"
                 for (k in ch) printf "  %s: %d\n", k, ch[k]
             } else {
                 print "  (no per-test status changes)"
             }
         }'