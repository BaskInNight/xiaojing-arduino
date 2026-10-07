# BLE_PROTOCOL.md — 蓝牙通信协议文档

## 1. 协议栈

| 项目 | 值 |
|---|---|
| 库 | NimBLE-Arduino 1.4.0 |
| 替代 | 禁用 Bluedroid（`lib_ignore = ESP32 BLE Arduino`） |
| 设备名 | `JJTP-XIAOJING` |
| 运行核 | Core 1 |

**禁止引入 `BLEDevice.h`，只能使用 NimBLE API。**

## 2. 服务与特征

| 项目 | UUID |
|---|---|
| Service | `0000FFF0-0000-1000-8000-00805F9B34FB` |
| Write (手机→ESP32) | `0000FFF1-...` (Write) |
| Notify (ESP32→手机) | `0000FFF2-...` (Notify) |

## 3. ESP32 → 手机（Notify 数据）

### AI 状态码（前缀 `A,`）

| 码 | 含义 | 触发时机 |
|---|---|---|
| `A,w` | wake_detected | 唤醒词检测成功 |
| `A,r` | recording | VAD 开始录音 |
| `A,d` | record_done | 录音完成，开始上传 |
| `A,s` | ble_suspending | BLE 即将关闭（云端推理） |
| `A,b` | ble_resume_done | BLE 恢复 |
| `A,i` | idle | 空闲 |
| `A,f` | cloud_failed | 云端调用失败 |

### 进度码（前缀 `P,`）

| 格式 | 示例 | 说明 |
|---|---|---|
| `P,xx` | `P,45` | 洗涤进度百分比 (0-100) |

### 状态码（前缀 `S,`）

| 格式 | 说明 |
|---|---|
| `S,code` | 设备状态码（数值） |

### JSON fallback

ESP32 也可能发送完整 JSON（旧协议兼容），小程序优先解析 `A,`/`P,`/`S,` 前缀。

## 4. 手机 → ESP32（Write 数据）

发送 JSON 指令，通过 20 字节分包传输。

### 开始洗涤

```json
{"cmd": "start", "steps": {"soak": 5, "wash": 10, "rinse": 3, "spin": 5}}
```

### 停止洗涤

```json
{"cmd": "stop"}
```

### 自定义模式

```json
{"cmd": "start", "steps": {...}, "mode": "custom"}
```

## 5. 分包传输协议

BLE 单包 MTU 限制 20 字节，长 JSON 需分包：

1. 计算 JSON 字节数
2. 每 20 字节切一包
3. 包间间隔 50ms
4. 最后一包正常发送（不足 20 字节也发）
5. ESP32 端拼接所有分包，收到完整 JSON 后解析

**函数：** `BLEDriver.cpp` 中的 `sendCommand()` / 数据接收回调

## 6. AI 预期断开生命周期

```
ESP32 发送 A,s (ble_suspending)
    │
    ▼
小程序设置 expectedDisconnect = true
    │
    ▼
BLE 实际断开
    │
    ▼
小程序检测到断开 → expectedDisconnect=true → 不显示"蓝牙断线"
    │
    ▼
小程序启动自动重连 (startAutoReconnect, 2s间隔, 最多30次)
    │
    ▼
ESP32 发送 A,b (ble_resume_done)
    │
    ▼
小程序停止重连，expectedDisconnect=false，aiProcessing=false
```

**关键：** 断开时**不清空** serviceId / writeCharId / notifyCharId，重连后直接复用。

## 7. 普通断线（非 AI 预期）

手机主动断开或设备掉电：

1. `expectedDisconnect = false`
2. 清空 serviceId / writeCharId / notifyCharId
3. toast 提示"蓝牙已断开，请重新连接"
4. 不启动 AI 自动重连

## 8. 小程序 BLE 模块

**文件：`miniapp4.0/miniprogram/utils/ble.js`**

| 功能 | 说明 |
|---|---|
| `openBLEAdapter()` | 初始化蓝牙适配器 |
| `startBLEScan()` | 扫描 `JJTP` 前缀设备 |
| `connectBLE(deviceId)` | 连接设备，获取服务 |
| `sendCommand(json)` | JSON → 20 字节分包发送 |
| `onBLENotify(callback)` | 注册 notify 监听 |
| `closeBLE()` | 断开并清理 |

## 9. 变更记录

- 2026-07-15：初始创建，基于 BLEDriver.cpp 和 miniapp 源码整理
