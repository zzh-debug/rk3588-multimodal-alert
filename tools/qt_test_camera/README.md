# qt_test_camera

自验证项目一自研驱动的 Qt + V4L2 测试相机（裸 V4L2 采集，不依赖 Qt Multimedia）。

双路实时预览：可见光（IMX415）与红外热成像（MLX90640），同轴同向并排显示，用于
快速验证驱动出流、方向与伪彩热力图。

## 关键结论（板端实测通过）

- 目标节点：rkisp0-vir0 mainpath = /dev/video45（板端重启后编号会变，用
  `v4l2-ctl --list-devices` 定位 rkisp_mainpath）
- 节点类型：`V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`，NV12 只有 1 个 plane
- 出流证明：zzh_imx415 3-001a stream on 3864x2192 SGBRG10 4 lanes 891Mbps/lane

## 编译（宿主机，无需容器/qmake）

    sh build.sh

## 部署（容器内 vscode 免密 sudo）

    sudo install -m 0755 qt_test_camera /rk3588_dev/nfs_rootfs/atk_dlrk3588/usr/bin/qt_test_camera
    sudo install -m 0755 qt_test_camera /rk3588_dev/nfs_rootfs/atk_dlrk3588/opt/ui/src/apps/qt_test_camera

板载 `systemui` 从 `/opt/ui/src/ATK-DLRK3588/apk1.cfg` 读取应用名，并优先
执行 `/opt/ui/src/apps/qt_test_camera`；Wayland/桌面快捷方式使用
`/usr/bin/qt_test_camera`。两处必须部署同一个构建产物，否则不同入口会启动
不同版本。`qt_test_camera.desktop` 使用 `/usr/bin` 绝对路径，避免 PATH 歧义。

## 板端运行（weston）

    export XDG_RUNTIME_DIR=/run QT_QPA_PLATFORM=wayland
    qt_test_camera /dev/video45 /dev/v4l-subdev2 /dev/video0 3840 2160

参数依次为可见光 RKISP mainpath、可见光 sensor subdev、热成像 metadata
节点以及可见光输入分辨率。桌面快捷方式使用相同参数。

抓帧按钮保存 `/tmp/frame.bmp`。界面初始化时通过 `VIDIOC_G_CTRL` 读取当前曝光
和模拟增益；滑块显示的是当前硬件控制值，不是 `VIDIOC_QUERYCTRL` 的默认值。

## 显示布局与方向校正

屏幕为 MIPI DSI 竖屏（1080×1920，设备树 `hactive=0x438`/`vactive=0x780`）。
为方便对比，采用**上下分屏**：上半屏可见光、下半屏红外，两者横向显示。

方向校正参数集中在 `main.cpp` 顶部两个宏，改动后重新编译即可，无需改逻辑：

```c
#define ROT_ANGLE 0   // 0/90/180/270（顺时针），相机 NV12 的旋转角度
#define FLIP_H 1      // 1=水平翻转（左右镜像校正）
```

- **可见光（IMX415 竖装）**：原始 buffer 横向、画面镜像颠倒，用 `ROT_ANGLE=0`
  + `FLIP_H=1` 校正。注意竖装导致源数据的行/列与屏幕左右上下互换，因此
  `FLIP_H` 翻转的是 `sy`（源行）而非 `sx`（源列）。
- **红外（MLX90640 横向安装）**：热力图 32×24，`QTransform().rotate(90)`
  顺时针旋转 90 度后与可见光同轴同向。

`nv12_to_rgb_rot()` 做 NV12→RGB888 的旋转/翻转/降采样；`copy_rgb888_to_qimage()`
按 `QImage::scanLine()` 逐行复制，避免显示宽度非 4 字节对齐时的行填充错位。

## 踩坑记录

1. MPLANE：rkisp_mainpath 是 multiplanar，必须用 `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE`；
   NV12 `num_planes=1`，Y 前 UV 后连续，sizeimage=12441600。
2. `v4l2_buffer.m.planes` 是指针，必须分配 `v4l2_plane` 并让 `buf.m.planes`
   指向它，否则 `QUERYBUF` 报 Bad address。
3. 布局内 QLabel 若用 `setPixmap(scaled(label->size()))` 会形成正反馈：pixmap
   增大 sizeHint → 布局给更多空间 → size 再增大，导致热成像栏无限扩张把控制条
   挤出屏幕。用固定目标尺寸缩放即可避免。
4. 方向校正（旋转/镜像）务必在板端用「手掌左右平移」实测，不要凭竖装/横装推断；
   源数据行/列与屏幕坐标的对应关系受 sensor 安装朝向影响，很容易搞反。
