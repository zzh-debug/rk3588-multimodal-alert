#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

OUTPUT_DIR=${1:-/tmp/p2-stage1-l1-reopen}
FAULT_DURATION=${2:-30}
RECOVERY_DURATION=${3:-10}
PARAM_ROOT=/sys/module/video_rkisp/parameters
BINARY=${P2_STAGE1_BINARY:-/usr/bin/p2_capture_sync}

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

positive_integer()
{
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
    esac
    [ "$1" -gt 0 ]
}

positive_integer "$FAULT_DURATION" || fail 'fault duration must be positive'
positive_integer "$RECOVERY_DURATION" || fail 'recovery duration must be positive'
[ "$(id -u)" -eq 0 ] || fail 'run as root'
[ -x "$BINARY" ] || fail "missing executable: $BINARY"
for parameter in l1_timeout_enable l1_timeout_ms l1_fault_drop_heartbeat; do
    [ -w "$PARAM_ROOT/$parameter" ] || fail "missing writable parameter: $parameter"
done

mkdir -p "$OUTPUT_DIR"
FAULT_LOG=$OUTPUT_DIR/fault.log
RECOVERY_LOG=$OUTPUT_DIR/recovery.log
FAULT_DIR=$OUTPUT_DIR/fault
RECOVERY_DIR=$OUTPUT_DIR/recovery
SUMMARY=$OUTPUT_DIR/summary.log

original_enable=$(cat "$PARAM_ROOT/l1_timeout_enable")
original_timeout=$(cat "$PARAM_ROOT/l1_timeout_ms")
original_drop=$(cat "$PARAM_ROOT/l1_fault_drop_heartbeat")
restore_params()
{
    printf '%s' "$original_drop" >"$PARAM_ROOT/l1_fault_drop_heartbeat" || true
    printf '%s' "$original_timeout" >"$PARAM_ROOT/l1_timeout_ms" || true
    printf '%s' "$original_enable" >"$PARAM_ROOT/l1_timeout_enable" || true
}
trap restore_params EXIT INT TERM

printf 500 >"$PARAM_ROOT/l1_timeout_ms"
printf 1 >"$PARAM_ROOT/l1_timeout_enable"
printf 1 >"$PARAM_ROOT/l1_fault_drop_heartbeat"

fault_status=0
timeout 10 "$BINARY" --duration "$FAULT_DURATION" --output "$FAULT_DIR" \
    >"$FAULT_LOG" 2>&1 || fault_status=$?
[ "$fault_status" -ne 124 ] || fail 'fault run reached outer timeout'
[ "$fault_status" -ne 0 ] || fail 'fault run unexpectedly succeeded'
grep -q 'Input/output error' "$FAULT_LOG" ||
    fail 'fault run did not report userspace EIO'
grep -q '^P2_STAGE1_CAPTURE_SYNC=FAIL$' "$FAULT_LOG" ||
    fail 'fault run did not produce the expected failure result'

printf 0 >"$PARAM_ROOT/l1_fault_drop_heartbeat"
timeout 30 "$BINARY" --duration "$RECOVERY_DURATION" --output "$RECOVERY_DIR" \
    >"$RECOVERY_LOG" 2>&1 || recovery_status=$?
recovery_status=${recovery_status:-0}
[ "$recovery_status" -eq 0 ] || fail 'reopened userspace session failed'
grep -q '^P2_STAGE1_CAPTURE_SYNC=PASS$' "$RECOVERY_LOG" ||
    fail 'reopened userspace session did not report PASS'

{
    printf 'FAULT_EXIT=%s\n' "$fault_status"
    printf 'FAULT_USERSPACE_EIO=PASS\n'
    printf 'RECOVERY_EXIT=%s\n' "$recovery_status"
    printf 'CLOSE_REOPEN=PASS\n'
    printf 'P2_STAGE1_L1_REOPEN=PASS\n'
} | tee "$SUMMARY"
