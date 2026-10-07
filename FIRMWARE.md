# FIRMWARE.md — 固件架构文档

> 修改固件前，先阅读本文件和 CLAUDE.md。

## 1. FreeRTOS 任务分配

| Core | 任务 | 函数/文件 | 优先级 |
|---|---|---|---|
| Core 1 | Arduino 主循环 | `loop()` in `main.cpp` | 正常 |
| Core 1 | BLE 服务 | NimBLE stack | 正常 |
| Core 1 | LVGL 显示刷新 | `DisplayDriver.cpp` | 正常 |
| Core 0 | 云端 AI（语音→LLM→TTS） | `cloudNetworkTask()` in `main.cpp` | 高 |
| Core 0 | Web 控制面板 | `WebInterface.cpp` | **已排除编译** |

**通信机制：** FreeRTOS 队列（`WashStep_t`）、信号量（`xSemaphoreGive/Take`）、`volatile bool` 标志位。

## 2. 洗涤状态机（WashExecutor）

**文件：`src/WashExecutor.cpp`**

队列驱动架构：`std::queue<WashStep_t>` + FreeRTOS mutex 保护，跨核安全。

### 指令集

| cmd_id | 功能 | val | time |
|---|---|---|---|
| `CMD_WATER_IN` (0x01) | 进水 + UV | 0=关UV / 1=开UV | 最大等待秒数 |
| `CMD_WASH_PULSATOR` (0x02) | 波轮洗 | 1轻柔/2标准/3强力 | 秒 |
| `CMD_TILT_BARREL` (0x03) | 姿态切换 | 0/90/180 | 必须填 0 |
| `CMD_WASH_DRUM` (0x04) | 滚筒洗 | 1轻柔/2标准/3强力 | 秒 |
| `CMD_SPIN_DRY` (0x05) | 脱水 | 1沥水/2高速 | 秒 |
| `CMD_HOME_BLDC` (0x06) | BLDC 归位 | 0 | 0 |
| `CMD_TURBIDITY_CHECK` (0x07) | 浊度检测 | 0单次/1持续 | 0 |
| `CMD_UV_STERILIZE` (0x08) | UV 杀菌 | 0关/1开 | 秒 |

### 安全注入（三阶段）

| 阶段 | 触发时机 | 注入指令 |
|---|---|---|
| Phase 1 安全屏障 | 队列起始 | `TILT_BARREL(0)` + `HOME_BLDC` |
| Phase 2 自动衔接 | 滚筒洗前/脱水前 | `TILT_BARREL(90)` / `TILT_BARREL(180)` |
| Phase 3 安全收尾 | 队列末尾 | `TURBIDITY_CHECK` + `TILT_BARREL(0)` |

**AI 生成的 `CMD_TILT_BARREL` 和 `CMD_HOME_BLDC` 被显式忽略。**

## 3. BLDC Softstart 换向（6 阶段）

**9000ms 周期：**

| 阶段 | 时长 | 动作 |
|---|---|---|
| CW_SOFT | 700ms | 正转软启动 (25%) |
| CW_RUN | 2500ms | 正转全速 |
| PREP_CCW | 1000ms | 制动 + 预设反转方向 |
| CCW_SOFT | 700ms | 反转软启动 (25%) |
| CCW_RUN | 2500ms | 反转全速 |
| PREP_CW | 1000ms | 制动 + 预设正转方向 |

**速度映射：** val=1→35%, val=2→50%, val=3→65%

**关键函数：** `BLDCMotor::brakeThenCW()` / `brakeThenCCW()` — 在制动期间预设方向信号。

## 4. 步进电机控制

**不用 `moveTo`**，改用连续运行 + IPS 硬停：

1. `setSpeedInHz(4000)` + `setAcceleration(2500)`
2. `runForward()` / `runBackward()` 启动连续运行
3. IPS 接近开关触发 → `forceStop()` + `setCurrentPosition(0)`
4. 磁性滞回去抖（3ms 默认，180° 用 20ms）

## 5. 中断设计

| 中断源 | GPIO | 类型 | 处理 |
|---|---|---|---|
| PAJ7620 手势 | GPIO 8 | FALLING | ISR 设置标志，主循环轮询处理 |
| EC11 编码器 A | GPIO 18 | CHANGE | ESP32Encoder 库，磁性滞回 |
| EC11 编码器 B | GPIO 13 | CHANGE | 同上 |
| EC11 按键 | MCP GPB0 | 轮询 | 主循环检测下降沿 |

## 6. PWM 资源分配

| 通道 | 频率 | 分辨率 | 用途 |
|---|---|---|---|
| LEDC ch0 | 20kHz | 8-bit | BLDC VR 调速 (GPIO 39) |

## 7. 内存规划

| 区域 | 用途 |
|---|---|
| 内部 SRAM | FreeRTOS 任务栈、全局变量、BLE 协议栈 |
| PSRAM (8MB) | 音频缓冲区（WAV 录音 ~215KB）、Edge Impulse 推理 |
| NVS Flash | 用户偏好（AI 更新的洗涤习惯） |
| Flash | 固件、Edge Impulse 模型、本地提示音 Base64 |

## 8. 源文件职责

| 文件 | 行数 | 职责 |
|---|---|---|
| `src/main.cpp` | ~2636 | 语音中枢、云端 AI、BLE 命令、OTA、主循环 |
| `src/WashExecutor.cpp` | ~507 | 洗涤状态机、softstart、安全注入 |
| `src/HardwareDriver.cpp` | ~364 | BLDC/步进/传感器/MCP23017 驱动 |
| `src/BLEDriver.cpp` | ~294 | NimBLE 蓝牙服务、多包 JSON 拼接 |
| `src/DisplayDriver.cpp` | ~548 | ILI9341 + LVGL 三页面 UI |
| `src/WebInterface.cpp` | ~234 | HTTP 控制面板（**已排除编译**） |
| `src/LocalAudio.h` | — | 5 段本地提示音 Base64 数据 |
| `include/WashProtocol.h` | — | 指令枚举、WashStep_t、安全阈值 |
| `include/HardwareConfig.h` | — | GPIO 定义、传感器参数 |
| `include/Config.h` | — | WiFi/API 密钥（**不上传公共仓库**） |

## 9. 变更记录

- 2026-07-15：初始创建，基于源码和 CLAUDE.md 整理
