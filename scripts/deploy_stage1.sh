#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${P2_STAGE1_BINARY:-"$repo_dir/build/aarch64/p2_capture_sync"}
rootfs=${P2_NFS_ROOTFS:-/rk3588_dev/nfs_rootfs/atk_dlrk3588}

[ -x "$binary" ] || {
    printf 'missing AArch64 binary: %s\n' "$binary" >&2
    exit 1
}
[ -d "$rootfs/usr/bin" ] || {
    printf 'missing NFS RootFS: %s\n' "$rootfs" >&2
    exit 1
}

sudo install -m 0755 "$binary" "$rootfs/usr/bin/p2_capture_sync.new"
sudo mv "$rootfs/usr/bin/p2_capture_sync.new" \
    "$rootfs/usr/bin/p2_capture_sync"
sha256sum "$binary" "$rootfs/usr/bin/p2_capture_sync"
