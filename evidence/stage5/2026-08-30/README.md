# P2.5 MPP H.264、OSD与RTSP板端证据（2026-08-30）

本目录保存精确提交`b7887575f10351a24c9922c96bc0d0a30de4f0f4`的精选证据。
原始H.264码流约45 MB，按仓库规则不提交；保存其SHA-256、完整`ffprobe`结果、
结构化摘要、事件、MediaMTX日志和不含真人的确定性OSD抽帧。正式运行中的真人
抽帧只做本地人工核查，不提交到Git仓库。

| 路径 | 内容 |
|---|---|
| `formal/summary.json` | 60秒融合、编码和发布结构化摘要 |
| `formal/events.csv` | 同次运行的逐对融合事件 |
| `formal/ffprobe.json` | 完整Annex-B码流解码与帧数 |
| `formal/stream-sha256.txt` | 未入库正式码流SHA-256 |
| `formal/mediamtx.log` | 发布及两次客户端重连日志 |
| `formal/client-1.log`,`client-2.log` | 两个客户端窗口；0字节表示无解码错误 |
| `formal/demo.jpg` | 确定性MPP红框/温度文字门禁 |
| `formal/demo-summary.json`,`demo-ffprobe.json` | 84帧确定性OSD码流摘要 |
| `restart/summary-1.json`至`summary-5.json` | 仅RTSP模式5轮应用重启摘要 |
| `restart/mediamtx.log` | 5轮publisher/reader建立和释放 |
| `environment.txt` | 板端系统、库、模型、标定和二进制哈希 |
| `commands.txt` | 复现命令 |

正式60秒得到1796个DQBUF、1795个推理/编码帧、1795个RTSP发布包和464对融合。
采集、推理、温度、融合错误均为0；可见光/编码/fusion队列均无淘汰；MPP源端只
导入6个采集DMA-BUF。H.264为High Profile、1080x1920、yuv420p、30 fps，
`ffprobe`完整读取1795帧。RGA和MPP平均耗时分别为10.728 ms和5.175 ms。

正式运行包含928个人员实例、542个热匹配和2次告警激活。状态拼图可见绿色普通
人员框、黄色热匹配框、红色告警框及匹配温度；确定性demo只验证OSD渲染，不作为
检测或温度准确率证据。两次客户端重连均无解码错误，修复后的仅RTSP部署模式
5/5轮启动通过，每轮234帧编码发布、0淘汰、0发布失败。

这些结果不改变跨光谱临时标定边界：温度只作为相对热证据，1 m配置只支持粗粒度
ROI；无参考温度计时不声明绝对测温精度，无正式映射门限时不声明像素级对齐。
