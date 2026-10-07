#ifndef BLE_DRIVER_H
#define BLE_DRIVER_H

#include <Arduino.h>

// BLE 服务UUID（与小程序完全一致的128位格式）
#define BLE_SERVICE_UUID        "0000FFF0-0000-1000-8000-00805F9B34FB"
#define BLE_WRITE_CHAR_UUID     "0000FFF1-0000-1000-8000-00805F9B34FB"
#define BLE_NOTIFY_CHAR_UUID    "0000FFF2-0000-1000-8000-00805F9B34FB"

// BLE 设备名称
#define BLE_DEVICE_NAME         "JJTP-XIAOJING"

// BLE 初始化
void BLE_Init();

// BLE 是否已连接
bool BLE_IsConnected();

// BLE 发送进度数据
void BLE_SendProgress(int progress);

// BLE 发送状态数据
void BLE_SendStatus(const char* status);

// BLE 获取待处理的命令（JSON字符串）
// 返回 NULL 表示没有命令
const char* BLE_GetPendingCommand();

// BLE 暂停/恢复（云端请求期间释放射频资源）
void BLE_ShutdownForCloud();
void BLE_ResumeAfterCloud();

// BLE 是否处于云端互斥暂停状态（只读）
bool BLE_IsSuspended();

// BLE 上报 AI 云端状态给小程序（复用现有 notify characteristic）
void BLE_SendAiStatus(const char* phase, uint32_t seq, bool resultReady, int cloudError, bool bleSuspended);

#endif
