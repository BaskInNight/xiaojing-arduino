# 小净智能洗衣机 (XIAOJING)

> 全国大学生物联网设计竞赛 — 乐鑫赛道参赛作品

## 项目目标

设计并实现一款 AI 语音控制、BLDC+步进双电机驱动的智能洗衣机，支持自然语言交互、自动洗涤方案规划、BLE 远程控制。

## 硬件平台

| 项目 | 型号/规格 |
|---|---|
| 主控 | ESP32-S3-WROOM-1 N16R8（16MB Flash, 8MB PSRAM） |
| 电机 | BLDC 无刷电机（PWM 调速）+ 步进电机（FastAccelStepper） |
| 传感器 | IPS 接近开关×4、浊度传感器、电容水位、PAJ7620 手势、EC11 编码器 |
| 音频 | INMP441 麦克风 + MAX98357 功放（I2S 共享总线） |
| 显示 | ILI9341 240×320 TFT（HSPI + LVGL） |
| 通信 | NimBLE 蓝牙 + Wi-Fi（云端 AI） |
| 扩展 | MCP23017 I2C GPIO 扩展器（驱动高压继电器） |

## 软件架构

- **构建工具**：PlatformIO + Arduino 框架
- **操作系统**：FreeRTOS 双核（Core 0：云端 AI；Core 1：主循环+BLE+显示）
- **语音链路**：Edge Impulse 本地唤醒 → 云端 MiMo 大模型（ASR+TTS）→ Tool Calling 生成洗涤指令
- **小程序**：微信小程序 BLE 控制端（当前版本 `miniapp4.0/`）

## 核心功能

- 语音唤醒"小净"，自然语言描述洗涤需求
- AI 自动生成多步洗涤方案（进水→波轮洗→换筒→滚筒洗→脱水→浊度检测）
- 6 阶段 BLDC 软启停换向（9s 周期，避免 ESC 重启）
- 步进电机 IPS 精确定位（0°/90°/180°）
- PAJ7620 手势启动/停止
- ILI9341 三页面实时显示（状态/进度/语音助手）
- 微信小程序 BLE 远程控制 + GPS 定位

## 快速开始

```bash
pio run                          # 编译
pio run -t upload                # 编译 + 烧录
pio run -t upload && pio device monitor  # 烧录 + 串口监控
pio run -t clean                 # 清理构建缓存
```

设备在线时 `pio run -t upload` 自动走 ArduinoOTA（hostname: `xiaojing`）。

## 目录结构

```
src/                    # ESP32 固件源码
include/                # 配置头文件（引脚、协议、WiFi密钥）
lib/                    # 第三方库（Edge Impulse、PAJ7620、ESC驱动）
miniapp4.0/             # 微信小程序（当前版本）
doc/                    # 硬件资料、接线图、数据手册
test/                   # 测试目录
platformio.ini          # PlatformIO 构建配置
CLAUDE.md               # Claude Code 工程指引
```

## 当前阶段

Demo 可用，备战竞赛。已完成实机验证，正在整理比赛文档。
