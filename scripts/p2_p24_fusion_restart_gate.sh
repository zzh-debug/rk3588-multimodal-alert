#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

rounds=${1:-5}
output_dir=${2:-/tmp/p2-fusion-restart-gate}
binary=${P2_FUSION_BINARY:-/usr/bin/p2_multimodal_fusion}

case "$rounds" in
    ''|*[!0-9]*)
        echo "round count must be a positive integer" >&2
        exit 2
        ;;
esac
if [ "$rounds" -eq 0 ]; then
    echo "round count must be positive" >&2
    exit 2
fi

mkdir -p "$output_dir"
round=1
while [ "$round" -le "$rounds" ]; do
    tag=$(printf '%02d' "$round")
    summary="$output_dir/round-$tag.json"
    events="$output_dir/round-$tag.csv"
    "$binary" --duration-sec 30 --pairs 5 \
        --summary "$summary" --events "$events" \
        >"$output_dir/round-$tag.log" 2>&1
    if ! grep -q '"result": "PASS"' "$summary"; then
        echo "fusion restart round $round failed" >&2
        exit 1
    fi
    round=$((round + 1))
done

cat >"$output_dir/summary.json" <<EOF
{
  "schema": "p2.multimodal-fusion-restart.v1",
  "result": "PASS",
  "rounds": $rounds,
  "fused_pair_target_per_round": 5
}
EOF
cat "$output_dir/summary.json"
