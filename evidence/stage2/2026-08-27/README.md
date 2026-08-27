# P2.2.1 温度数学基线证据

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
```

## 事实边界

- 结果证明真实数据可解析、全部温度输出有限、坏点修复有效且实时管线连续；
- 没有参考温度计或黑体，不能据此声明绝对测温误差；
- 精确FOV、固定支架和跨光谱靶尚未确认，本轮不声明区域映射精度；
- 时域滤波和伪彩仍属于P2.2后续工作。
