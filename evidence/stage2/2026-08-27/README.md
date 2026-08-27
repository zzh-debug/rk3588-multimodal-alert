# P2.2 温度数学与处理基线证据

## 版本和输入

| 项目 | 值 |
|---|---|
| 应用提交 | `89dc9268e588` |
| 项目一基线 | `c140532b82723bdfe1396b51304f2dfe9c4c73e7` |
| 内核 | Linux 5.10.209 `#14`，AArch64 |
| 根文件系统 | NFS root |
| EEPROM | 1664 bytes，SHA-256 `cc4628409b57ca3137065b10fd40c684fcef9d16ab493ace882710ad490185b2` |
| ZMLX黄金帧 | 3400 bytes，SHA-256 `1027c7e0fc754663e7897379c0842cfbb912fe711fefb6285ebeb6c7507ab3e0` |
| 实时探针二进制 | SHA-256 `fe869a5d11830166b31c5beb15890f2be33463d3adbbf9a7e3228b8635ca9f85` |
| P2.1回归二进制 | SHA-256 `b06eaa970e0011ab54b7146d467edcd86eb4b5230182745f3af33b8b5709d9f6` |
| P2.2.2应用提交 | `22e9e07b36517fe9ebe2b6c41bbf1b16ebd83665` |
| P2.2.2实时探针二进制 | SHA-256 `a3d5d6ad40d3eb86b86df65271e7df8c95abd42d08b8a7aa5c00b5ef6cc9ef56` |
| P2.2.2报告 | SHA-256 `14eb403a4cae6f31bab3312d6ae099f26ee8a51e29c995c27425034525e43762` |
| P2.2.3应用提交 | `3501809f3003e5955a32a3a50139456e0ef8fc6d` |
| AArch64映射探针 | SHA-256 `ab4fe6faf006a2b559bb6e7bf3c341e0c26c918343f37e1be27c2fd7c1d6de8d` |
| AArch64拟合工具 | SHA-256 `7f609b0044dae47d314118d9f3ed63484b3cf08ae8b46aa6e75b5b900bcb31c9` |
| Melexis数据手册 | Revision 11，SHA-256 `378f5a67b246dbce788facbf5858b3187dea13f789f1ff6361d2ae04ce6a35af` |

`raw/`中的EEPROM与ZMLX二进制属于设备私有黄金输入，被Git忽略。报告和本文只记录
哈希；公开仓库前仍需复核是否适合发布设备校准数据。

## 报告

| 文件 | 内容 |
|---|---|
| `offline-golden.json` | 默认参数、坏点修复开启的离线黄金结果 |
| `offline-no-bad-pixel-correction.json` | 关闭坏点修复的诊断对比 |
| `offline-emissivity-1.json` | 发射率1.0参数敏感性对比 |
| `offline-reflected-offset-0.json` | 反射温差0℃参数敏感性对比 |
| `realtime-600.json` | 精确绑定`89dc9268e588`的600对板端连续性报告 |
| `p21-regression-summary.json` | `ThermalCapture`扩展后的P2.1双路采集回归 |
| `realtime-processing-600.json` | 精确绑定`22e9e07b3651`的温度数学、时域滤波和伪彩600对板端连续性报告 |
| `cross-spectral-synthetic-aarch64.json` | 精确绑定`3501809f3003`的AArch64合成区域映射正向烟测，不是物理标定结果 |
| `cross-spectral-uncalibrated-rejected.json` | 未标定/未确认型号模板在板端被拒绝的反向门禁 |
| `cross-spectral-synthetic-aarch64.conf` | 板端由6组合成点生成的配置，仅验证拟合/序列化路径 |

## 再生成命令

```bash
# 开发板：导出真实输入
dd if=/sys/bus/nvmem/devices/zzh_mlx90640_eeprom-5-33/nvmem \
  of=/tmp/mlx90640-eeprom-be.bin bs=1664 count=1
p2_thermal_export --output /tmp/zmlx-golden-pair.bin
sha256sum /tmp/mlx90640-eeprom-be.bin /tmp/zmlx-golden-pair.bin

# 容器：离线构建和黄金测试
./scripts/build_native.sh
./build/native/p2_thermal_math_probe \
  --eeprom evidence/stage2/2026-08-27/raw/mlx90640-eeprom-be.bin \
  --zmlx evidence/stage2/2026-08-27/raw/zmlx-golden-pair.bin

# 开发板：600对实时门禁
p2_thermal_realtime_probe --pairs 600 \
  --output /tmp/p2-thermal-realtime-600.json

# 开发板：P2.1回归
p2_capture_sync --duration 15 --output /tmp/p2-stage2-p21-regression

# 开发板：P2.2.2温度处理链600对门禁
p2_thermal_realtime_probe --pairs 600 \
  --filter-tau-ms 250 --filter-reset-gap-ms 1000 \
  --color-min-c 20 --color-max-c 45 \
  --output /tmp/p2-thermal-processing-600.json

# 开发板：仅用于复现AArch64软件路径，不得用作真实标定
p2_calibration_fit \
  --points cross_spectral_points_synthetic.csv \
  --output /tmp/p2-board-synthetic.conf \
  --calibration-id synthetic-aarch64-smoke --model BAA \
  --visible-width 3840 --visible-height 2160 \
  --nominal-distance-mm 1500 --min-distance-mm 1000 \
  --max-distance-mm 2000 --max-error-px 0.001
p2_cross_spectral_probe \
  --config /tmp/p2-board-synthetic.conf --thermal-region 2,4,3,5 \
  --visible-roi 240,377.5,360,547.5 --min-overlap 0.49
```

## 事实边界

- 结果证明真实数据可解析、全部温度输出有限、坏点修复有效且实时管线连续；
- 没有参考温度计或黑体，不能据此声明绝对测温误差；
- 精确FOV、固定支架和跨光谱靶尚未确认，本轮不声明区域映射精度；
- 时域滤波只证明有界状态、时间语义、平滑效果和处理开销；默认参数仍需按最终场景权衡噪声与响应延迟；
- 伪彩使用固定温区保证跨帧可比，不等于完成显示端缩放、OSD或跨光谱映射。
- P2.2.3提交的映射证据全部为合成几何，只证明软件与AArch64执行路径；在真实型号、支架、靶和独立验证点缺失时不得声明区域映射精度。
