#include "BLEDriver.h"

#ifdef CONFIG_BT_ENABLED

#include <NimBLEDevice.h>

// BLE 全局对象
static NimBLEServer* pServer = NULL;
static NimBLECharacteristic* pWriteChar = NULL;
static NimBLECharacteristic* pNotifyChar = NULL;
static bool bleConnected = false;
static bool bleInitialized = false;
static bool bleSuspended = false;

// 命令缓冲区（拼接模式）
static char commandBuffer[512];
static int commandBufferLen = 0;
static volatile bool hasNewCommand = false;
static uint32_t lastChunkTime = 0;

// ==========================================
// BLE 回调类
// ==========================================

// 服务器连接回调
class ServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer) {
        bleConnected = true;
        Serial.println("✅ [BLE] 设备已连接");
    }

    void onDisconnect(NimBLEServer* pServer) {
        bleConnected = false;
        Serial.println("❌ [BLE] 设备已断开");
        // 重新开始广播
        NimBLEDevice::startAdvertising();
    }
};

// 写特征值回调（接收小程序指令 — 多包拼接模式）
class WriteCallbacks : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        int len = value.length();
        if (len <= 0) return;

        // 如果上一包已超时（3秒无后续数据），重置缓冲区
        if (commandBufferLen > 0 && millis() - lastChunkTime > 3000) {
            commandBufferLen = 0;
        }
        lastChunkTime = millis();

        // 拼接到缓冲区
        int remain = sizeof(commandBuffer) - 1 - commandBufferLen;
        if (remain < len) {
            Serial.println("❌ [BLE] 命令缓冲区溢出，重置");
            commandBufferLen = 0;
            return;
        }
        memcpy(commandBuffer + commandBufferLen, value.c_str(), len);
        commandBufferLen += len;
        commandBuffer[commandBufferLen] = '\0';

        // 检查是否收到完整JSON（大括号匹配）
        int depth = 0;
        for (int i = 0; i < commandBufferLen; i++) {
            if (commandBuffer[i] == '{') depth++;
            else if (commandBuffer[i] == '}') depth--;
        }

        if (depth == 0 && commandBufferLen > 0) {
            // 完整JSON，触发命令处理
            hasNewCommand = true;
            Serial.printf("📨 [BLE] 完整指令(%d字节): %s\n", commandBufferLen, commandBuffer);
            commandBufferLen = 0;  // 重置，准备下一条
        } else {
            Serial.printf("📥 [BLE] 收到分片(%d字节)，已缓冲%d字节，等待后续数据...\n", len, commandBufferLen);
        }
    }
};

// ==========================================
// BLE 初始化
// ==========================================
void BLE_Init() {
    Serial.println("🔵 [BLE] 正在初始化蓝牙...");
    Serial.printf("📊 [BLE] 初始化前: 内部堆=%dKB, PSRAM=%dKB\n", ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);

    // 等待其他硬件初始化完成
    delay(500);

    // 初始化BLE设备（NimBLE模式）
    NimBLEDevice::init(BLE_DEVICE_NAME);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // 最大功率

    // 创建BLE服务器
    pServer = NimBLEDevice::createServer();
    if (!pServer) {
        Serial.println("❌ [BLE] 创建服务器失败");
        return;
    }
    pServer->setCallbacks(new ServerCallbacks());

    // 创建BLE服务
    NimBLEService* pService = pServer->createService(BLE_SERVICE_UUID);
    if (!pService) {
        Serial.println("❌ [BLE] 创建服务失败");
        return;
    }

    // 创建写特征值（接收指令）
    pWriteChar = pService->createCharacteristic(
        BLE_WRITE_CHAR_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    if (!pWriteChar) {
        Serial.println("❌ [BLE] 创建写特征值失败");
        return;
    }
    pWriteChar->setCallbacks(new WriteCallbacks());

    // 创建通知特征值（发送状态）
    pNotifyChar = pService->createCharacteristic(
        BLE_NOTIFY_CHAR_UUID,
        NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::READ
    );
    if (!pNotifyChar) {
        Serial.println("❌ [BLE] 创建通知特征值失败");
        return;
    }
    // NimBLE 自动处理 CCCD 描述符，无需手动添加 BLE2902

    // 启动服务
    pService->start();

    // 配置广播（间隔放宽到100-150ms，给WiFi腾射频时间）
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinInterval(0xA0);    // 100ms
    pAdvertising->setMaxInterval(0xF0);    // 150ms

    // 开始广播
    NimBLEDevice::startAdvertising();

    bleInitialized = true;
    Serial.printf("📊 [BLE] 初始化后: 内部堆=%dKB, PSRAM=%dKB\n", ESP.getFreeHeap()/1024, ESP.getFreePsram()/1024);
    Serial.printf("✅ [BLE] 初始化完成，设备名: %s\n", BLE_DEVICE_NAME);
    Serial.println("✅ [BLE] 等待小程序连接...");
}

// ==========================================
// BLE 状态查询
// ==========================================
bool BLE_IsConnected() {
    if (bleSuspended) return false;
    return bleConnected;
}

// ==========================================
// BLE 发送进度
// ==========================================
void BLE_SendProgress(int progress) {
    if (bleSuspended || !bleConnected || !pNotifyChar) return;

    // Compact format: "P,<value>" — max 6 bytes (P,100)
    char buf[8];
    int len = snprintf(buf, sizeof(buf), "P,%d", progress);
    if (len <= 0 || len >= (int)sizeof(buf)) return;
    pNotifyChar->setValue(buf);
    pNotifyChar->notify();
}

// ==========================================
// BLE 发送状态
// ==========================================
void BLE_SendStatus(const char* status) {
    if (bleSuspended || !bleConnected || !pNotifyChar) return;

    // Compact format: "S,<code>" — max 12 bytes
    const char* code = "info";
    if (status) {
        if (strstr(status, "洗涤") || strstr(status, "启动")) code = "washing";
        else if (strstr(status, "忙")) code = "busy";
        else if (strstr(status, "完成")) code = "done";
        else if (strstr(status, "停止") || strstr(status, "已停")) code = "idle";
        else if (strstr(status, "错误") || strstr(status, "失败")) code = "error";
        else if (strstr(status, "连接")) code = "connect";
    }
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "S,%s", code);
    if (len <= 0 || len >= (int)sizeof(buf)) return;
    pNotifyChar->setValue(buf);
    pNotifyChar->notify();
}

// ==========================================
// BLE 获取待处理命令
// ==========================================
const char* BLE_GetPendingCommand() {
    if (bleSuspended) return NULL;
    if (hasNewCommand) {
        hasNewCommand = false;
        return commandBuffer;
    }
    return NULL;
}

// ==========================================
// BLE 暂停（云端请求期间释放射频资源）
// ==========================================
void BLE_ShutdownForCloud() {
    if (!bleInitialized || bleSuspended) return;
    bleSuspended = true;
    bleConnected = false;
    Serial.println("[BLE] shutting down for cloud request...");
    Serial.flush();
    NimBLEDevice::stopAdvertising();
    if (pServer && pServer->getConnectedCount() > 0) {
        pServer->disconnect(pServer->getPeerInfo(0).getConnHandle());
    }
    NimBLEDevice::deinit(true);
    Serial.printf("[BLE] deinit done, heap=%u\n", ESP.getFreeHeap());
    Serial.flush();
}

// ==========================================
// BLE 恢复
// ==========================================
void BLE_ResumeAfterCloud() {
    if (!bleSuspended) return;
    Serial.println("[BLE] resuming after cloud request...");
    Serial.flush();
    bleSuspended = false;
    bleInitialized = false;
    BLE_Init();
    Serial.println("[BLE] resume complete");
    Serial.flush();
}

bool BLE_IsSuspended() {
    return bleSuspended;
}

void BLE_SendAiStatus(const char* phase, uint32_t seq, bool resultReady, int cloudError, bool bleSuspendedFlag) {
    if (bleSuspendedFlag || !bleConnected || !pNotifyChar) return;

    // Compact format: "A,<code>,<seq>" — max ~14 bytes, well under 20 byte BLE notify limit
    char code = 'x';
    if (phase) {
        // Map long phase names to single char codes
        if (strcmp(phase, "wake_detected") == 0) code = 'w';
        else if (strcmp(phase, "recording") == 0) code = 'r';
        else if (strcmp(phase, "record_done") == 0) code = 'd';
        else if (strcmp(phase, "ble_suspending") == 0) code = 's';
        else if (strcmp(phase, "ble_resume_done") == 0) code = 'b';
        else if (strcmp(phase, "idle") == 0) code = 'i';
        else if (strcmp(phase, "failed") == 0) code = 'f';
        else if (strcmp(phase, "cloud_failed") == 0) code = 'f';
    }

    char buf[20];
    int len = snprintf(buf, sizeof(buf), "A,%c,%lu", code, (unsigned long)seq);
    if (len <= 0 || len >= (int)sizeof(buf)) return;

    pNotifyChar->setValue(buf);
    pNotifyChar->notify();
}

#else

// 蓝牙未启用时的空实现
void BLE_Init() {
    Serial.println("⚠️ [BLE] 蓝牙未启用");
}

bool BLE_IsConnected() {
    return false;
}

void BLE_SendProgress(int progress) {}

void BLE_SendStatus(const char* status) {}

const char* BLE_GetPendingCommand() {
    return NULL;
}

void BLE_ShutdownForCloud() {}
void BLE_ResumeAfterCloud() {}
bool BLE_IsSuspended() { return false; }
void BLE_SendAiStatus(const char* phase, uint32_t seq, bool resultReady, int cloudError, bool bleSuspendedFlag) {}

#endif // CONFIG_BT_ENABLED
