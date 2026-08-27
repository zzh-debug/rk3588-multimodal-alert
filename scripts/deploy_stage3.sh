#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu

repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build_dir=${P2_AARCH64_BUILD_DIR:-"$repo_dir/build/aarch64"}
sdk_root=${P2_SDK_ROOT:-/rk3588_dev}
rootfs=${P2_NFS_ROOTFS:-"$sdk_root/nfs_rootfs/atk_dlrk3588"}
model_source=${P2_YOLOV5_MODEL:-"$sdk_root/external/rknpu2/examples/rknn_yolov5_demo/model/RK3588/yolov5s-640-640.rknn"}
expected_model_sha=7c6b801de602b8aaa72269fab8daab21810418a4b01c8e8995313916e278fe71

for binary in p2_person_detect p2_person_detect_nv12
do
    [ -x "$build_dir/$binary" ] || {
        printf 'missing AArch64 binary: %s/%s\n' "$build_dir" "$binary" >&2
        exit 1
    }
done
[ -f "$model_source" ] || {
    printf 'missing SDK YOLOv5 model: %s\n' "$model_source" >&2
    exit 1
}
actual_model_sha=$(sha256sum "$model_source" | awk '{print $1}')
[ "$actual_model_sha" = "$expected_model_sha" ] || {
    printf 'unexpected YOLOv5 model SHA-256: %s\n' "$actual_model_sha" >&2
    exit 1
}
[ -d "$rootfs/usr/bin" ] || {
    printf 'missing NFS RootFS: %s\n' "$rootfs" >&2
    exit 1
}

sudo install -d -m 0755 "$rootfs/usr/share/p2/models"
for binary in p2_person_detect p2_person_detect_nv12
do
    sudo install -m 0755 "$build_dir/$binary" \
        "$rootfs/usr/bin/$binary.new"
    sudo mv "$rootfs/usr/bin/$binary.new" "$rootfs/usr/bin/$binary"
done
sudo install -m 0644 "$model_source" \
    "$rootfs/usr/share/p2/models/yolov5s-640-640.rknn.new"
sudo mv "$rootfs/usr/share/p2/models/yolov5s-640-640.rknn.new" \
    "$rootfs/usr/share/p2/models/yolov5s-640-640.rknn"

sha256sum "$build_dir/p2_person_detect" \
    "$build_dir/p2_person_detect_nv12" \
    "$rootfs/usr/bin/p2_person_detect" \
    "$rootfs/usr/bin/p2_person_detect_nv12" \
    "$rootfs/usr/share/p2/models/yolov5s-640-640.rknn"
