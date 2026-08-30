#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

SERVICE=/usr/bin/p2_multimodal_service.sh
SERVICE_DIR=/tmp/p2-p27-service
STREAM_URL=rtsp://127.0.0.1:8554/p2
LED=/sys/class/leds/zzh:white:fill/brightness
rm -rf "$SERVICE_DIR"
mkdir -p "$SERVICE_DIR"

test "$(cat "$LED")" -eq 0

P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" start >"$SERVICE_DIR/start.log" 2>&1
sleep 3
P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" status >"$SERVICE_DIR/status-start.log"
timeout 8 ffmpeg -hide_banner -loglevel error -rtsp_transport tcp \
    -i "$STREAM_URL" -t 2 -an -f null - \
    >"$SERVICE_DIR/client-before.log" 2>&1

app_pid=$(cat "$SERVICE_DIR/app.pid")
kill -TERM "$app_pid"
attempt=0
while [ "$attempt" -lt 12 ]; do
    restart_count=$(cat "$SERVICE_DIR/restart.count" 2>/dev/null || printf 0)
    new_app_pid=$(cat "$SERVICE_DIR/app.pid" 2>/dev/null || printf 0)
    case "$new_app_pid" in
        ''|*[!0-9]*) new_app_pid=0 ;;
    esac
    if [ "$restart_count" -ge 1 ] && [ "$new_app_pid" -ne "$app_pid" ]; then
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done
restart_count=$(cat "$SERVICE_DIR/restart.count" 2>/dev/null || printf 0)
[ "$restart_count" -ge 1 ]
attempt=0
while [ "$attempt" -lt 10 ]; do
    if timeout 8 ffmpeg -hide_banner -loglevel error \
        -rtsp_transport tcp -i "$STREAM_URL" -t 2 -an -f null - \
        >>"$SERVICE_DIR/client-after.log" 2>&1; then
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 10 ]
P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" status >"$SERVICE_DIR/status-recovered.log"

mediamtx_pid=$(cat "$SERVICE_DIR/mediamtx.pid")
kill -TERM "$mediamtx_pid"
attempt=0
while [ "$attempt" -lt 12 ]; do
    restart_count=$(cat "$SERVICE_DIR/restart.count" 2>/dev/null || printf 0)
    new_mediamtx_pid=$(cat "$SERVICE_DIR/mediamtx.pid" 2>/dev/null || printf 0)
    case "$new_mediamtx_pid" in
        ''|*[!0-9]*) new_mediamtx_pid=0 ;;
    esac
    if [ "$restart_count" -ge 2 ] &&
       [ "$new_mediamtx_pid" -ne "$mediamtx_pid" ]; then
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 12 ]
attempt=0
while [ "$attempt" -lt 10 ]; do
    if timeout 8 ffmpeg -hide_banner -loglevel error \
        -rtsp_transport tcp -i "$STREAM_URL" -t 2 -an -f null - \
        >>"$SERVICE_DIR/client-after-mediamtx.log" 2>&1; then
        break
    fi
    sleep 1
    attempt=$((attempt + 1))
done
[ "$attempt" -lt 10 ]
P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" status \
    >"$SERVICE_DIR/status-media-recovered.log"

P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" stop >"$SERVICE_DIR/status-stop.log"
test "$(cat "$LED")" -eq 0
if P2_SERVICE_DIR="$SERVICE_DIR" "$SERVICE" status \
    >"$SERVICE_DIR/status-final.log" 2>&1; then
    printf '%s\n' 'service remained running after stop' >&2
    exit 1
fi
if ps -ef | grep -E '[p]2_multimodal|[m]ediamtx' >/dev/null 2>&1; then
    printf '%s\n' 'service child remained after stop' >&2
    exit 1
fi

/usr/bin/p2_multimodal_fusion --duration-sec 20 --summary \
    "$SERVICE_DIR/resource-summary.json" \
    --events "$SERVICE_DIR/resource-events.csv" \
    >"$SERVICE_DIR/resource.log" 2>&1
grep -q '"result": "PASS"' "$SERVICE_DIR/resource-summary.json"
grep -q '"process":' "$SERVICE_DIR/resource-summary.json"
grep -q '"open_fds":' "$SERVICE_DIR/resource-summary.json"
test "$(cat "$LED")" -eq 0
cat "$SERVICE_DIR/resource-summary.json"
cat "$SERVICE_DIR/status-recovered.log"
