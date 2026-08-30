# P2.4.3 人员框×热ROI融合证据

本目录保存实现提交`d9507a8f5572`和修复提交`2b1a59cb6dc3`的板端精选证据。

| 文件 | 内容 |
|---|---|
| `fusion-60s.json` | 干净提交版本60秒正式摘要 |
| `fusion-60s.csv` | 同次运行467个逐对AlertEvent记录 |
| `extended-120s.json` | 干净提交版本延长观察：925/925对，3次激活、2次释放 |
| `thermal-suppressed.json/.csv` | 将绝对热阈值抬至45℃、保留YOLO路径的反事实对照 |
| `pre-fix-nonfinite-failure.json` | 修复前第82个热pair出现767/768有限温度并退出 |
| `post-fix-repair-smoke.json` | 临时诊断构建30秒预跑，实际修复1个瞬态温度像素 |
| `restart-summary.json` | 5/5轮完整进程启停摘要 |
| `restart-round-01/05.json` | 启停首尾两轮结构化结果 |
| `environment.txt` | 提交、板端、模型、标定、EEPROM和二进制哈希 |
| `commands.txt` | 可复验命令 |

正式60秒结果：1795个可见光DQBUF、468个有效热pair、467/467个融合时间对；
采集、队列和Join均0 gap/0淘汰/0缺失结果。时间配对绝对偏差P50/P95/最大值为
8.202/15.764/16.622 ms，融合计算P50/P95为0.092/0.422 ms。

同一干净提交继续进行120秒延长观察，3595个可见光DQBUF、926个有效热pair和
925/925个融合时间对仍保持0采集/队列/配对错误；实际经历3次激活和2次释放，
验证板端状态迁移路径可以闭环。该轮未再出现瞬态非有限像素。

45℃对照仍处理113个带人员框的融合帧，但热连通域、人员×热区匹配和告警激活均
为0，证明AlertEvent不是由YOLO单模态直接触发。

`post-fix-repair-smoke.json`来自提交修复前的临时诊断构建，文件中的
`application_commit`只反映当时最近提交，不能单独复现工作树；该文件只证明修复
分支实际处理过1个瞬态像素。可复现的最终源码是`2b1a59cb6dc3`，正式二进制哈希见
`environment.txt`，修复纯函数由原生CTest覆盖。
