#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${P2_AARCH64_BUILD_DIR:-"$repo_dir/build/aarch64"}
camera_bsp_root=${CAMERA_BSP_ROOT:-"$repo_dir/../rk3588_camera_bsp"}

cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="$repo_dir/cmake/toolchains/buildroot-aarch64.cmake" \
    -DCAMERA_BSP_ROOT="$camera_bsp_root" \
    -DP2_BUILD_HARDWARE=ON \
    -DBUILD_TESTING=OFF
cmake --build "$build_dir" --parallel
for binary in \
    p2_capture_sync \
    p2_thermal_export \
    p2_thermal_math_probe \
    p2_thermal_realtime_probe \
    p2_calibration_capture \
    p2_calibration_gui \
    p2_person_detect \
    p2_person_detect_nv12 \
    p2_person_dataset_capture \
    p2_person_evaluate
do
    file "$build_dir/$binary"
done
