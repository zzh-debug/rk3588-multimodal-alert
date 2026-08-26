# RK3588 多模态预警应用仓库约束

## 范围

- 本仓库只维护项目二的用户态应用、公共接口、配置、标定、模型清单、测试、文档和证据。
- Sensor、DTS、RKCIF/RKISP、MLX90640 Meta/VB2 和 LED PWM 驱动属于项目一，不在本仓库复制维护。
- `tools/qt_test_camera/` 是人工查看双路画面的辅助工具，不计入项目二业务功能和性能成果。
- `详细注释版/` 只保存已经过板测的正式源码教学镜像，不作为编译输入。

## 上游契约

- 项目一仓库位于同级目录 `../rk3588_camera_bsp`。
- 当前锁定项目一基线提交：`c140532b82723bdfe1396b51304f2dfe9c4c73e7`。
- MLX90640 用户态 ABI 的权威头文件为项目一的
  `include/uapi/linux/zzh_mlx90640_meta.h`；构建时从项目一引用，并记录提交和头文件哈希。
- 可见光节点必须通过 media graph 动态定位 `rkisp_mainpath`，热成像节点必须通过
  `VIDIOC_QUERYCAP`/`ZMLX` 格式识别，不得把 `/dev/video45`、`/dev/video0` 当作稳定接口。

## 构建与实现

- 正式应用采用 C++17 和 CMake；厂商 C API 使用薄封装，Buffer、fd 和线程生命周期使用 RAII 管理。
- 主机可验证的同步、队列、状态机和标定数学先做原生单元测试；硬件路径使用 Buildroot AArch64 交叉工具链构建并在开发板验证。
- 禁止以 root 或 sudo 编译；不得在 SDK 根目录初始化 Git。
- 第一版先用 MMAP 建立正确性基线，只有在 fd、stride、cache sync、ownership 和 profile 证据完整后才引入 DMA-BUF。
- 所有队列必须有容量上限、明确的满队列策略和分类丢弃计数；不得通过无限缓存掩盖消费者过慢。
- 业务时间统一使用 `CLOCK_MONOTONIC`；墙上时钟只用于会话标签，不参与帧匹配和超时判断。

## 成果与证据

- 编译成功、SDK 中存在厂商示例或单帧截图均不能替代真实板端验收。
- 每次正式运行记录应用提交、Image/DTB/module、IQ、模型、标定文件和配置哈希。
- DMA-BUF只表述为减少CPU全帧复制，不使用“全程零拷贝”。
- 模型必须记录来源、许可证、输入布局、量化方式和SHA-256；仓库默认只提交manifest，不提交大模型二进制。
- 没有参考温度计时，不声明MLX90640绝对测温精度；没有精确FOV和固定支架时，不声明像素级跨光谱对齐。
- LED自动控制必须故障默认关，并在退出和异常恢复路径写回brightness 0。

## Git与发布

- 独立仓库根目录仅为本目录，默认分支 `main`，远端
  `git@github-rk3588:zzh-debug/rk3588-multimodal-alert.git`。
- GitHub仓库当前为private；公开前检查第三方许可证、模型、标定数据、日志、设备信息和网络地址。
- 未经用户明确要求不推送、改写历史或公开仓库。
