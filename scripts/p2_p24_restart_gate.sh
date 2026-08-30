#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

rounds=${1:-20}
binary=${P2_ASYNC_BINARY:-/usr/bin/p2_person_detect_async}
output_dir=${P2_RESTART_OUTPUT_DIR:-/tmp/p2-p24-restart-gate}

mkdir -p "$output_dir"
round=1
while [ "$round" -le "$rounds" ]; do
    summary="$output_dir/round-$round.json"
    "$binary" --duration-sec 1 --warmup 0 --json "$summary" \
        > "$output_dir/round-$round.log" 2>&1
    grep -q '"sequence_gaps": 0' "$summary"
    grep -q '"bad_bytes_used": 0' "$summary"
    grep -q '"pending": 0' "$summary"
    grep -q '"rknn_io_mem": {"enabled": true' "$summary"
    round=$((round + 1))
done

"$binary" --duration-sec 60 --warmup 0 \
    > "$output_dir/interrupted.log" 2>&1 &
interrupted_pid=$!
sleep 2
kill -TERM "$interrupted_pid"
wait "$interrupted_pid"

"$binary" --duration-sec 2 --warmup 0 \
    --json "$output_dir/reopen.json" \
    > "$output_dir/reopen.log" 2>&1
grep -q '"sequence_gaps": 0' "$output_dir/reopen.json"
grep -q '"bad_bytes_used": 0' "$output_dir/reopen.json"
grep -q '"pending": 0' "$output_dir/reopen.json"
grep -q '"rknn_io_mem": {"enabled": true' "$output_dir/reopen.json"

printf 'restart_rounds=%s interrupted_reopen=pass\n' "$rounds"
