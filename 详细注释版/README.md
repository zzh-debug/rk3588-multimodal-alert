# 项目二详细注释版

项目二业务代码尚未开始。本目录已经预留，后续按模块保存真实源码的教学镜像：

```text
application/
  capture/       V4L2 可见光与热阵列采集
  sync/          CLOCK_MONOTONIC 最近邻配对
  thermal/       EEPROM 参数和温度算法
  calibration/   区域级跨光谱映射
  inference/     RGA/RKNN 人员检测与融合
  pipeline/      DMA-BUF 多池、队列和生命周期
  streaming/     MPP、OSD、RTSP
  illumination/  PWM 补光状态机
```

当前不复制 RKNN、RGA、MPP 或 RKADK 厂商示例作为“项目二源码”。项目二每产生
一个经过板测的权威实现，才增加相应注释版，并记录函数输入输出、内存/Buffer
所有权、线程上下文、同步关系和失败回退。
