#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

evidence_dir=${1:-/tmp/p2-p26-illumination-gate}
mkdir -p "$evidence_dir"
rm -f "$evidence_dir"/*

led_path=/sys/class/leds/zzh:white:fill
test "$(cat "$led_path/brightness")" -eq 0
sha256sum /usr/bin/p2_multimodal_fusion /usr/bin/p2_illumination_probe \
    >"$evidence_dir/binary-sha256.txt"

/usr/bin/p2_multimodal_fusion --duration-sec 10 \
    --summary "$evidence_dir/default-summary.json" \
    --events "$evidence_dir/default-events.csv" \
    >"$evidence_dir/default.log" 2>&1
grep -q '"result": "PASS"' "$evidence_dir/default-summary.json"
grep -q '"enabled": false' "$evidence_dir/default-summary.json"
grep -q '"state": "OFF"' "$evidence_dir/default-summary.json"
grep -q '"led_final_brightness": 0' "$evidence_dir/default-summary.json"

/usr/bin/p2_multimodal_fusion --duration-sec 10 \
    --auto-illumination \
    --summary "$evidence_dir/auto-normal-summary.json" \
    --events "$evidence_dir/auto-normal-events.csv" \
    --illumination-events "$evidence_dir/auto-normal-illumination.csv" \
    >"$evidence_dir/auto-normal.log" 2>&1
grep -q '"result": "PASS"' "$evidence_dir/auto-normal-summary.json"
grep -q '"enabled": true' "$evidence_dir/auto-normal-summary.json"
grep -q '"activations": 0' "$evidence_dir/auto-normal-summary.json"
grep -q '"led_final_brightness": 0' "$evidence_dir/auto-normal-summary.json"

/usr/bin/p2_illumination_probe --duration-sec 12 --force-dark \
    --forced-thermal-sec 4 --write-led --target-brightness 32 \
    --summary "$evidence_dir/controller-cycle-summary.json" \
    --events "$evidence_dir/controller-cycle-events.csv" \
    >"$evidence_dir/controller-cycle.log" 2>&1
grep -q '"result": "PASS"' "$evidence_dir/controller-cycle-summary.json"
grep -q '"activations": 1' "$evidence_dir/controller-cycle-summary.json"
grep -q '"deactivations": 1' "$evidence_dir/controller-cycle-summary.json"
grep -q '"faults": 0' "$evidence_dir/controller-cycle-summary.json"
grep -q '"final_brightness": 0' "$evidence_dir/controller-cycle-summary.json"

/usr/bin/p2_multimodal_fusion --duration-sec 8 \
    --auto-illumination --illumination-target 8 \
    --illumination-dark-p50 140 --illumination-dark-p90 200 \
    --illumination-stop-p50 150 --illumination-stop-p90 220 \
    --summary "$evidence_dir/integrated-trigger-summary.json" \
    --events "$evidence_dir/integrated-trigger-events.csv" \
    --illumination-events "$evidence_dir/integrated-trigger-illumination.csv" \
    >"$evidence_dir/integrated-trigger.log" 2>&1
grep -q '"result": "PASS"' "$evidence_dir/integrated-trigger-summary.json"
grep -q '"activations": 1' "$evidence_dir/integrated-trigger-summary.json"
grep -q '"maximum_brightness": 8' "$evidence_dir/integrated-trigger-summary.json"
grep -q '"led_final_brightness": 0' "$evidence_dir/integrated-trigger-summary.json"

test "$(cat "$led_path/brightness")" -eq 0
grep -A1 white-fill /sys/kernel/debug/pwm >"$evidence_dir/pwm-final.txt"
