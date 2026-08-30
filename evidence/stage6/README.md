# P2.6 PWM自动补光板端证据（2026-08-30）

本目录保存P2.6.1状态机与集成提交的精选板端证据（精确实现提交
`0a510dea32603a9e261c0935eb94d4777856ca92`，文档提交独立记录）。P2.6的目标是证明亮度/AIQ
遥测、持续热目标驱动PWM、状态滞回和安全退出；不把受控阈值触发写成真实夜间
照度或模型精度提升。

| 文件 | 内容 |
|---|---|
| `default-summary.json` | 默认模式10秒基线，自动补光未启用 |
| `auto-normal-summary.json` | 正常照明下开启自动补光，确认0误激活 |
| `controller-cycle-summary.json` | `p2_illumination_probe`暗场/热目标受控循环 |
| `integrated-trigger-summary.json` | `p2_multimodal_fusion`真实融合结果驱动PWM受控循环 |
| `*-events.csv` | 对应可见/热融合和状态机逐帧事件 |
| `*-illumination.csv` | 集成模式的亮度、曝光、增益、状态和目标记录 |
| `*.log` | 对应程序完整标准输出 |
| `binary-sha256.txt` | 板端程序哈希 |
| `pwm-final.txt` | 门禁结束时白光PWM状态 |
| `commands.txt` | 构建、部署和门禁复现命令 |
| `environment.txt` | 板端系统、内核、AIQ和依赖版本 |

## 结果

- 原生状态机、luma采样和几何/融合回归共13/13测试通过。
- 默认模式10秒：294/295可见帧、74热融合对，自动补光禁用，LED保持0。
- 正常照明自动模式10秒：294可见帧、76热融合对，0次误激活，0次控制故障，
  最终brightness 0；当前环境P50/P90约109/161，高于默认暗场阈值60/110。
- 控制器受控循环12秒：1次激活、1次释放、最大brightness 32、17次写入、0失败，
  最终brightness 0。
- 集成受控触发8秒：59热融合对，真实`person_with_thermal_evidence`触发
  `DARK_PENDING -> RAMP -> ON`，illumination自身1次激活/1次关闭，目标brightness 8，
  退出显式`shutdown -> OFF`，最终brightness 0，0次故障。
- 开发板白光PWM周期保持50000 ns（20 kHz），门禁结束duty为0 ns。

受控集成测试提高了暗场阈值以覆盖当前实验室画面，只证明业务融合结果和真实
sysfs PWM之间的连接及安全关闭。没有示波器、电流计、照度计和温度计，不声明
PWM波形、电流、功耗、照度增益或温升。
