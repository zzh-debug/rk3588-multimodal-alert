# qt_test_camera

自验证项目一自研驱动的 Qt + V4L2 测试相机（裸 V4L2 采集，不依赖 Qt Multimedia）。

## 关键结论（板端实测通过）

- 目标节点：rkisp0-vir0 mainpath = /dev/video45（板端重启后编号会变，用
  v4l2-ctl --list-devices 定位 rkisp_mainpath）
- 节点类型：V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE，NV12 只有 1 个 plane
- 出流证明：zzh_imx415 3-001a stream on 3864x2192 SGBRG10 4 lanes 891Mbps/lane

## 编译（宿主机，无需容器/qmake）

    sh build.sh

## 部署（容器内 vscode 免密 sudo）

    sudo install -m 0755 qt_test_camera /rk3588_dev/nfs_rootfs/atk_dlrk3588/usr/bin/qt_test_camera
    sudo install -m 0755 qt_test_camera /rk3588_dev/nfs_rootfs/atk_dlrk3588/opt/ui/src/apps/qt_test_camera

板载 `systemui` 从 `/opt/ui/src/ATK-DLRK3588/apk1.cfg` 读取应用名，并优先
执行 `/opt/ui/src/apps/qt_test_camera`；Wayland/桌面快捷方式使用
`/usr/bin/qt_test_camera`。两处必须部署同一个构建产物，否则不同入口会启动
不同版本。`qt_test_camera.desktop` 使用 `/usr/bin` 绝对路径，避免PATH歧义。

## 板端运行（weston）

    export XDG_RUNTIME_DIR=/run QT_QPA_PLATFORM=wayland
    qt_test_camera /dev/video45 /dev/v4l-subdev2 /dev/video0 3840 2160

参数依次为可见光 RKISP mainpath、可见光 sensor subdev、热成像 metadata
节点以及可见光输入分辨率。桌面快捷方式使用相同参数。

抓帧按钮保存 `/tmp/frame.bmp`。显示尺寸根据当前屏幕自动计算，并将 NV12
降采样、旋转为 RGB888 后显示。

界面初始化时通过 `VIDIOC_G_CTRL` 读取当前曝光和模拟增益；滑块显示的是
当前硬件控制值，不再显示 `VIDIOC_QUERYCTRL` 返回的默认值。RGB888 主画面
按 `QImage::scanLine()` 逐行复制，以正确处理显示宽度不是 4 字节对齐时的
行填充，避免画面灰白或 RGB 通道逐行错位。

## 踩坑记录

1. MPLANE：rkisp_mainpath 是 multiplanar，必须用 V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE；
   NV12 num_planes=1，Y 前 UV 后连续，sizeimage=12441600。
2. v4l2_buffer.m.planes 是指针，必须分配 v4l2_plane 并让 buf.m.planes 指向它，
   否则 QUERYBUF 报 Bad address。
