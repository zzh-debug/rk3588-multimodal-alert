# P2.3.1 RGA/RKNN真机基线证据

| 项目 | 值 |
|---|---|
| 应用提交 | `80346d92fc3ad4be337ff038025ca9200d7fd5fb` |
| 板端内核 | Linux 5.10.209 `#14` |
| 根文件系统 | NFS：`192.168.5.11:/home/zzh/workspace/myproject/nfs_rootfs/atk_dlrk3588` |
| RKNN API / 驱动 | `2.0.0b0 (35a6907d79@2024-03-24T10:31:14)` / `0.9.6` |
| RGA用户API启动输出 | `1.10.1_[1]` |
| `p2_person_detect` SHA-256 | `7be8dfbf213fefa25d4d5eda353f0ee14d5593893dc33a4db997f0733313d4b6` |
| `p2_person_detect_nv12` SHA-256 | `2d34b48402f4b25a050d7b1e8db62a5ed3664b6c450112a6f1de5ba566ab1992` |
| RKNN模型 SHA-256 | `7c6b801de602b8aaa72269fab8daab21810418a4b01c8e8995313916e278fe71` |

## 文件

| 文件 | 内容 | SHA-256 |
|---|---|---|
| `p23-bus.json` | SDK固定样本单次真实RGA/RKNN结果，检出4人 | `1501bbbe6ae3a0f0fef1114a6151cd654141662b55d7ce8f03c876e32c2672e8` |
| `p23-live-30s.json` | 4K实时30秒吞吐和延迟分位 | `8b87ee370df09c250df83fddd1f803f93b5a1af23804411776fd3328bdcd4882` |
| `p23-live-30s.csv` | 当前空场景检测明细，仅含表头 | `64e7a5caae0163ca47c233afd8e4e6bee798c3d5256bf8ac9450def98920278b` |
| `commands.log` | 构建、部署和复现命令 | 由当前提交直接审计 |

固定样本来源为SDK
`external/rknpu2/examples/rknn_yolov5_demo/model/bus.jpg`，JPEG SHA-256为
`fb4914d123d97c440cd127ef0e98d4bdc68cd88e2683657528928b4a34014e16`；
板端生成的临时NV12 SHA-256为
`12c5efd12a27b8d5c82531e488593977f0fffd449a20fa1175d191f9afb3a547`。
图像和NV12不提交，只保存来源与哈希。

结论边界：固定样本证明实际person输出链路；30秒结果证明暂定性能门槛。当前
没有真实场景标注集，因此没有Precision/Recall/mAP声明；同步推理产生的49次
sequence gap也保留为P2.4待治理事实。
