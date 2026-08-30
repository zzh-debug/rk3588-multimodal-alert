#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

evidence_dir=${1:-/tmp/p2-p25-restart-gate}
stream_url=${P2_RTSP_URL:-rtsp://127.0.0.1:8554/p2}
rounds=${P2_RESTART_ROUNDS:-5}
mkdir -p "$evidence_dir"
rm -f "$evidence_dir"/*

mediamtx /etc/mediamtx.yml >"$evidence_dir/mediamtx.log" 2>&1 &
mediamtx_pid=$!
cleanup()
{
    kill "$mediamtx_pid" 2>/dev/null || true
    wait "$mediamtx_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM
sleep 1

round=1
while [ "$round" -le "$rounds" ]; do
    /usr/bin/p2_multimodal_fusion --duration-sec 8 \
        --rtsp-url "$stream_url" \
        --summary "$evidence_dir/summary-$round.json" \
        --events "$evidence_dir/events-$round.csv" \
        >"$evidence_dir/application-$round.log" 2>&1 &
    app_pid=$!
    sleep 3
    timeout 5 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
        -i "$stream_url" -t 2 -an -f null - \
        >"$evidence_dir/client-$round.log" 2>&1
    wait "$app_pid"
    grep -q '"result": "PASS"' "$evidence_dir/summary-$round.json"
    grep -q '"publish_failures": 0' "$evidence_dir/summary-$round.json"
    round=$((round + 1))
done

test "$(grep -c 'is publishing to path' "$evidence_dir/mediamtx.log")" \
    -eq "$rounds"
test "$(grep -c 'is reading from path' "$evidence_dir/mediamtx.log")" \
    -eq "$rounds"
