# HARDWARE.md — 硬件设计文档

> 修改任何硬件接口前，必须先阅读本文件。

## 1. ESP32-S3 主控

| 项目 | 值 |
|---|---|
| 模组 | ESP32-S3-WROOM-1 N16R8 |
| Flash | 16MB（QIO 80MHz） |
| PSRAM | 8MB（OPI 80MHz） |
| CPU | 双核 Xtensa LX7，240MHz |
| 分区表 | `default_16MB.csv` |

## 2. GPIO 分配总表

**定义文件：`include/HardwareConfig.h`**

| 类别 | 信号 | GPIO | 说明 |
|---|---|---|---|
| **I2C 总线** | SDA | 6 | PAJ7620 + MCP23017 共线 (100kHz) |
| | SCL | 7 | |
| **步进电机** | PUL | 41 | FastAccelStepper 脉冲 |
| | DIR | 40 | 方向 |
| | ENA | 48 | 使能（LOW=启用） |
| **BLDC 电机** | VR (PWM) | 39 | LEDC ch0, 20kHz, 8-bit |
| | ZF (DIR) | 42 | HIGH=正转, LOW=反转 |
| **IPS 接近开关** | IPS 0° | 1 | LOW=触发，竖直位置 |
| | IPS 90° | 2 | LOW=触发，水平位置 |
| | IPS 180° | 47 | LOW=触发，倒立位置 |
| | IPS BLDC | 21 | LOW=触发，内筒归位 |
| **传感器** | 水位 | 4 | XKC-Y28 电容式，HIGH=满 |
| | 浊度 ADC | 5 | TS300B，12bit, 11dB 衰减 |
| **交互** | PAJ INT | 8 | PAJ7620 手势中断 |
| | EC11 A | 18 | 编码器 A 相 |
| | EC11 B | 13 | 编码器 B 相 |
| **TFT 显示** | CS | 10 | HSPI |
| | DC | 9 | |
| | RST | 14 | |
| | MOSI | 11 | |
| | SCK | 12 | |
| **音频 I2S** | WS | 15 | I2S_NUM_0 共享（TX+RX） |
| | SCK | 16 | |
| | SD | 17 | 麦克风数据输入 |
| | AMP DIN | 19 | MAX98357 数据输出 |

## 3. MCP23017 I2C 扩展（高压负载）

**地址：0x20**

| 引脚 | 功能 | 驱动函数 |
|---|---|---|
| GPA0 | 进水阀（继电器） | `DRV_WaterValve(bool)` |
| GPA1 | 未分配（预留） | — |
| GPA2 | UV 杀菌灯（继电器） | `DRV_UVLamp(bool)` |
| GPA3 | 未分配（预留） | — |
| GPA4-7 | 未分配（预留） | — |
| GPB0 | EC11 按键输入 | INPUT 模式 |
| GPB1-6 | 未分配（预留） | — |
| GPB7 | 系统心跳 LED | 约 500ms 闪烁 |

**可扩展容量：** GPA1, GPA4-7, GPB1-6 共 11 个引脚可用于未来外设。

## 4. 电机子系统

### BLDC 无刷电机
- **驱动**：PWM (LEDC ch0) + 方向 GPIO
- **PWM**：20kHz, 8-bit (0-255)
- **速度映射**：轻柔 35%，标准 50%，强力 65%（波轮洗）；脱水 40%/85%
- **换向**：6 阶段 softstart（9000ms 周期）：CW_SOFT → CW_RUN → PREP_CCW → CCW_SOFT → CCW_RUN → PREP_CW
- **关键**：使用 `brakeThenCW()`/`brakeThenCCW()` 在制动期间预设方向，ESC 需要此信号

### 步进电机
- **驱动**：FastAccelStepper 库
- **定位方式**：连续运行 + IPS 接近开关硬停（不用 `moveTo`）
- **速度**：4000 Hz，加速度 2500
- **位置**：0°（竖直）/ 90°（水平）/ 180°（倒立）

## 5. 传感器子系统

| 传感器 | 原理 | 接口 | 关键参数 |
|---|---|---|---|
| IPS 接近开关×4 | 电感式 | 数字 GPIO (INPUT_PULLUP) | LOW=触发，去抖 3ms（180° 用 20ms） |
| 浊度 TS300B | 光学 | ADC (GPIO 5) | 12bit, 11dB, 10 次均值，K=3291.3 |
| 水位 XKC-Y28 | 电容式 | 数字 GPIO (GPIO 4) | HIGH=满 |
| PAJ7620 | 手势 | I2C (0x73) + INT (GPIO 8) | ISR 中断触发 |
| EC11 编码器 | 旋转 | GPIO 中断 (A=18, B=13) | 磁性滞回算法防抖 |

## 6. 音频子系统

| 项目 | 值 |
|---|---|
| 麦克风 | INMP441 (I2S) |
| 功放 | MAX98357 (I2S) |
| 总线 | I2S_NUM_0，TX+RX 共享 |
| 采样率 | 16kHz / 16bit / mono |
| 特殊 | 云端推理期间 I2S 切换为 TX-only 播放 TTS |

## 7. 显示子系统

| 项目 | 值 |
|---|---|
| 屏幕 | ILI9341 240×320 TFT |
| 接口 | HSPI (自定义引脚) |
| 驱动 | TFT_eSPI + LVGL |
| 页面 | 3 页：状态主页 / 洗涤进度 / 语音助手 |

## 8. 尚未实现的硬件（无代码）

以下器件在设计文档或旧报告中可能存在描述，但当前代码库中**无任何驱动代码**：

| 器件 | 状态 |
|---|---|
| 排水阀/排水泵 | 无 GPIO 定义 |
| 加热器 | 无 GPIO 定义 |
| 风机/烘干 | 无代码 |
| 蠕动泵（洗涤剂自动投放） | 无代码 |
| 流量计 | 无代码 |
| PCB | 手工搭线样机，无 PCB 设计文件 |

## 9. 电源架构

当前为开发板直接供电 + 外部继电器板驱动高压负载。无独立电源管理 IC 或电池。

## 10. 变更记录

- 2026-07-15：初始创建，基于 HardwareConfig.h 和源码整理
