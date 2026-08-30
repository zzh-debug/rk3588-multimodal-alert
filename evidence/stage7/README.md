# P2.7.1 整链可靠性板端证据（2026-08-30）

本目录保存supervisor实现提交`e45ec0b264b18cea3a05f7afd9b15270cfedcda9`、资源
字段修正后的精确板测提交`2c26c31d53032dc9c3a002c37707e49ac9b61e20`的60秒长稳、
supervisor恢复和资源摘要证据。P2.7整体仍保留“2小时长稳待确认”边界。

| 路径 | 内容 |
|---|---|
| `long/summary.json` | 60秒整链RTSP长稳摘要 |
| `long/events.csv` | 同次运行的融合事件 |
| `long/client.log` | 5秒RTSP客户端窗口，0字节表示无解码错误 |
| `long/mediamtx.log` | 60秒发布服务日志 |
| `long/meminfo-*`,`df-*` | 运行前后资源快照 |
| `recovery/service.log` | supervisor启动、业务故障和MediaMTX故障恢复日志 |
| `recovery/client-before.log` | 第一次故障前RTSP读取 |
| `recovery/client-after.log` | 业务进程重启后的有界重试读取 |
| `recovery/client-after-mediamtx.log` | MediaMTX重启后的有界重试读取 |
| `recovery/status-*.log` | 启动、业务恢复、MediaMTX恢复和停止状态 |
| `recovery/resource-summary.json` | 20秒资源摘要，含RSS/线程/fd |
| `recovery/resource-events.csv` | 资源摘要对应融合事件 |

## 结果

- 60秒：1795帧编码、454对热融合、1795/1795 RTSP包，0队列淘汰、0发布失败，
  端到端应用PASS；峰值VmPeak约640880 KB，结束RSS约35876 KB，线程2，fd33。
- 杀业务app：supervisor `restart_count=1`，新业务和RTSP发布恢复，客户端成功读取。
- 杀MediaMTX：应用安全报告Broken pipe并退出，supervisor `restart_count=2`，
  重建MediaMTX和业务，客户端恢复读取。
- stop：业务、MediaMTX、supervisor均退出，PID文件清理，LED brightness=0、PWM duty=0。

故障恢复中的首个短暂404/connection refused属于重启窗口，门禁使用有界重试并将
完整日志保留；最终恢复必须是客户端实际完成解码，而不是只看到进程存在。

P2.7.2尚需用户确认运行时长后执行2小时或更长整链长稳，并在结束后重新打开相机。
