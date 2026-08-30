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
| `rknn-inputs-set-15s.json` | 同提交传统RKNN输入A/B |
| `rknn-iomem-15s.json` | 同提交RKNN io_mem输入A/B |
| `rknn-iomem-60s.json/.log` | io_mem默认路径60秒正式门禁 |
| `rknn-iomem-equivalence.json/.log` | 新旧完整输入路径同帧一致性 |
| `restart-round-01/20.json` | 20轮短启停首尾摘要 |
| `restart-interrupted.log` | SIGTERM中断运行日志 |
| `restart-reopen.json` | 中断后重新打开相机摘要 |
| `rknn-iomem-environment.txt` | P2.4.2b提交、命令和二进制哈希 |

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

提交`19fdb295f857`进一步使用RKNN分配的non-cacheable `io_mem` fd作为RGA目标。
60秒1796 DQBUF、0采集gap、0队列淘汰、30.012推理FPS，端到端P50/P95为
30.302/41.689 ms；新旧输入路径5/5同帧一致。20轮短启停和SIGTERM后重开均通过。
输出仍由`rknn_outputs_get`交给CPU后处理，因此不宣称端到端零拷贝。

P2.4.3提交`d9507a8f5572`接入双路结果级时间Join、人员框×粗粒度热ROI融合和
AlertEvent去抖；提交`2b1a59cb6dc3`补充最多4个瞬态非有限热像素的有界修复。
正式60秒467/467对完成融合，采集/队列/配对均0 gap，配对偏差P95 15.764 ms，
融合P95 0.422 ms。45℃反事实对照保留113个人员框但热区、匹配和激活均为0，
完整进程启停5/5通过。证据位于`fusion/`，P2.4至此关闭。
