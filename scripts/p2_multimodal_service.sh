#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

SERVICE_DIR=${P2_SERVICE_DIR:-/tmp/p2-multimodal-service}
APP=${P2_SERVICE_APP:-/usr/bin/p2_multimodal_fusion}
MEDIAMTX=${P2_SERVICE_MEDIAMTX:-/usr/bin/mediamtx}
MEDIAMTX_CONFIG=${P2_SERVICE_MEDIAMTX_CONFIG:-/etc/mediamtx.yml}
RTSP_URL=${P2_SERVICE_RTSP_URL:-rtsp://127.0.0.1:8554/p2}
LED_BRIGHTNESS=${P2_SERVICE_LED_BRIGHTNESS:-/sys/class/leds/zzh:white:fill/brightness}

SUPERVISOR_PID="$SERVICE_DIR/supervisor.pid"
APP_PID="$SERVICE_DIR/app.pid"
MEDIAMTX_PID="$SERVICE_DIR/mediamtx.pid"
STOP_FILE="$SERVICE_DIR/stop.request"
RESTART_FILE="$SERVICE_DIR/restart.count"
LOG_FILE="$SERVICE_DIR/service.log"

is_alive()
{
    kill -0 "$1" 2>/dev/null
}

read_pid()
{
    [ -r "$1" ] || return 1
    pid=$(cat "$1")
    case "$pid" in
        ''|*[!0-9]*) return 1 ;;
    esac
    printf '%s\n' "$pid"
}

write_zero()
{
    if [ -w "$LED_BRIGHTNESS" ]; then
        printf '0' >"$LED_BRIGHTNESS" 2>/dev/null || true
    fi
}

status()
{
    supervisor=$(read_pid "$SUPERVISOR_PID" 2>/dev/null || true)
    app=$(read_pid "$APP_PID" 2>/dev/null || true)
    mediamtx=$(read_pid "$MEDIAMTX_PID" 2>/dev/null || true)
    if [ -n "$supervisor" ] && is_alive "$supervisor"; then
        printf 'RUNNING supervisor=%s app=%s mediamtx=%s restarts=%s\n' \
            "$supervisor" "${app:-none}" "${mediamtx:-none}" \
            "$(cat "$RESTART_FILE" 2>/dev/null || printf 0)"
        return 0
    fi
    printf '%s\n' 'STOPPED'
    return 1
}

cleanup()
{
    trap - EXIT INT TERM
    app=$(read_pid "$APP_PID" 2>/dev/null || true)
    mediamtx=$(read_pid "$MEDIAMTX_PID" 2>/dev/null || true)
    if [ -n "$app" ] && is_alive "$app"; then
        kill -TERM "$app" 2>/dev/null || true
        wait "$app" 2>/dev/null || true
    fi
    if [ -n "$mediamtx" ] && is_alive "$mediamtx"; then
        kill -TERM "$mediamtx" 2>/dev/null || true
        wait "$mediamtx" 2>/dev/null || true
    fi
    write_zero
    rm -f "$SUPERVISOR_PID" "$APP_PID" "$MEDIAMTX_PID"
}

run_supervisor()
{
    mkdir -p "$SERVICE_DIR"
    rm -f "$STOP_FILE"
    printf '%s\n' "$$" >"$SUPERVISOR_PID"
    printf '0\n' >"$RESTART_FILE"
    trap cleanup EXIT INT TERM
    while [ ! -e "$STOP_FILE" ]; do
        "$MEDIAMTX" "$MEDIAMTX_CONFIG" >>"$LOG_FILE" 2>&1 &
        mediamtx_pid=$!
        printf '%s\n' "$mediamtx_pid" >"$MEDIAMTX_PID"
        sleep 1
        if [ -e "$STOP_FILE" ]; then
            break
        fi
        if [ "${P2_AUTO_ILLUMINATION:-0}" = '1' ]; then
            "$APP" --duration-sec 86400 --rtsp-url "$RTSP_URL" \
                --auto-illumination \
                --illumination-events "$SERVICE_DIR/illumination.csv" \
                --summary "$SERVICE_DIR/summary.json" \
                --events "$SERVICE_DIR/events.csv" \
                >>"$LOG_FILE" 2>&1 &
        else
            "$APP" --duration-sec 86400 --rtsp-url "$RTSP_URL" \
                --summary "$SERVICE_DIR/summary.json" \
                --events "$SERVICE_DIR/events.csv" \
                >>"$LOG_FILE" 2>&1 &
        fi
        app_pid=$!
        printf '%s\n' "$app_pid" >"$APP_PID"
        while [ ! -e "$STOP_FILE" ] &&
              is_alive "$app_pid" && is_alive "$mediamtx_pid"; do
            sleep 1
        done
        app_alive=0
        mediamtx_alive=0
        is_alive "$app_pid" && app_alive=1 || true
        is_alive "$mediamtx_pid" && mediamtx_alive=1 || true
        if [ "$app_alive" -eq 1 ]; then
            kill -TERM "$app_pid" 2>/dev/null || true
        fi
        if [ "$mediamtx_alive" -eq 1 ]; then
            kill -TERM "$mediamtx_pid" 2>/dev/null || true
        fi
        wait "$app_pid" 2>/dev/null || true
        wait "$mediamtx_pid" 2>/dev/null || true
        rm -f "$APP_PID" "$MEDIAMTX_PID"
        if [ -e "$STOP_FILE" ]; then
            break
        fi
        restart_count=$(cat "$RESTART_FILE" 2>/dev/null || printf 0)
        restart_count=$((restart_count + 1))
        printf '%s\n' "$restart_count" >"$RESTART_FILE"
        sleep 1
    done
}

mkdir -p "$SERVICE_DIR"
command=${1:-status}
case "$command" in
    start)
        if status >/dev/null 2>&1; then
            printf '%s\n' 'already running'
            exit 0
        fi
        rm -f "$STOP_FILE"
        nohup "$0" supervise >>"$LOG_FILE" 2>&1 </dev/null &
        printf '%s\n' "$!" >"$SUPERVISOR_PID"
        sleep 1
        status
        ;;
    stop)
        supervisor=$(read_pid "$SUPERVISOR_PID" 2>/dev/null || true)
        if [ -n "$supervisor" ] && is_alive "$supervisor"; then
            : >"$STOP_FILE"
            kill -TERM "$supervisor" 2>/dev/null || true
            attempt=0
            while is_alive "$supervisor" && [ "$attempt" -lt 10 ]; do
                sleep 1
                attempt=$((attempt + 1))
            done
        fi
        write_zero
        rm -f "$SUPERVISOR_PID" "$APP_PID" "$MEDIAMTX_PID" "$STOP_FILE"
        printf '%s\n' 'STOPPED'
        ;;
    restart)
        "$0" stop
        "$0" start
        ;;
    status)
        status
        ;;
    supervise)
        run_supervisor
        ;;
    *)
        printf 'usage: %s {start|stop|restart|status}\n' "$0" >&2
        exit 2
        ;;
esac
