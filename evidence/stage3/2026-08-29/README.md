# P2.3.2 真实人员小样本验收证据

> 日期：2026-08-29（Asia/Shanghai）<br>
> 结论：PASS<br>
> 范围：当前ATK-DLRK3588、当前IMX415安装、当前实验室、单人正常照明小样本

## 1. 验收输入

| 项目 | real-01 | real-02 |
|---|---:|---:|
| 图片数 | 80 | 45 |
| 人员正样本 | 20 | 6 |
| 无人负样本 | 60 | 39 |
| 人员实例 | 20 | 6 |
| 采集方向 | `270cw` | `270cw` |
| 图片尺寸 | 540×960 BMP | 540×960 BMP |

两轮合计125张、26个人员实例和99张无人负样本。人工使用浏览器标注器逐张确认
模型建议框；只有手臂而无可识别头肩/躯干的画面不标为person。原始人物图片、
manifest、预测和人工真值保存在私有数据目录，不提交Git。

## 2. 冻结门槛与结果

评估使用person置信度0.25、IoU 0.50，预先冻结Precision和Recall均不低于0.80。

| 指标 | real-01 | real-02 | 汇总 |
|---|---:|---:|---:|
| TP | 20 | 6 | 26 |
| FP | 0 | 0 | 0 |
| FN | 0 | 0 | 0 |
| Precision | 1.000 | 1.000 | 1.000 |
| Recall | 1.000 | 1.000 | 1.000 |
| F1 | 1.000 | 1.000 | 1.000 |

两轮均PASS。采集期间共125张抽样图，V4L2 sequence gap、坏bytesused和缺失单调
时间戳均为0。四方向预烟测确认当前安装`270cw`正立，项目旧姿态的`90cw`不再
作为默认方向。

## 3. 可审计文件

| 文件 | 内容 |
|---|---|
| `real-01-capture-summary.json` | 80张采集运行摘要 |
| `real-01-evaluation.json` | 第一轮评估结果 |
| `real-02-capture-summary.json` | 45张补充采集运行摘要 |
| `real-02-evaluation.json` | 第二轮评估结果 |
| `person-evaluation-summary.json` | 两轮合并计数和事实边界 |
| `person-dataset-hashes.sha256` | 私有元数据、图片树和板端二进制哈希 |

板端采集二进制记录的`application_commit`为`e96ade04661d-dirty`：采集发生在实现
提交前的同一工作树。证据同时冻结板端二进制SHA-256，不能把该字段改写成后续
提交。原生9项测试和AArch64交叉编译在同一实现工作树通过。

## 4. 复现命令

```bash
p2_person_evaluate \
  --manifest manifest.csv \
  --annotations annotations.csv \
  --predictions predictions.csv \
  --confidence 0.25 \
  --iou 0.50 \
  --min-precision 0.80 \
  --min-recall 0.80 \
  --json evaluation.json \
  --failures failures.csv
```

## 5. 结论边界

该结果允许关闭P2.3的“当前设备小型真实人员标注集工程验收”，不等于COCO mAP、
跨人员/跨场景泛化或低照度精度结论。样本只有一名人员、同一实验室且主要为正常
照明；多人、不同服装、远距离小目标和系统性低照度评估留作后续扩展，不阻塞
P2.4采集与推理解耦。
