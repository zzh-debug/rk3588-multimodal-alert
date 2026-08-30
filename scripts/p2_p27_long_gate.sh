#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

evidence_dir=${1:-/tmp/p2-p27-long-gate}
stream_url=${P2_RTSP_URL:-rtsp://127.0.0.1:8554/p2}
duration_seconds=${P2_DURATION_SEC:-60}
case "$duration_seconds" in
    ''|*[!0-9]*)
        printf '%s\n' 'P2_DURATION_SEC must be an integer' >&2
        exit 2
        ;;
esac
if [ "$duration_seconds" -eq 0 ]; then
    printf '%s\n' 'P2_DURATION_SEC must be positive' >&2
    exit 2
fi
mkdir -p "$evidence_dir"
rm -f "$evidence_dir"/*

cat /proc/meminfo >"$evidence_dir/meminfo-before.txt"
df -h >"$evidence_dir/df-before.txt"
sha256sum /usr/bin/p2_multimodal_fusion /usr/bin/p2_mpp_stream \
    >"$evidence_dir/binary-sha256.txt"
mediamtx /etc/mediamtx.yml >"$evidence_dir/mediamtx.log" 2>&1 &
mediamtx_pid=$!
app_pid=
cleanup()
{
    if [ -n "$app_pid" ]; then
        kill "$app_pid" 2>/dev/null || true
        wait "$app_pid" 2>/dev/null || true
    fi
    kill "$mediamtx_pid" 2>/dev/null || true
    wait "$mediamtx_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 1
/usr/bin/p2_multimodal_fusion --duration-sec "$duration_seconds" \
    --rtsp-url "$stream_url" \
    --summary "$evidence_dir/summary.json" \
    --events "$evidence_dir/events.csv" \
    >"$evidence_dir/application.log" 2>&1 &
app_pid=$!
sleep 8
timeout 12 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
    -i "$stream_url" -t 5 -an -f null - \
    >"$evidence_dir/client.log" 2>&1
wait "$app_pid"
app_pid=
kill "$mediamtx_pid" 2>/dev/null || true
wait "$mediamtx_pid" 2>/dev/null || true
mediamtx_pid=
cat /proc/meminfo >"$evidence_dir/meminfo-after.txt"
df -h >"$evidence_dir/df-after.txt"
grep -q '"result": "PASS"' "$evidence_dir/summary.json"
grep -q '"publish_failures": 0' "$evidence_dir/summary.json"
grep -q '"process":' "$evidence_dir/summary.json"
grep -q '"open_fds":' "$evidence_dir/summary.json"
/usr/bin/p2_multimodal_fusion --duration-sec 3 \
    --summary "$evidence_dir/reopen-summary.json" \
    --events "$evidence_dir/reopen-events.csv" \
    >"$evidence_dir/reopen.log" 2>&1
grep -q '"result": "PASS"' "$evidence_dir/reopen-summary.json"
test "$(cat /sys/class/leds/zzh:white:fill/brightness)" -eq 0
cat "$evidence_dir/summary.json"
