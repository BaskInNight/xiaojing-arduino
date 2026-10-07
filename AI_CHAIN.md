# AI_CHAIN.md — AI 语音链路文档

## 1. 数据流全景

```
INMP441 (I2S RX)
    │
    ▼
Edge Impulse 本地唤醒词检测 ("xiao_jing" > 0.8)
    │
    ▼
VAD 录音（高通滤波 + 能量阈值 600，1.2s 静音截断）
    │
    ▼
WAV 编码 (16kHz/16bit/mono) → Base64 (~215KB)
    │
    ▼
HTTPS POST → MiMo mimo-v2.5 (Tool Calling)
    │
    ├─ speech_reply (文本) ──→ MiMo mimo-v2.5-tts ──→ Base64 WAV ──→ I2S TX → MAX98357
    │
    └─ pipeline[] ──→ WashStep_t ──→ EXE_PushStep() ──→ WashExecutor 状态机
```

## 2. 唤醒词检测（本地）

| 项目 | 值 |
|---|---|
| 框架 | Edge Impulse |
| 模型 | `lib/Washer-Voice-Control_inferencing/` |
| 标签 | `xiao_jing` |
| 阈值 | > 0.8 |
| 函数 | `run_classifier()` in `main.cpp` |
| 运行核 | Core 1（主循环中调用） |

## 3. VAD 录音

| 参数 | 值 |
|---|---|
| 采样率 | 16kHz |
| 位深 | 16bit |
| 声道 | Mono |
| 能量阈值 | > 600（放大后） |
| 静音截断 | 1.2s 连续低能量 |
| 空唤醒超时 | 2.5s 无有效语音 |
| 最大录音 | 5s |
| 去噪 | 高通滤波去底噪 |

## 4. 云端大模型

| 项目 | 值 |
|---|---|
| 模型（对话） | `mimo-v2.5` |
| 模型（TTS） | `mimo-v2.5-tts`（声音"冰糖"） |
| API 地址 | `https://token-plan-cn.xiaomimimo.com/v1/chat/completions` |
| TLS | `WiFiClientSecure` + `setInsecure()` |
| 调用方式 | 手动拼接 HTTPS POST（非 HTTPClient） |
| 运行核 | Core 0 独立 FreeRTOS 任务 |

### 上传数据

- 系统提示词：洗涤指令集、用户偏好、安全约束
- 用户语音：Base64 WAV 音频
- Tool 定义：`generate_wash_pipeline` JSON Schema

### 下发指令（Tool Calling）

```json
{
  "speech_reply": "好的，为你选择标准洗涤模式",
  "updated_preferences": {"mode": "standard"},
  "pipeline": [
    {"cmd": "WATER_IN", "val": 1, "time": 3},
    {"cmd": "WASH_PULSATOR", "val": 2, "time": 9},
    {"cmd": "WASH_DRUM", "val": 2, "time": 5},
    {"cmd": "SPIN_DRY", "val": 2, "time": 8}
  ]
}
```

pipeline 中的 cmd 映射到 `WashStep_t`，注入执行器队列。

## 5. TTS 播放

1. MiMo mimo-v2.5-tts 合成语音 → Base64 WAV
2. ESP32 解码 Base64 → I2S DMA 写入
3. MAX98357 功放驱动扬声器
4. 函数：`executeTTS()` in `main.cpp`

## 6. 本地提示音

**文件：`src/LocalAudio.h`**

| 常量 | 内容 | 触发时机 |
|---|---|---|
| `AUDIO_WAKE_UP` | "小净在听" | 唤醒词检测成功 |
| `AUDIO_HEAR_NOTHING` | 未检测到语音 | VAD 超时 |
| `AUDIO_THINKING` | "稍等我想一下" | 开始云端推理 |
| `AUDIO_TASK_DONE` | 完成提示 | 洗涤指令执行完毕 |
| `AUDIO_NET_ERROR` | 网络错误 | WiFi 断开 |
| `AUDIO_API_TIMEOUT` | API 超时 | 云端响应超时 |
| `AUDIO_JSON_ERROR` | JSON 解析错误 | Tool Calling 返回异常 |

## 7. BLE 与云端共存

云端推理期间 BLE 需要关闭（共享射频）：

1. `BLE_ShutdownForCloud()` — 关闭 NimBLE，发送 `A,s` 给小程序
2. 云端推理完成
3. `BLE_ResumeAfterCloud()` — 恢复 NimBLE，发送 `A,b`
4. 小程序 `expectedDisconnect` 机制避免误报断线

## 8. 关键函数索引

| 函数 | 文件 | 行号 | 说明 |
|---|---|---|---|
| `cloudNetworkTask()` | main.cpp | ~781 | 云端 AI 主任务 |
| `executeTTS()` | main.cpp | ~562 | TTS 合成+播放 |
| `BLE_ShutdownForCloud()` | main.cpp | — | BLE 暂停 |
| `BLE_ResumeAfterCloud()` | main.cpp | — | BLE 恢复 |

## 9. 变更记录

- 2026-07-15：初始创建，基于 9 节技术审计整理
