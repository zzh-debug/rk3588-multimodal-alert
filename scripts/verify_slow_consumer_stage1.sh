#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

OUTPUT_DIR=${1:-/tmp/p2-stage1-slow-consumer}
DURATION=${2:-20}
DELAY_MS=${3:-50}
CAPACITY=${4:-64}
BINARY=${P2_STAGE1_BINARY:-/usr/bin/p2_capture_sync}

fail()
{
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

case "$DURATION:$DELAY_MS:$CAPACITY" in
    ''|*[!0-9:]*) fail 'duration, delay and capacity must be integers' ;;
esac
[ "$DURATION" -gt 0 ] || fail 'duration must be positive'
[ "$CAPACITY" -gt 0 ] || fail 'capacity must be positive'
[ -x "$BINARY" ] || fail "missing executable: $BINARY"

rm -rf "$OUTPUT_DIR"
if ! "$BINARY" --duration "$DURATION" --consumer-delay-ms "$DELAY_MS" \
    --queue-capacity "$CAPACITY" --output "$OUTPUT_DIR" \
    >"$OUTPUT_DIR.stdout" 2>&1; then
    cat "$OUTPUT_DIR.stdout" >&2
    fail 'slow-consumer run did not complete successfully'
fi

grep -q '^P2_STAGE1_CAPTURE_SYNC=PASS$' "$OUTPUT_DIR.stdout" ||
    fail 'slow-consumer run did not report PASS'
queue_drops=$(sed -n 's/^EVENT_QUEUE_DROPS=\([0-9][0-9]*\)$/\1/p' \
    "$OUTPUT_DIR.stdout" | tail -n 1)
[ -n "$queue_drops" ] || fail 'queue drop count missing'
[ "$queue_drops" -gt 0 ] || fail 'slow consumer did not exercise bounded queue'

printf 'DURATION_S=%s\n' "$DURATION"
printf 'CONSUMER_DELAY_MS=%s\n' "$DELAY_MS"
printf 'QUEUE_CAPACITY=%s\n' "$CAPACITY"
printf 'EVENT_QUEUE_DROPS=%s\n' "$queue_drops"
printf 'P2_STAGE1_SLOW_CONSUMER=PASS\n'
