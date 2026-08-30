#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

evidence_dir=${1:-/tmp/p2-p25-stream-gate}
stream_url=${P2_RTSP_URL:-rtsp://127.0.0.1:8554/p2}
duration_sec=${P2_DURATION_SEC:-60}
mkdir -p "$evidence_dir"
rm -f "$evidence_dir"/*

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
sha256sum /usr/bin/p2_multimodal_fusion /usr/bin/p2_mpp_stream \
    >"$evidence_dir/binary-sha256.txt"
/usr/bin/p2_multimodal_fusion --duration-sec "$duration_sec" \
    --rtsp-url "$stream_url" \
    --h264-output "$evidence_dir/stream.h264" \
    --summary "$evidence_dir/summary.json" \
    --events "$evidence_dir/events.csv" \
    >"$evidence_dir/application.log" 2>&1 &
app_pid=$!
sleep 8

timeout 10 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
    -i "$stream_url" -t 5 -an -f null - \
    >"$evidence_dir/client-1.log" 2>&1
sleep 10
timeout 10 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
    -i "$stream_url" -t 5 -an -f null - \
    >"$evidence_dir/client-2.log" 2>&1

wait "$app_pid"
app_pid=
ffprobe -v error -select_streams v:0 -count_frames \
    -show_entries stream=codec_name,profile,width,height,pix_fmt,r_frame_rate,avg_frame_rate,nb_read_frames \
    -of json "$evidence_dir/stream.h264" >"$evidence_dir/ffprobe.json"
ffmpeg -hide_banner -loglevel error -i "$evidence_dir/stream.h264" \
    -ss 30 -frames:v 1 -y "$evidence_dir/frame.jpg"
sha256sum "$evidence_dir/stream.h264" \
    >"$evidence_dir/stream-sha256.txt"

grep -q '"result": "PASS"' "$evidence_dir/summary.json"
grep -q '"publish_failures": 0' "$evidence_dir/summary.json"
grep -q '"width": 1080' "$evidence_dir/ffprobe.json"
grep -q '"height": 1920' "$evidence_dir/ffprobe.json"
test "$(grep -c 'is reading from path' "$evidence_dir/mediamtx.log")" -ge 2
cat "$evidence_dir/summary.json"
cat "$evidence_dir/ffprobe.json"
