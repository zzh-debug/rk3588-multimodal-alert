#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${P2_AARCH64_BUILD_DIR:-"$repo_dir/build/aarch64"}
rootfs=${P2_NFS_ROOTFS:-/rk3588_dev/nfs_rootfs/atk_dlrk3588}
binaries="p2_capture_sync p2_thermal_export p2_thermal_math_probe p2_thermal_realtime_probe"

[ -d "$rootfs/usr/bin" ] || {
    printf 'missing NFS RootFS: %s\n' "$rootfs" >&2
    exit 1
}

for binary in $binaries
do
    [ -x "$build_dir/$binary" ] || {
        printf 'missing AArch64 binary: %s/%s\n' "$build_dir" "$binary" >&2
        exit 1
    }
    sudo install -m 0755 "$build_dir/$binary" \
        "$rootfs/usr/bin/$binary.new"
    sudo mv "$rootfs/usr/bin/$binary.new" "$rootfs/usr/bin/$binary"
    sha256sum "$build_dir/$binary" "$rootfs/usr/bin/$binary"
done
