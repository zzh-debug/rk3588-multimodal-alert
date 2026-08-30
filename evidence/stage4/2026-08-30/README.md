# P2.4.1 采集/推理解耦板端证据（2026-08-30）

本目录保存实现提交 `4ad8be6a9fb9178692de61120827e8c7a8b12002` 的精选证据。

| 文件 | 内容 |
|---|---|
| `async-60s.json` | 默认容量 1 的 60 秒正式结构化摘要 |
| `async-60s.log` | 同次运行的完整标准输出 |
| `queue-capacity-1-15s.json` | 容量 1 的 15 秒延迟对照 |
| `queue-capacity-3-15s.json` | 容量 3 的 15 秒延迟对照 |
| `environment.txt` | 内核、NFS 根、程序和模型哈希 |
| `dmabuf-source-60s.json/.log` | DMA-BUF源路径60秒正式门禁 |
| `dmabuf-source-equivalence.json/.log` | 同帧DMA-BUF/MMAP结果一致性 |
| `dmabuf-ab-15s.json` | 同代码DMA-BUF 15秒A/B |
| `mmap-ab-15s.json` | 同代码MMAP 15秒A/B |
| `dmabuf-environment.txt` | P2.4.2a提交、命令和二进制哈希 |

正式门禁得到 1796 个 DQBUF、29.899 FPS，采集侧 0 sequence gap、0 坏 payload、
0 缺失单调时间戳。推理略慢时队列主动淘汰 120 帧，`processed gap frames` 同为
120；退出后队列 pending 为 0。

P2.4.1首轮交接是 V4L2 MMAP Buffer 租约，不是 DMA-BUF。该批日志中的
`application_full_frame_cpu_copies=0` 只表示应用在 4K 采集帧交给推理线程时没有
执行全帧 CPU memcpy。

后续提交 `1ed71fd650db` 已把RGA源切到V4L2 EXPBUF/importbuffer_fd：正式60秒
6/6 Buffer导出、RGA仅import 6次，1796 DQBUF、0采集gap、0队列淘汰，推理
30.019 FPS，端到端P50/P95为29.957/36.131 ms。5帧同帧双路径对照0不一致。
当前640×640 RGB到RKNN仍使用`rknn_inputs_set`，所以依然不称端到端零拷贝。
