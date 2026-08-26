#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

count=${1:-100}
output_root=${2:-/tmp/p2-stage1-reopen}
binary=${P2_STAGE1_BINARY:-/usr/bin/p2_capture_sync}

case "$count" in
    ''|*[!0-9]*) echo 'count must be a positive integer' >&2; exit 2 ;;
esac
[ "$count" -gt 0 ] || { echo 'count must be a positive integer' >&2; exit 2; }
[ -x "$binary" ] || { echo "missing executable: $binary" >&2; exit 1; }

rm -rf "$output_root"
mkdir -p "$output_root"
passed=0
start_ns=$(date +%s)

i=1
while [ "$i" -le "$count" ]; do
    run_dir="$output_root/run-$(printf '%03d' "$i")"
    if "$binary" --duration 1 --output "$run_dir" >"$run_dir.stdout" 2>&1; then
        grep -q '^P2_STAGE1_CAPTURE_SYNC=PASS$' "$run_dir.stdout" || {
            echo "run $i did not report PASS" >&2
            cat "$run_dir.stdout" >&2
            exit 1
        }
        [ -s "$run_dir/summary.json" ] || {
            echo "run $i did not produce summary.json" >&2
            exit 1
        }
        passed=$((passed + 1))
    else
        echo "run $i failed" >&2
        cat "$run_dir.stdout" >&2
        exit 1
    fi
    i=$((i + 1))
done

elapsed=$(( $(date +%s) - start_ns ))
printf 'REOPEN_RUNS=%s\n' "$count"
printf 'REOPEN_PASSED=%s\n' "$passed"
printf 'REOPEN_ELAPSED_S=%s\n' "$elapsed"
printf 'P2_STAGE1_REOPEN=PASS\n'
