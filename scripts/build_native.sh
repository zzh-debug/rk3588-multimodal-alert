#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${P2_NATIVE_BUILD_DIR:-"$repo_dir/build/native"}

cmake -S "$repo_dir" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DP2_BUILD_HARDWARE=OFF \
    -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel
(cd "$build_dir" && ctest --output-on-failure)
