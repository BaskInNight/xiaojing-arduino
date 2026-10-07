/**
 * @file main.cpp
 * @brief 智能无交叉洗衣机 V3.2 终极入口
 * @note 集成小净大模型交互、语音唤醒与底层肌肉执行队列
 */

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <Wire.h>
#include "mbedtls/base64.h"
// mbedtls/platform.h — 暂停 PSRAM 重定向方向
// #include "mbedtls/platform.h"
#include <driver/gpio.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

// ⚠️ 大一统架构：基础设施与协议层
#include "Config.h"         // WiFi、API 密钥与云端配置
#include "HardwareConfig.h" // 硬件引脚与全局常量字典
#include "WashProtocol.h"   // 机器动作指令集与队列协议

// ⚠️ 大一统架构：驱动层与执行中枢
#include "HardwareDriver.h" // 纯粹的肌肉驱动
#include "WashExecutor.h"   // 带有防火墙的安全大脑
#include "BLEDriver.h"      // 蓝牙BLE通信
#include "DisplayDriver.h"  // ILI9341 屏幕显示

// ⚠️ AI 模型与本地音频
#include <gaozhisai-wake_inferencing.h>
#include "LocalAudio.h"

// TLS 对照实验：true = 跳过 BLE 初始化，验证 WiFi+BLE 共存是否导致 TLS connect 卡死
static const bool TLS_TEST_DISABLE_BLE = false;
// 唤醒提示音诊断：true = 跳过"小净在听"播放，验证是否是功放/I2S导致崩溃
static const bool TLS_TEST_DISABLE_WAKE_PROMPT_AUDIO = false;
// 全局音频提示禁用：true = 跳过所有本地提示音/TTS，验证是否是功放/I2S/供电干扰
static const bool DEBUG_DISABLE_ALL_LOCAL_AUDIO_PROMPTS = false;
// 用标准测试 WAV 替代真实录音，验证 API 格式是否正确
static const bool DEBUG_USE_FAKE_STANDARD_WAV = false;
// 详细云端调试日志：true = 打印 WAV header hex / base64 首尾 / 每 32KB 进度 / memlog
static const bool DEBUG_VERBOSE_CLOUD = false;
// 云端成功后播放固定提示音：仅测试功放/I2S播放链路，不影响主流程
static const bool DEBUG_ENABLE_POST_CLOUD_TASK_DONE_AUDIO = true;
// 音频播放诊断等级：0=跳过 1=仅功放 2=仅base64解析 3=base64+I2S(200ms,功放关) 4=功放+I2S(可配时长) 5=完整播放
static const int DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL = 0;
// Level 4 播放测试时长（毫秒），自动 clamp 到 WAV data_size
static const uint32_t DEBUG_POST_CLOUD_AUDIO_TEST_MS = 2000;
// Level 4 输出模式：0=正常音频 1=静音(全0) 2=降音量(PCM>>2, 约25%)
static const int DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE = 1;

// === 音频诊断分级开关 ===
// DEBUG_ALLOW_AMP_HARDWARE: 允许功放硬件使能（MCP23017 GPIO 控制）
//   true = setAmplifierState(true) 可以打开功放
//   false = setAmplifierState(true) 被拦截，功放永远关闭
static const bool DEBUG_ALLOW_AMP_HARDWARE = true;

// DEBUG_ENABLE_AUDIO_PLAYBACK: 允许本地提示音播放（Base64 解码 + I2S 写入）
//   true = playBase64Audio() 正常执行（本地 WAV 提示音）
//   false = playBase64Audio() 直接 return，不播放任何声音
// 注意：此开关不影响 executeTTS()，TTS 由 DEBUG_ENABLE_CLOUD_TTS 独立控制
static const bool DEBUG_ENABLE_AUDIO_PLAYBACK = true;

// DEBUG_ENABLE_CLOUD_TTS: 允许云端 TTS 合成播放
//   true = executeTTS() 正常请求云端 TTS 并播放
//   false = executeTTS() 直接 return，不请求也不播放
static const bool DEBUG_ENABLE_CLOUD_TTS = true;

// ==========================================
// 🔬 BLDC 正反转自动诊断（临时测试，完成后回退）
// ==========================================
// 1 = 开机自动运行 BLDC 正反转诊断，跳过所有洗涤/云端/手势逻辑
// 0 = 正常运行
#define DEBUG_AUTO_BLDC_DIRECTION_DIAG 0

// ==========================================
// AI 云端流程可观测状态（纯诊断，不影响业务逻辑）
// ==========================================
enum AiPhase {
    AI_IDLE = 0,
    AI_WAKE_DETECTED,
    AI_RECORDING,
    AI_RECORD_DONE,
    AI_CLOUD_PREPARING,
    AI_BLE_SUSPENDING,
    AI_BLE_SUSPENDED,
    AI_CLOUD_CONNECTING,
    AI_CLOUD_ENCODING,
    AI_CLOUD_UPLOADING,
    AI_CLOUD_WAITING_RESPONSE,
    AI_CLOUD_PARSING,
    AI_RESULT_READY,
    AI_FAILED,
    AI_BLE_RESUMING,
    AI_BLE_RESUME_DONE,
    AI_BLE_RESUME_FAILED
};
volatile AiPhase shared_ai_phase = AI_IDLE;
volatile uint32_t shared_ai_seq = 0;
volatile bool shared_ai_result_ready = false;

static const char* AiPhaseToString(AiPhase phase) {
    switch (phase) {
        case AI_IDLE: return "idle";
        case AI_WAKE_DETECTED: return "wake_detected";
        case AI_RECORDING: return "recording";
        case AI_RECORD_DONE: return "record_done";
        case AI_CLOUD_PREPARING: return "cloud_preparing";
        case AI_BLE_SUSPENDING: return "ble_suspending";
        case AI_BLE_SUSPENDED: return "ble_suspended";
        case AI_CLOUD_CONNECTING: return "cloud_connecting";
        case AI_CLOUD_ENCODING: return "cloud_encoding";
        case AI_CLOUD_UPLOADING: return "cloud_uploading";
        case AI_CLOUD_WAITING_RESPONSE: return "cloud_waiting_response";
        case AI_CLOUD_PARSING: return "cloud_parsing";
        case AI_RESULT_READY: return "result_ready";
        case AI_FAILED: return "failed";
        case AI_BLE_RESUMING: return "ble_resuming";
        case AI_BLE_RESUME_DONE: return "ble_resume_done";
        case AI_BLE_RESUME_FAILED: return "ble_resume_failed";
        default: return "unknown";
    }
}

static void SetAiPhase(AiPhase phase) {
    shared_ai_phase = phase;
    Serial.printf("[AI_STATE] %s\n", AiPhaseToString(phase));
    Serial.flush();
}

// ==========================================
// 内存诊断函数（ChatGPT 建议的详细堆分析）
// ==========================================
static void memlog(const char* tag) {
    Serial.printf(
        "\n[%s]\n"
        "reset_reason=%d\n"
        "free_heap=%u, min_free_heap=%u\n"
        "INT free=%u, INT largest=%u, INT min=%u\n"
        "DMA free=%u, DMA largest=%u\n"
        "PSRAM free=%u, PSRAM largest=%u\n"
        "stack_hwm=%u\n",
        tag,
        esp_reset_reason(),
        ESP.getFreeHeap(),
        ESP.getMinFreeHeap(),
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_DMA),
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        uxTaskGetStackHighWaterMark(NULL) * 4
    );
    bool ok = heap_caps_check_integrity_all(true);
    Serial.printf("heap_integrity=%s\n", ok ? "OK" : "BROKEN");
    Serial.flush();
}

// ==========================================
// 网络分层诊断：DNS → TCP 443 → TLS（Core 0 / Core 1 对照）
// ==========================================
static void testNetworkConnectSteps(void* pvParam) {
    int core = (int)(uintptr_t)pvParam;
    Serial.printf("\n===== [NETTEST-C%d] start, heap=%u =====\n", core, ESP.getFreeHeap());
    Serial.flush();

    // 1. DNS
    IPAddress ip;
    Serial.printf("[NETTEST-C%d] before DNS\n", core);
    Serial.flush();
    bool dnsOk = WiFi.hostByName("api.xiaomimimo.com", ip);
    Serial.printf("[NETTEST-C%d] after DNS ok=%d ip=%s\n", core, dnsOk, ip.toString().c_str());
    Serial.flush();

    // 2. 明文 TCP 443
    WiFiClient tcp;
    tcp.setTimeout(10000);
    Serial.printf("[NETTEST-C%d] before TCP 443 connect\n", core);
    Serial.flush();
    bool tcpOk = tcp.connect("api.xiaomimimo.com", 443);
    Serial.printf("[NETTEST-C%d] after TCP 443 connect ok=%d\n", core, tcpOk);
    Serial.flush();
    tcp.stop();

    // 3. TLS host
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);
    client.setHandshakeTimeout(10);
    Serial.printf("[NETTEST-C%d] before TLS host connect\n", core);
    Serial.flush();
    bool tlsOk = client.connect("api.xiaomimimo.com", 443);
    Serial.printf("[NETTEST-C%d] after TLS host connect ok=%d\n", core, tlsOk);
    Serial.flush();
    client.stop();

    Serial.printf("[NETTEST-C%d] DONE dns=%d tcp=%d tls=%d heap=%u\n", core, dnsOk, tcpOk, tlsOk, ESP.getFreeHeap());
    Serial.flush();
    vTaskDelete(NULL);
}

// ==========================================
// 1. 网络与 API 映射配置 (对接 Config.h)
// ==========================================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* apiKey = MIMO_API_KEY;
const char* apiUrl = MIMO_API_URL;
static const char* MIMO_API_HOST = "api.xiaomimimo.com";
static const IPAddress PRIMARY_DNS(223, 5, 5, 5);
static const IPAddress SECONDARY_DNS(1, 1, 1, 1);

static void configureReliableDns() {
    // 保留 DHCP 分配的本机地址/网关/掩码，只覆盖 DNS 服务器。
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, PRIMARY_DNS, SECONDARY_DNS);
}

static bool resolveMimoHostWithRetry(IPAddress& resolvedIp) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (WiFi.hostByName(MIMO_API_HOST, resolvedIp)) {
            Serial.printf("[DNS] %s -> %s (attempt %d)\n",
                          MIMO_API_HOST, resolvedIp.toString().c_str(), attempt);
            return true;
        }
        Serial.printf("[DNS] resolve failed (attempt %d/3)\n", attempt);
        Serial.flush();
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    return false;
}

#define GPB7_BIT MCP_GPB7_LED // 映射心跳灯至硬件配置引脚

// ==========================================
// 2. 全局状态机与交互标志
// ==========================================
enum SystemState { SYS_WAKE_DETECT, SYS_RECORDING };
SystemState currentState = SYS_WAKE_DETECT;

bool isPlaying = false;

// ==========================================
// 3. 音频流捕获与 VAD 侦听参数
// ==========================================
#define WINDOW_SIZE EI_CLASSIFIER_RAW_SAMPLE_COUNT 
#define STEP_SIZE   (WINDOW_SIZE / 4)              
int16_t* audio_buffer = NULL;  // 按需分配到 PSRAM，唤醒时分配，录音前释放
int buffer_idx = 0;   

#define REC_TIME_SEC 5
#define REC_SAMPLE_RATE 16000
#define REC_BUFFER_SIZE (REC_SAMPLE_RATE * 2 * REC_TIME_SEC) 

// ==========================================
// 4. PSRAM JSON 内存分配器
// ==========================================
class SpiRamAllocator : public ArduinoJson::Allocator {
public:
    void* allocate(size_t size) override { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM); }
    void deallocate(void* ptr) override { heap_caps_free(ptr); }
    void* reallocate(void* ptr, size_t new_size) override { return heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM); }
};
SpiRamAllocator mySpiRamAlloc;

// ==========================================
// 5. FreeRTOS 双核调度
// ==========================================
volatile bool cloud_task_done = false;
char* shared_tts_reply = NULL;
uint8_t* shared_pcm_buf = NULL;
size_t shared_pcm_len = 0;

// 延迟执行：pipeline 步骤先缓存，TTS 播完后再推入执行器
static const int MAX_DEFERRED_STEPS = 20;
WashStep_t shared_deferred_steps[MAX_DEFERRED_STEPS];
int shared_deferred_count = 0;

// 统一错误码：替代多个 volatile bool
enum CloudError {
    CLOUD_OK = 0,
    CLOUD_NET_ERROR,
    CLOUD_API_TIMEOUT,
    CLOUD_JSON_ERROR
};
volatile CloudError shared_cloud_error = CLOUD_OK;
volatile bool shared_is_pure_pref = false;

static void NotifyAiStatus() {
    BLE_SendAiStatus(
        AiPhaseToString(shared_ai_phase),
        shared_ai_seq,
        shared_ai_result_ready,
        (int)shared_cloud_error,
        BLE_IsSuspended()
    );
}

void printMemoryStatus(const char* tag) {
    Serial.printf("🔍 [%s] SRAM: %d B | PSRAM: %d B\n", tag, ESP.getFreeHeap(), ESP.getFreePsram());
}

void handleMCPHeartbeat() {
    static uint32_t lastHeartbeatTime = 0;
    static bool mcpHeartbeatState = false;

    if (millis() - lastHeartbeatTime >= 200) { 
        lastHeartbeatTime = millis();
        mcpHeartbeatState = !mcpHeartbeatState;
        DRV_SetHeartbeat(mcpHeartbeatState); // 🚀 直接调用底层肌肉驱动，不再接触寄存器
    }
}

void setAmplifierState(bool enable) {
    if (enable && !DEBUG_ALLOW_AMP_HARDWARE) {
        Serial.println("[AUDIO_SAFE] amp hardware disabled, amp on blocked");
        Serial.flush();
        return;
    }
    uint8_t gpioa_val = mcp_read_register(0x12);
    if (enable) gpioa_val |= (1 << 3);
    else gpioa_val &= ~(1 << 3);
    mcp_write_register(0x12, gpioa_val);
}

// INMP441 32-bit → 16-bit PCM 转换
static size_t convertInmp441Raw32ToPcm16(
    const int32_t* raw32, size_t rawBytes,
    int16_t* pcm16, size_t maxPcmBytes)
{
    size_t samples = rawBytes / 4;
    if (maxPcmBytes < samples * 2) samples = maxPcmBytes / 2;
    for (size_t i = 0; i < samples; i++) {
        int32_t v = raw32[i] >> 14;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        pcm16[i] = (int16_t)v;
    }
    return samples * 2;
}

// PCM16 诊断
static void dumpPcmStats(const int16_t* pcm, size_t pcmLen) {
    size_t count = pcmLen / 2;
    if (count == 0) { Serial.println("[PCM] empty"); return; }
    int16_t mn = pcm[0], mx = pcm[0];
    long long sumAbs = 0;
    int zeroCnt = 0, satCnt = 0;
    for (size_t i = 0; i < count; i++) {
        int16_t s = pcm[i];
        if (s < mn) mn = s;
        if (s > mx) mx = s;
        sumAbs += abs(s);
        if (s == 0) zeroCnt++;
        if (s == 32767 || s == -32768) satCnt++;
    }
    Serial.printf("[PCM] len=%u, samples=%u, min=%d, max=%d, avgAbs=%lld, zero=%d, sat=%d\n",
        pcmLen, count, mn, mx, sumAbs / count, zeroCnt, satCnt);
    Serial.print("[PCM] first16:");
    for (size_t i = 0; i < 16 && i < count; i++) Serial.printf(" %d", pcm[i]);
    Serial.println();
    Serial.flush();
}

void i2s_combined_init() {
    const i2s_config_t i2s_config = {
        .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = i2s_bits_per_sample_t(32),
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, 
        .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false, 
        .tx_desc_auto_clear = true 
    };
    const i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK, .ws_io_num = I2S_WS,
        .data_out_num = I2S_AMP_DIN, .data_in_num = I2S_SD        
    };
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);

    gpio_set_drive_capability((gpio_num_t)I2S_SCK, GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability((gpio_num_t)I2S_WS, GPIO_DRIVE_CAP_1);
    gpio_set_drive_capability((gpio_num_t)I2S_AMP_DIN, GPIO_DRIVE_CAP_1);
}

int microphone_audio_signal_get_data(size_t offset, size_t length, float *out_ptr) {
    numpy::int16_to_float(&audio_buffer[offset], out_ptr, length);
    return 0;
}

void generateWavHeader(uint8_t* header, uint32_t pcmDataLen, uint32_t sampleRate = 16000) {
    uint32_t totalDataLen = pcmDataLen + 36;
    uint32_t byteRate = sampleRate * 2;
    uint8_t head[44] = {
        'R','I','F','F',
        (uint8_t)(totalDataLen & 0xff), (uint8_t)((totalDataLen >> 8) & 0xff), (uint8_t)((totalDataLen >> 16) & 0xff), (uint8_t)((totalDataLen >> 24) & 0xff),
        'W','A','V','E','f','m','t',' ',
        16, 0, 0, 0, 1, 0, 1, 0,
        (uint8_t)(sampleRate & 0xff), (uint8_t)((sampleRate >> 8) & 0xff), (uint8_t)((sampleRate >> 16) & 0xff), (uint8_t)((sampleRate >> 24) & 0xff),
        (uint8_t)(byteRate & 0xff), (uint8_t)((byteRate >> 8) & 0xff), (uint8_t)((byteRate >> 16) & 0xff), (uint8_t)((byteRate >> 24) & 0xff),
        2, 0, 16, 0,
        'd','a','t','a',
        (uint8_t)(pcmDataLen & 0xff), (uint8_t)((pcmDataLen >> 8) & 0xff), (uint8_t)((pcmDataLen >> 16) & 0xff), (uint8_t)((pcmDataLen >> 24) & 0xff)
    };
    memcpy(header, head, 44);
}

void dumpWavHeader(const uint8_t* wav, size_t wavSize) {
    Serial.println("[WAV] header hex (first 44 bytes):");
    for (int i = 0; i < 44 && i < (int)wavSize; i++) {
        Serial.printf("%02X ", wav[i]);
        if ((i + 1) % 16 == 0) Serial.println();
    }
    Serial.println();
    Serial.printf("[WAV] riff=%.4s, wave=%.4s, fmt=%.4s, data=%.4s\n",
        wav, wav + 8, wav + 12, wav + 36);
    uint32_t riffSize = *(uint32_t*)(wav + 4);
    uint16_t audioFmt = *(uint16_t*)(wav + 20);
    uint16_t channels = *(uint16_t*)(wav + 22);
    uint32_t sampleRate = *(uint32_t*)(wav + 24);
    uint32_t byteRate = *(uint32_t*)(wav + 28);
    uint16_t blockAlign = *(uint16_t*)(wav + 32);
    uint16_t bitsPerSample = *(uint16_t*)(wav + 34);
    uint32_t dataSize = *(uint32_t*)(wav + 40);
    Serial.printf("[WAV] riffSize=%u, audioFmt=%u, channels=%u, sampleRate=%u\n", riffSize, audioFmt, channels, sampleRate);
    Serial.printf("[WAV] byteRate=%u, blockAlign=%u, bitsPerSample=%u\n", byteRate, blockAlign, bitsPerSample);
    Serial.printf("[WAV] dataSize=%u, wavSize=%u, expected_dataSize=%u\n", dataSize, wavSize, wavSize - 44);
    Serial.flush();
}

void playBase64Audio(const char* base64Wav) {
    if (!DEBUG_ENABLE_AUDIO_PLAYBACK) {
        Serial.println("[AUDIO_SAFE] blocked because DEBUG_ENABLE_AUDIO_PLAYBACK=false");
        Serial.flush();
        return;
    }
    Serial.println("[AUDIO_PLAY] begin"); Serial.flush();
    size_t input_len = strlen(base64Wav);
    if (input_len == 0) return;

    isPlaying = true;
    setAmplifierState(true);
    Serial.println("[AUDIO_PLAY] amp on"); Serial.flush();

    size_t decoded_len = (input_len / 4) * 3;
    uint8_t* wav_buf = (uint8_t*)heap_caps_malloc(decoded_len, MALLOC_CAP_SPIRAM);
    if (!wav_buf) {
        Serial.println("[AUDIO_PLAY] alloc FAILED"); Serial.flush();
        setAmplifierState(false); isPlaying = false; return;
    }

    size_t actual_len = 0;
    mbedtls_base64_decode(wav_buf, decoded_len, &actual_len, (const unsigned char*)base64Wav, input_len);

    // 校验：解码后至少要 44 字节的标准 WAV 头
    if (actual_len < 44) {
        Serial.printf("❌ [音频] WAV 数据过短 (%d 字节)，无法解析头\n", actual_len);
        free(wav_buf);
        setAmplifierState(false);
        isPlaying = false;
        return;
    }

    uint32_t sample_rate = wav_buf[24] | (wav_buf[25] << 8) | (wav_buf[26] << 16) | (wav_buf[27] << 24);
    uint16_t channels = wav_buf[22] | (wav_buf[23] << 8);
    uint16_t bits = wav_buf[34] | (wav_buf[35] << 8);
    uint32_t data_size = wav_buf[40] | (wav_buf[41] << 8) | (wav_buf[42] << 16) | (wav_buf[43] << 24);
    uint32_t byteRate = sample_rate * channels * (bits / 8);
    uint32_t duration_ms = (byteRate > 0) ? (data_size * 1000UL / byteRate) : 0;
    Serial.printf("[AUDIO_PLAY] wav parsed sr=%u bits=%u ch=%u data=%u duration=%ums\n",
        sample_rate, bits, channels, data_size, duration_ms); Serial.flush();

    // 切换 I2S 到 16-bit stereo（WAV 是 16-bit，硬件默认 32-bit 会导致消费速度翻倍）
    i2s_set_clk(I2S_PORT, sample_rate, 16, I2S_CHANNEL_STEREO);

    uint32_t data_offset = 44;
    for (uint32_t i = 12; i < actual_len - 4; i++) {
        if (wav_buf[i] == 'd' && wav_buf[i+1] == 'a' && wav_buf[i+2] == 't' && wav_buf[i+3] == 'a') {
            data_offset = i + 8; break;
        }
    }

    int16_t* pcm_data = (int16_t*)(wav_buf + data_offset);
    size_t pcm_samples = (actual_len > data_offset) ? (actual_len - data_offset) / 2 : 0;

    // 批量写入优化：将 mono 转 stereo 后一次性写入
    const size_t BATCH_SAMPLES = 512; // 每批 512 个立体声样本 = 2048 字节
    int16_t* stereo_buf = (int16_t*)heap_caps_malloc(BATCH_SAMPLES * 4, MALLOC_CAP_DMA);
    if (stereo_buf) {
        Serial.println("[AUDIO_PLAY] i2s write begin"); Serial.flush();
        size_t total_written = 0;
        for (size_t i = 0; i < pcm_samples; i += BATCH_SAMPLES) {
            size_t batch_count = min(BATCH_SAMPLES, pcm_samples - i);
            // mono 转 stereo
            for (size_t j = 0; j < batch_count; j++) {
                stereo_buf[j * 2] = pcm_data[i + j];
                stereo_buf[j * 2 + 1] = pcm_data[i + j];
            }
            size_t bytes_written = 0;
            i2s_write(I2S_PORT, stereo_buf, batch_count * 4, &bytes_written, portMAX_DELAY);
            total_written += bytes_written;
            handleMCPHeartbeat();
            DISP_Handler();
        }
        Serial.printf("[AUDIO_PLAY] i2s write end (%u bytes)\n", (unsigned)total_written); Serial.flush();
        heap_caps_free(stereo_buf);
    }

    // 静音填充：批量写入
    int16_t* silence_buf = (int16_t*)heap_caps_calloc(2048, 2, MALLOC_CAP_DMA);
    if (silence_buf) {
        size_t bytes_written;
        i2s_write(I2S_PORT, silence_buf, 4096, &bytes_written, portMAX_DELAY);
        heap_caps_free(silence_buf);
    }

    i2s_zero_dma_buffer(I2S_PORT);
    setAmplifierState(false);
    Serial.println("[AUDIO_PLAY] amp off"); Serial.flush();
    free(wav_buf);
    // 恢复 I2S 到麦克风使用的 32-bit 16kHz
    i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);
    isPlaying = false;
}


// ==========================================
// 3. TTS 云端请求
// ==========================================
class PSRAMStream : public Stream {
public:
    char* buffer;
    size_t index;
    size_t max_size;
    HTTPClient* httpObj;
    
    PSRAMStream(size_t size, HTTPClient* h) {
        max_size = size;
        index = 0;
        httpObj = h;
        buffer = (char*)heap_caps_malloc(max_size, MALLOC_CAP_SPIRAM);
        if (buffer) buffer[0] = '\0';
    }
    ~PSRAMStream() { if (buffer) heap_caps_free(buffer); }
    
    size_t write(uint8_t data) override {
        if (buffer && index < max_size - 1) { buffer[index++] = data; buffer[index] = '\0'; return 1; }
        return 0;
    }
    
    size_t write(const uint8_t *buf, size_t size) override {
        if (buffer && index + size < max_size - 1) {
            memcpy(buffer + index, buf, size);
            index += size;
            buffer[index] = '\0';
            handleMCPHeartbeat(); 
            
            char* search_start = index > 256 ? (buffer + index - 256) : buffer;
            if (strstr(search_start, "\"expires_at\"") != NULL || strstr(search_start, "\"usage\"") != NULL) {
                if (httpObj && httpObj->getStreamPtr()) { httpObj->getStreamPtr()->stop(); }
            }
            return size;
        }
        return 0;
    }
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
    void flush() override {}
};

void executeTTS(const char* text) {
    if (!DEBUG_ENABLE_CLOUD_TTS) {
        Serial.println("[AUDIO_SAFE] executeTTS skipped (cloud TTS disabled)");
        Serial.flush();
        return;
    }
    if (!text || strlen(text) == 0) return;
    // 限制 TTS 文本长度，避免过长请求（~60 中文字符 ≈ 180 字节）
    static const size_t TTS_MAX_BYTES = 200;
    char truncated[TTS_MAX_BYTES];
    if (strlen(text) >= TTS_MAX_BYTES) {
        strncpy(truncated, text, TTS_MAX_BYTES - 4);
        strcpy(truncated + TTS_MAX_BYTES - 4, "...");
        text = truncated;
        Serial.printf("[TTS] text truncated to %u bytes\n", TTS_MAX_BYTES - 1);
    }
    Serial.printf("[TTS] requesting cloud TTS for: %.80s%s\n", text, strlen(text) > 80 ? "..." : "");
    Serial.flush();

    // TLS 握手可能 >5s，临时摘除 Core 0 空闲任务的看门狗
    TaskHandle_t idle_0 = xTaskGetIdleTaskHandleForCPU(0);
    esp_task_wdt_delete(idle_0);

    WiFiClientSecure client; client.setInsecure();
    HTTPClient http; http.setTimeout(25000); 
    http.begin(client, apiUrl);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("api-key", apiKey);
    http.addHeader("Connection", "close"); 

    JsonDocument doc(&mySpiRamAlloc);
    doc["model"] = "mimo-v2.5-tts";
    JsonArray msgs = doc["messages"].to<JsonArray>();
    
    // ⚠️ 严格遵照官方文档：自然语言风格控制必须放在 role: user 中
    JsonObject styleMsg = msgs.add<JsonObject>();
    styleMsg["role"] = "user"; 
    // 这是我为你精调的提示词，能让“冰糖”的声音瞬间变得活泼、亲切、语速明快！
    styleMsg["content"] = "用明亮活泼的年轻女性嗓音，像一个温柔贴心、充满活力的家庭管家在跟主人说话。语气亲切自然，带一点微笑的感情，语速轻快干练，彻底告别播音腔。";
    
    // ⚠️ 严格遵照官方文档：要合成的纯净文本必须放在 role: assistant 中
    JsonObject contentMsg = msgs.add<JsonObject>();
    contentMsg["role"] = "assistant"; 
    // 直接传入文本，不再强加 (开心) 这种括号标签，让 user 的提示词去发挥作用
    contentMsg["content"] = text; 
    
    // ⚠️ 严格遵照官方文档：只传支持的参数
    JsonObject audioCfg = doc["audio"].to<JsonObject>();
    audioCfg["format"] = "wav"; 
    audioCfg["voice"] = "冰糖"; 

    String requestBody; serializeJson(doc, requestBody);
    
    int httpCode = http.POST(requestBody);
    
    if (httpCode == 200) {
        PSRAMStream psram_stream(500 * 1024, &http);
        if (psram_stream.buffer) {
            http.writeToStream(&psram_stream);

            char* json_buf = psram_stream.buffer;
            char* start_ptr = strstr(json_buf, "\"data\""); 
            if (start_ptr) {
                char* colon_ptr = strchr(start_ptr, ':'); 
                if (colon_ptr) {
                    char* value_quote = strchr(colon_ptr, '"'); 
                    if (value_quote) {
                        start_ptr = value_quote + 1; 
                        char* end_quote = strchr(start_ptr, '"'); 
                        if (end_quote) {
                            *end_quote = '\0'; 
                            playBase64Audio(start_ptr); 
                        }
                    }
                }
            }
            if (start_ptr == NULL || strstr(json_buf, "\"data\"") == NULL) {
                playBase64Audio(AUDIO_JSON_ERROR); 
            }
        } else {
            playBase64Audio(AUDIO_JSON_ERROR);
        }
    } else {
        playBase64Audio(AUDIO_API_TIMEOUT); 
    }
    
    http.end();
    client.stop(); // ⚠️ 修复 Socket 句柄泄漏
    esp_task_wdt_add(idle_0);  // 恢复看门狗订阅
}

// ==========================================
// 4. FreeRTOS 后台守护进程 (⚠️ 静态任务池架构)
// ==========================================
// 逐行读取（带超时），替代 readStringUntil 避免服务端延迟时直接返回空
// ==========================================
static bool readLineWithTimeout(WiFiClientSecure& client, String& line, uint32_t timeoutMs) {
    line = "";
    unsigned long start = millis();
    while (millis() - start < timeoutMs) {
        if (!client.connected() && !client.available()) return false;
        int avail = client.available();
        if (avail > 0) {
            for (int i = 0; i < avail; i++) {
                int c = client.read();
                if (c < 0) break;
                if (c == '\n') return true;
                if (c != '\r') line += (char)c;
            }
        } else {
            vTaskDelay(1);
        }
    }
    return line.length() > 0;  // 超时但有数据也算成功
}

// ==========================================
// 读取 HTTPS 响应（状态行 + headers + body 最多 32KB，支持 Content-Length/chunked）
static bool readHttpResponseLimited(WiFiClientSecure& client, int& statusCode, char*& responseBody, size_t& responseLen) {
    statusCode = -1;
    responseBody = nullptr;
    responseLen = 0;

    // 等待服务端首字节，最多 60 秒
    unsigned long waitStart = millis();
    while (!client.available()) {
        if (!client.connected()) {
            Serial.println("[HTTP] connection closed before response byte");
            return false;
        }
        if (millis() - waitStart > 60000) {
            Serial.println("[HTTP] wait first response byte timeout 60s");
            return false;
        }
        if ((millis() - waitStart) % 1000 < 15) {
            Serial.printf("[HTTP] waiting response... %lu ms\n", millis() - waitStart);
            Serial.flush();
        }
        vTaskDelay(10);
    }
    Serial.printf("[HTTP] first byte available after %lu ms\n", millis() - waitStart);
    Serial.flush();

    // 读取状态行
    String statusLine;
    if (!readLineWithTimeout(client, statusLine, 30000)) {
        Serial.println("[HTTP] status line read timeout");
        return false;
    }
    statusLine.trim();
    Serial.printf("[HTTP] status line raw: %s\n", statusLine.c_str());
    Serial.flush();
    if (statusLine.startsWith("HTTP/1.1 ")) statusCode = statusLine.substring(9, 12).toInt();
    else if (statusLine.startsWith("HTTP/1.0 ")) statusCode = statusLine.substring(9, 12).toInt();
    else { Serial.printf("❌ 无效状态行: %.64s\n", statusLine.c_str()); return false; }

    // 读取 headers
    int contentLength = -1;
    bool chunked = false;
    while (client.connected() || client.available()) {
        String line;
        if (!readLineWithTimeout(client, line, 15000)) break;
        line.trim();
        if (line.length() == 0) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("content-length:")) contentLength = line.substring(15).toInt();
        if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
    }

    Serial.printf("[HTTP] headers done, content-length=%d, chunked=%d\n", contentLength, chunked);
    Serial.flush();

    const size_t MAX_BODY = 32768;
    size_t cap = MAX_BODY;
    if (!chunked && contentLength >= 0 && (size_t)contentLength < cap) cap = contentLength;

    responseBody = (char*)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!responseBody) { Serial.println("❌ responseBody PSRAM分配失败"); return false; }

    size_t total = 0;
    if (chunked) {
        // RFC 7230 chunked body: <hex-size> CRLF <data> CRLF ... 0 CRLF trailers CRLF
        while (true) {
            String sizeLine;
            if (!readLineWithTimeout(client, sizeLine, 15000)) {
                Serial.println("❌ chunk size读取超时");
                heap_caps_free(responseBody); responseBody = nullptr;
                return false;
            }
            sizeLine.trim();
            int extensionPos = sizeLine.indexOf(';');
            if (extensionPos >= 0) sizeLine = sizeLine.substring(0, extensionPos);
            sizeLine.trim();

            char* parseEnd = nullptr;
            unsigned long chunkSize = strtoul(sizeLine.c_str(), &parseEnd, 16);
            if (sizeLine.length() == 0 || parseEnd == sizeLine.c_str() || *parseEnd != '\0') {
                Serial.printf("❌ 无效chunk size: %.32s\n", sizeLine.c_str());
                heap_caps_free(responseBody); responseBody = nullptr;
                return false;
            }

            if (chunkSize == 0) {
                // 读取并忽略可选 trailer headers，直到空行。
                while (client.connected() || client.available()) {
                    String trailer;
                    if (!readLineWithTimeout(client, trailer, 5000)) break;
                    trailer.trim();
                    if (trailer.length() == 0) break;
                }
                break;
            }

            if (chunkSize > cap - total) {
                Serial.printf("❌ chunked响应超过上限: total=%u chunk=%lu max=%u\n",
                    (unsigned)total, chunkSize, (unsigned)cap);
                heap_caps_free(responseBody); responseBody = nullptr;
                return false;
            }

            size_t chunkRead = 0;
            unsigned long activityStart = millis();
            while (chunkRead < chunkSize) {
                int avail = client.available();
                if (avail > 0) {
                    size_t remaining = (size_t)chunkSize - chunkRead;
                    size_t n = (size_t)avail > remaining ? remaining : (size_t)avail;
                    int r = client.readBytes((uint8_t*)responseBody + total + chunkRead, n);
                    if (r > 0) { chunkRead += (size_t)r; activityStart = millis(); }
                } else {
                    if (!client.connected() || millis() - activityStart > 15000) {
                        Serial.printf("❌ chunk数据读取中断: %u/%lu\n", (unsigned)chunkRead, chunkSize);
                        heap_caps_free(responseBody); responseBody = nullptr;
                        return false;
                    }
                    vTaskDelay(1);
                }
            }
            total += chunkRead;

            // 消费每个 chunk 数据后的 CRLF。
            String chunkTerminator;
            if (!readLineWithTimeout(client, chunkTerminator, 5000) || chunkTerminator.length() != 0) {
                Serial.println("❌ chunk结尾CRLF无效");
                heap_caps_free(responseBody); responseBody = nullptr;
                return false;
            }
        }
        Serial.printf("📥 chunked响应解码完成: %u 字节\n", (unsigned)total);
    } else {
        // 读取普通 Content-Length/connection-close body 到 PSRAM。
        unsigned long activityStart = millis();
        while ((client.connected() || client.available()) && total < cap) {
            int avail = client.available();
            if (avail > 0) {
                size_t room = cap - total;
                size_t n = (size_t)avail > room ? room : (size_t)avail;
                int r = client.readBytes((uint8_t*)responseBody + total, n);
                if (r > 0) { total += (size_t)r; activityStart = millis(); }
            } else {
                if (millis() - activityStart > 15000) break;
                vTaskDelay(1);
            }
        }
    }
    responseBody[total] = '\0';
    responseLen = total;
    Serial.printf("📥 响应 body 读取: %u/%d 字节\n", (unsigned)total, contentLength);
    Serial.flush();
    if (statusCode < 200 || statusCode >= 300) {
        Serial.println("[HTTP_ERROR_BODY_BEGIN]");
        for (size_t i = 0; i < responseLen && i < 512; i++) {
            char c = responseBody[i];
            if (c >= 32 && c <= 126) Serial.print(c);
            else if (c == '\n' || c == '\r' || c == '\t') Serial.print(c);
            else Serial.print('.');
        }
        Serial.println();
        Serial.println("[HTTP_ERROR_BODY_END]");
        Serial.flush();
    }
    return true;
}

void cloudNetworkTask(void *pvParameters) {
    Serial.printf("[CLOUD] cloudNetworkTask entered, SRAM=%u, PSRAM=%u, core=%d\n", ESP.getFreeHeap(), ESP.getFreePsram(), xPortGetCoreID());
    Serial.flush();
    memlog("cloud-task-start");
    Serial.flush();

    TaskHandle_t idle_0 = xTaskGetIdleTaskHandleForCPU(0);
    esp_task_wdt_delete(idle_0);

    char* _skeletonJson = nullptr;
    char* _fullBase64 = nullptr;

    do {
        if (!shared_pcm_buf || shared_pcm_len == 0) break;
        if (EXE_IsWashing()) {
            Serial.println("⚠️ [云端AI] 洗涤进行中，拒绝处理新指令");
            heap_caps_free(shared_pcm_buf); shared_pcm_buf = NULL; shared_pcm_len = 0;
            break;
        }

        // 先建立 TLS 连接，避免 base64/JSON 构建后再握手导致内部 SRAM 不稳定
        WiFiClientSecure client;
        client.setInsecure();
        client.setTimeout(60000);

        SetAiPhase(AI_CLOUD_CONNECTING);
        if (DEBUG_VERBOSE_CLOUD) memlog("TLS connect前");

        Serial.printf("[TLS] WiFi.status=%d, RSSI=%d, IP=%s, core=%d, millis=%lu\n",
            WiFi.status(),
            WiFi.RSSI(),
            WiFi.localIP().toString().c_str(),
            xPortGetCoreID(),
            millis()
        );

        if (WiFi.status() != WL_CONNECTED) {
            Serial.printf("WiFi not connected before TLS, status=%d\n", WiFi.status());
            shared_cloud_error = CLOUD_NET_ERROR;
            shared_ai_result_ready = false;
            SetAiPhase(AI_FAILED);
            client.stop();
            break;
        }

        Serial.printf("[TLS] before client.connect, SRAM=%u, PSRAM=%u, core=%d\n", ESP.getFreeHeap(), ESP.getFreePsram(), xPortGetCoreID());
        Serial.flush();
        setAmplifierState(false);
        delay(50);
        Serial.println("[AUDIO_TEST] amplifier forced OFF before TLS");
        Serial.flush();

        IPAddress mimoIp;
        bool dnsResolved = resolveMimoHostWithRetry(mimoIp);
        bool connected = dnsResolved && client.connect(MIMO_API_HOST, 443);

        Serial.println("[TLS] after client.connect");
        Serial.printf("[TLS] connected=%d\n", connected);
        Serial.flush();

        if (DEBUG_VERBOSE_CLOUD) memlog("TLS connect后");

        if (!connected) {
            Serial.println("TLS connect failed");
            shared_cloud_error = CLOUD_NET_ERROR;
            shared_ai_result_ready = false;
            SetAiPhase(AI_FAILED);
            client.stop();
            break;
        }

        // 1. WAV + Base64
        SetAiPhase(AI_CLOUD_ENCODING);
        if (DEBUG_VERBOSE_CLOUD) memlog("base64前");
        uint32_t wav_size;
        uint8_t* wav_buf;
        if (DEBUG_USE_FAKE_STANDARD_WAV) {
            // 标准测试 WAV: 16kHz, mono, 16-bit, 1秒 440Hz 正弦波
            const uint32_t sampleRate = 16000;
            const uint32_t duration = 1;
            const uint32_t pcm_len = sampleRate * duration * 2; // 16-bit mono
            wav_size = 44 + pcm_len;
            wav_buf = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
            if (!wav_buf) { Serial.println("❌ wav_buf PSRAM分配失败!"); break; }
            generateWavHeader(wav_buf, pcm_len, sampleRate);
            // 填充 440Hz 正弦波
            for (uint32_t i = 0; i < pcm_len / 2; i++) {
                float t = (float)i / sampleRate;
                int16_t sample = (int16_t)(16000.0f * sinf(2.0f * 3.14159265f * 440.0f * t));
                wav_buf[44 + i * 2] = sample & 0xFF;
                wav_buf[44 + i * 2 + 1] = (sample >> 8) & 0xFF;
            }
            Serial.printf("[WAV] FAKE standard WAV: %u bytes (%uHz, mono, 16-bit, %us)\n", wav_size, sampleRate, duration);
        } else {
            wav_size = 44 + shared_pcm_len;
            wav_buf = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
            if (!wav_buf) { Serial.println("❌ wav_buf PSRAM分配失败!"); break; }
            generateWavHeader(wav_buf, shared_pcm_len);
            memcpy(wav_buf + 44, shared_pcm_buf, shared_pcm_len);
            heap_caps_free(shared_pcm_buf); shared_pcm_buf = NULL;
        }
        if (DEBUG_VERBOSE_CLOUD) dumpWavHeader(wav_buf, wav_size);

        const char* audioPrefix = "data:audio/wav;base64,";
        size_t audioPrefixLen = strlen(audioPrefix);
        size_t rawBase64Len = 4 * ((wav_size + 2) / 3);
        size_t fullAudioLen = audioPrefixLen + rawBase64Len;
        char* full_base64_str = (char*)heap_caps_malloc(fullAudioLen + 1, MALLOC_CAP_SPIRAM);
        if (!full_base64_str) { heap_caps_free(wav_buf); Serial.println("❌ base64 PSRAM分配失败!"); break; }
        _fullBase64 = full_base64_str;

        memcpy(full_base64_str, audioPrefix, audioPrefixLen);
        size_t olen = 0;
        int b64_ret = mbedtls_base64_encode((unsigned char*)(full_base64_str + audioPrefixLen), rawBase64Len + 1, &olen, wav_buf, wav_size);
        if (b64_ret != 0) { Serial.printf("❌ base64 encode failed: %d\n", b64_ret); heap_caps_free(wav_buf); break; }
        if (olen != rawBase64Len) {
            Serial.printf("❌ base64 length mismatch: olen=%u expected=%u\n", olen, rawBase64Len);
            heap_caps_free(wav_buf); break;
        }
        full_base64_str[fullAudioLen] = '\0';
        heap_caps_free(wav_buf);
        Serial.printf("📦 [内存] rawBase64Len=%u, fullAudioLen=%u, SRAM=%u, PSRAM=%u\n", rawBase64Len, fullAudioLen, ESP.getFreeHeap(), ESP.getFreePsram());
        Serial.flush();
        if (DEBUG_VERBOSE_CLOUD) memlog("base64后");


        // 3. 构建 JSON
        Preferences prefs; prefs.begin("xiaojing", true);
        String history_pref = prefs.getString("user_pref", "无特殊偏好");
        prefs.end();

        JsonDocument doc(&mySpiRamAlloc);
        doc["model"] = "mimo-v2.5";
        JsonArray msgs = doc["messages"].to<JsonArray>();

        String sysPrompt = "你是智能洗衣管家小净。严禁Emoji。必须调用工具生成洗涤pipeline。\n";
        sysPrompt += "【用户历史偏好】：" + history_pref + "\n\n";
        sysPrompt += "【系统自动处理（你无需生成）】：\n  - BLDC内筒归零：每次洗涤开始前系统自动执行\n  - 归零复位：若你的pipeline末尾不是CMD_TILT_BARREL(val=0)，系统自动补上\n\n";
        sysPrompt += "【你可编排的全部指令】：\n1. CMD_WATER_IN: 进水(val:0关UV/1开UV, time:最大等待秒数)\n2. CMD_WASH_PULSATOR: 波轮洗(val:1轻柔/2标准/3强力, time:秒) - 需在0°位置\n3. CMD_WASH_DRUM: 滚筒洗(val:1轻柔/2标准/3强力, time:秒) - 需在90°位置\n4. CMD_SPIN_DRY: 脱水(val:1沥水/2高速脱水, time:秒) - 需在180°位置\n5. CMD_TILT_BARREL: 换筒(val:0/90/180对应角度, time:0) - 自由切换桶姿态\n6. CMD_TURBIDITY_CHECK: 浊度检测(val:0单次/1持续, time:0)\n7. CMD_UV_STERILIZE: 洗后紫外杀菌(val:1开UV/0关UV, time:持续秒数) - 建议洗涤完成后追加\n\n";
        sysPrompt += "【编排规则】：\n- 波轮洗前必须有CMD_TILT_BARREL(val=0)，滚筒洗前必须有CMD_TILT_BARREL(val=90)，脱水前必须有CMD_TILT_BARREL(val=180)\n- 可自由组合：如波轮洗→滚筒洗→波轮洗→脱水，只需在每次切换模式前插入对应的换筒指令\n- 浊度检测可放在任意位置，用于判断是否需要重洗\n- 建议洗涤完成后追加CMD_UV_STERILIZE(val=1,time=60)进行桶杀菌。用户说\"杀菌久一点\"可time=120，\"不要紫外\"可不追加或val=0\n- pipeline末尾应以CMD_TILT_BARREL(val=0)结束（归零复位），若遗漏系统会自动补上\n\n";
        sysPrompt += "【任务指引】：\n1. 洗涤指令：根据用户意图编排完整洗涤流程。speech_reply限30字内，口语化（好嘞、没问题、交给我吧）。\n2. 偏好修改：将新习惯融合，在updated_preferences输出最新摘要。无新偏好输出\"\"。\n3. 日常闲聊：若仅打招呼或闲聊，speech_reply必须极简(15字内)，语气活泼俏皮带点撒娇（呀、呢、哦）。此时pipeline为[]。";

        JsonObject sysMsg = msgs.add<JsonObject>();
        sysMsg["role"] = "system"; sysMsg["content"] = sysPrompt;
        JsonObject userMsg = msgs.add<JsonObject>();
        userMsg["role"] = "user";
        JsonArray contentArr = userMsg["content"].to<JsonArray>();
        JsonObject audioObj = contentArr.add<JsonObject>();
        audioObj["type"] = "input_audio"; audioObj["input_audio"]["data"] = "###AUDIO_BASE64_PLACE_HOLDER###";
        JsonObject textObj = contentArr.add<JsonObject>();
        textObj["type"] = "text"; textObj["text"] = "请解析录音指令并编排洗涤程序。";

        JsonObject tool = doc["tools"].to<JsonArray>().add<JsonObject>();
        tool["type"] = "function";
        JsonObject func = tool["function"].to<JsonObject>();
        func["name"] = "generate_wash_pipeline";
        JsonObject params = func["parameters"].to<JsonObject>();
        params["type"] = "object";
        JsonObject props = params["properties"].to<JsonObject>();
        props["speech_reply"]["type"] = "string";
        props["updated_preferences"]["type"] = "string";
        props["pipeline"]["type"] = "array";
        props["pipeline"]["items"]["type"] = "object";
        JsonObject items_props = props["pipeline"]["items"]["properties"].to<JsonObject>();
        items_props["cmd"]["type"] = "string"; items_props["val"]["type"] = "integer"; items_props["time"]["type"] = "integer";
        params["required"].to<JsonArray>().add("speech_reply");
        params["required"].to<JsonArray>().add("updated_preferences");
        params["required"].to<JsonArray>().add("pipeline");

        // 4. 序列化 + Base64 拼合
        size_t skeletonSize = measureJson(doc) + 1024;
        char* skeletonJson = (char*)heap_caps_malloc(skeletonSize, MALLOC_CAP_SPIRAM);
        _skeletonJson = skeletonJson;
        size_t skWritten = serializeJson(doc, skeletonJson, skeletonSize);
        if (skWritten >= skeletonSize - 1) { Serial.printf("❌ skeletonJson truncated: %u/%u\n", skWritten, skeletonSize); break; }
        const char* placeholder = "\"###AUDIO_BASE64_PLACE_HOLDER###\"";
        char* pos = strstr(skeletonJson, placeholder);

        if (pos) {
            size_t prefix_len_json = pos - skeletonJson;
            size_t suffix_len = strlen(pos + strlen(placeholder));
            size_t finalLen = prefix_len_json + 1 + fullAudioLen + 1 + suffix_len;
            size_t finalSize = finalLen + 1;
            char* finalJson = (char*)heap_caps_malloc(finalSize, MALLOC_CAP_SPIRAM);
            if (finalJson) {
                // memcpy 拼接，避免 215KB strcat 线性扫描
                size_t fl = 0;
                memcpy(finalJson + fl, skeletonJson, prefix_len_json); fl += prefix_len_json;
                finalJson[fl++] = '"';
                memcpy(finalJson + fl, full_base64_str, fullAudioLen); fl += fullAudioLen;
                finalJson[fl++] = '"';
                memcpy(finalJson + fl, pos + strlen(placeholder), suffix_len); fl += suffix_len;
                finalJson[fl] = '\0';
                if (fl != finalLen) {
                    Serial.printf("❌ finalJson length mismatch: fl=%u expected=%u\n", fl, finalLen);
                    heap_caps_free(finalJson); break;
                }
                // 完整性检查
                bool integrityOk = heap_caps_check_integrity_all(true);
                if (DEBUG_VERBOSE_CLOUD) {
                    Serial.printf("[JSON_CHECK] finalLen=%u, finalSize=%u, heap=%s\n", finalLen, finalSize, integrityOk ? "OK" : "BROKEN");
                    char* audioPtr = strstr(finalJson, "data:audio/wav;base64,");
                    if (audioPtr) {
                        size_t offset = audioPtr - finalJson;
                        char* b64Start = audioPtr + audioPrefixLen;
                        Serial.printf("[JSON_CHECK] audio_prefix_offset=%u\n", offset);
                        Serial.print("[JSON_CHECK] b64_first16=");
                        for (int i = 0; i < 16 && b64Start[i]; i++) Serial.print(b64Start[i]);
                        Serial.println();
                        char* b64End = full_base64_str + fullAudioLen - 16;
                        Serial.print("[JSON_CHECK] b64_last16=");
                        for (int i = 0; i < 16 && b64End[i]; i++) Serial.print(b64End[i]);
                        Serial.println();
                    } else {
                        Serial.println("[JSON_CHECK] ❌ audio prefix NOT found!");
                    }
                    Serial.flush();
                }
                if (DEBUG_VERBOSE_CLOUD) memlog("JSON拼完后");

                Serial.printf("🌐 发起云端请求... JSON=%u字节\n", finalLen);

                SetAiPhase(AI_CLOUD_UPLOADING);
                // 发送 headers
                client.printf("POST /v1/chat/completions HTTP/1.1\r\n");
                client.printf("Host: api.xiaomimimo.com\r\n");
                client.printf("Content-Type: application/json\r\n");
                client.printf("Accept-Encoding: identity\r\n");
                client.printf("api-key: %s\r\n", apiKey);
                client.printf("Content-Length: %u\r\n", finalLen);
                client.printf("Connection: close\r\n");
                client.printf("\r\n");
                if (DEBUG_VERBOSE_CLOUD) memlog("headers发送后");

                // 分块发送 body (1024字节/块，通过 INTERNAL staging buffer)
                if (DEBUG_VERBOSE_CLOUD) {
                    bool heapOkBefore = heap_caps_check_integrity_all(true);
                    Serial.printf("[BODY] heap before send: %s\n", heapOkBefore ? "OK" : "BROKEN");
                    Serial.flush();
                }
                const size_t CHUNK = 1024;
                uint8_t* txbuf = (uint8_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!txbuf) { Serial.println("❌ txbuf INTERNAL分配失败!"); heap_caps_free(finalJson); break; }
                size_t sent = 0;
                unsigned long bodyStart = millis();
                while (sent < finalLen) {
                    if (millis() - bodyStart > 30000) { Serial.println("❌ body timeout 30s"); break; }
                    size_t n = min(CHUNK, finalLen - sent);
                    memcpy(txbuf, finalJson + sent, n);
                    size_t w = client.write(txbuf, n);
                    if (w == 0) {
                        Serial.printf("❌ body write=0 at %u/%u\n", sent, finalLen);
                        break;
                    }
                    sent += w;
                    if (DEBUG_VERBOSE_CLOUD && sent % 32768 < CHUNK) Serial.printf("  body %u/%u\n", sent, finalLen);
                    vTaskDelay(1);
                }
                heap_caps_free(txbuf);
                if (DEBUG_VERBOSE_CLOUD) {
                    bool heapOkAfter = heap_caps_check_integrity_all(true);
                    Serial.printf("[BODY] heap after send: %s\n", heapOkAfter ? "OK" : "BROKEN");
                    Serial.flush();
                }

                if (sent != finalLen) {
                    Serial.printf("❌ body incomplete: %u/%u\n", sent, finalLen);
                    shared_cloud_error = CLOUD_NET_ERROR;
                    heap_caps_free(finalJson);
                    client.stop();
                    break;
                }
                if (DEBUG_VERBOSE_CLOUD) memlog("body发送后");
                Serial.printf("📡 body sent %u bytes in %lu ms\n", sent, millis() - bodyStart);

                // 读取响应
                SetAiPhase(AI_CLOUD_WAITING_RESPONSE);
                int httpCode = -1;
                char* respBody = nullptr;
                size_t respLen = 0;
                bool gotResponse = readHttpResponseLimited(client, httpCode, respBody, respLen);
                if (DEBUG_VERBOSE_CLOUD) memlog("response读取后");
                Serial.printf("📡 HTTP状态: %d, body=%u字节\n", httpCode, respLen);

                if (gotResponse && httpCode == 200 && respBody) {
                    SetAiPhase(AI_CLOUD_PARSING);
                    Serial.printf("[AI_PARSE] body_len=%u\n", (unsigned)respLen);
                    if (respLen > 0) {
                        size_t preview = respLen < 800 ? respLen : 800;
                        Serial.printf("[AI_PARSE] body_preview(%u): %.*s\n", (unsigned)preview, (int)preview, respBody);
                    }
                    JsonDocument resDoc(&mySpiRamAlloc);
                    DeserializationError error = deserializeJson(resDoc, respBody, respLen);
                    if (!error) {
                        const char* argsStr = resDoc["choices"][0]["message"]["tool_calls"][0]["function"]["arguments"];
                        Serial.printf("[AI_PARSE] argsStr=%s, len=%u\n", argsStr ? "found" : "NULL", argsStr ? (unsigned)strlen(argsStr) : 0);
                        if (argsStr) {
                            JsonDocument argsDoc(&mySpiRamAlloc);
                            DeserializationError argsErr = deserializeJson(argsDoc, argsStr);
                            if (argsErr) {
                                Serial.printf("[AI_PARSE_WARN] arguments JSON parse failed: %s\n", argsErr.c_str());
                            }
                            const char* new_pref = argsDoc["updated_preferences"];
                            if (new_pref && strlen(new_pref) > 2) { Preferences w; w.begin("xiaojing",false); w.putString("user_pref",new_pref); w.end(); }
                            const char* ai_reply = argsDoc["speech_reply"];
                            Serial.printf("[AI_PARSE] speech_reply=%s\n", ai_reply ? ai_reply : "(null)");
                            if (ai_reply) shared_tts_reply = strdup(ai_reply);
                            JsonArray pipeline = argsDoc["pipeline"];
                            Serial.printf("[AI_PARSE] pipeline.size=%u, looking for field 'pipeline' (array of {cmd,val,time})\n", (unsigned)pipeline.size());
                            if (pipeline.size() > 0) {
                                Serial.println("\n========== 小净 AI 规划洗涤序列 ==========");
                                // 延迟执行：先缓存步骤，TTS 播完后再推入执行器
                                shared_deferred_count = 0;
                                shared_deferred_steps[shared_deferred_count++] = {CMD_HOME_BLDC, 0, 0};
                                bool lastIsHome = false;
                                int step_idx = 0;
                                for (JsonObject step : pipeline) {
                                    WashStep_t nextStep; nextStep.cmd_id = CMD_NONE;
                                    const char* cmd_str = step["cmd"];
                                    Serial.printf("  [%d] cmd=%s val=%d time=%d\n", step_idx,
                                        cmd_str ? cmd_str : "(null)", (int)(step["val"]|0), (int)(step["time"]|0));
                                    if (cmd_str) {
                                        if (strcmp(cmd_str,"CMD_WATER_IN")==0) nextStep.cmd_id=CMD_WATER_IN;
                                        else if (strcmp(cmd_str,"CMD_WASH_PULSATOR")==0) nextStep.cmd_id=CMD_WASH_PULSATOR;
                                        else if (strcmp(cmd_str,"CMD_WASH_DRUM")==0) nextStep.cmd_id=CMD_WASH_DRUM;
                                        else if (strcmp(cmd_str,"CMD_SPIN_DRY")==0) nextStep.cmd_id=CMD_SPIN_DRY;
                                        else if (strcmp(cmd_str,"CMD_TILT_BARREL")==0) nextStep.cmd_id=CMD_TILT_BARREL;
                                        else if (strcmp(cmd_str,"CMD_TURBIDITY_CHECK")==0) nextStep.cmd_id=CMD_TURBIDITY_CHECK;
                                        else if (strcmp(cmd_str,"CMD_UV_STERILIZE")==0) nextStep.cmd_id=CMD_UV_STERILIZE;
                                        else if (strcmp(cmd_str,"CMD_HOME_BLDC")==0) { Serial.printf("⚠️ 忽略AI的BLDC归零\n"); continue; }
                                        else { Serial.printf("[AI_PARSE_WARN] unknown cmd: %s\n", cmd_str); }
                                    }
                                    if (nextStep.cmd_id != CMD_NONE) {
                                        nextStep.val = step["val"]|0; nextStep.time = step["time"]|0;
                                        if (shared_deferred_count < MAX_DEFERRED_STEPS) {
                                            shared_deferred_steps[shared_deferred_count++] = nextStep;
                                        }
                                        lastIsHome = (nextStep.cmd_id == CMD_TILT_BARREL && nextStep.val == 0);
                                    } else {
                                        Serial.printf("[AI_PARSE_WARN] fallback: step[%d] skipped (cmd_id=NONE)\n", step_idx);
                                    }
                                    step_idx++;
                                }
                                if (!lastIsHome && shared_deferred_count < MAX_DEFERRED_STEPS) {
                                    shared_deferred_steps[shared_deferred_count++] = {CMD_TILT_BARREL, 0, 0};
                                    Serial.println("🛡️ [兜底] 自动追加归零复位");
                                }
                                // 默认洗后 UV 杀菌：如果 AI 没有提供 CMD_UV_STERILIZE，则追加 60 秒
                                bool hasUvStep = false;
                                for (int i = 0; i < shared_deferred_count; i++) {
                                    if (shared_deferred_steps[i].cmd_id == CMD_UV_STERILIZE) {
                                        hasUvStep = true;
                                        break;
                                    }
                                }
                                if (!hasUvStep && shared_deferred_count > 0 && shared_deferred_count < MAX_DEFERRED_STEPS - 1) {
                                    // 在归零复位之前插入 UV 杀菌
                                    // 找到归零复位的位置（最后一个 step）
                                    int insertIdx = shared_deferred_count;
                                    if (shared_deferred_steps[shared_deferred_count - 1].cmd_id == CMD_TILT_BARREL &&
                                        shared_deferred_steps[shared_deferred_count - 1].val == 0) {
                                        insertIdx = shared_deferred_count - 1;
                                    }
                                    // 后移归零复位
                                    shared_deferred_steps[shared_deferred_count] = shared_deferred_steps[insertIdx];
                                    shared_deferred_steps[insertIdx] = {CMD_UV_STERILIZE, 1, 60};
                                    shared_deferred_count++;
                                    Serial.println("[UV] post-wash sanitize appended 60s");
                                } else if (hasUvStep) {
                                    Serial.println("[UV] skipped default sanitize because AI provided UV step");
                                }
                                Serial.printf("[AI_FLOW] pipeline parsed, %d steps deferred until TTS done\n", shared_deferred_count);
                                Serial.println("========================================");
                            } else {
                                Serial.println("[AI_PARSE_WARN] washing sequence empty (pipeline.size=0)");
                                shared_is_pure_pref = (argsDoc["updated_preferences"].as<String>().length() > 2);
                                shared_deferred_count = 0;
                            }
                        } else {
                            Serial.println("[AI_PARSE_WARN] tool_calls[0].function.arguments not found");
                        }
                        shared_ai_result_ready = true;
                        SetAiPhase(AI_RESULT_READY);
                    } else {
                        Serial.printf("[AI_PARSE_WARN] top-level JSON parse failed: %s\n", error.c_str());
                        shared_cloud_error = CLOUD_JSON_ERROR; shared_ai_result_ready = false; SetAiPhase(AI_FAILED);
                    }
                } else if (httpCode == 401 || httpCode == 403) {
                    shared_cloud_error = CLOUD_NET_ERROR; shared_ai_result_ready = false; SetAiPhase(AI_FAILED);
                } else {
                    shared_cloud_error = CLOUD_API_TIMEOUT; shared_ai_result_ready = false; SetAiPhase(AI_FAILED);
                }
                if (respBody) heap_caps_free(respBody);
                heap_caps_free(finalJson);
                client.stop();
                if (DEBUG_VERBOSE_CLOUD) memlog("client.stop后");
            }
        }
    } while(0);

    // 统一清理（所有 break 路径都走到这里）
    if (_skeletonJson) { heap_caps_free(_skeletonJson); _skeletonJson = nullptr; }
    if (_fullBase64) { heap_caps_free(_fullBase64); _fullBase64 = nullptr; }
    if (shared_pcm_buf) {
        Serial.printf("[CLOUD_CLEANUP] releasing pending PCM: %u bytes\n", (unsigned)shared_pcm_len);
        heap_caps_free(shared_pcm_buf);
        shared_pcm_buf = nullptr;
    }
    shared_pcm_len = 0;
    esp_task_wdt_add(idle_0);
    memlog("cloud-task-exit");

    Serial.println("🏁 [Cloud] 交互解析完毕。");
    cloud_task_done = true;
    vTaskDelete(NULL);
}

// ==========================================
// 诊断测试 A: 纯 TLS 连接测试
// ==========================================
void testTlsOnly() {
    Serial.println("\n========== TEST A: TLS ONLY ==========");
    memlog("tls-only before");
    WiFiClientSecure client;
    memlog("tls-only after client construct");
    client.setInsecure();
    memlog("tls-only after setInsecure");
    client.setTimeout(20000);
    memlog("tls-only before connect");
    bool ok = client.connect("api.xiaomimimo.com", 443);
    memlog("tls-only after connect");
    Serial.printf("TLS connect result=%d\n", ok);
    client.stop();
    memlog("tls-only after stop");
    Serial.println("========== TEST A DONE ==========\n");
}

// ==========================================
// 诊断测试 B: 小 payload 手写 HTTPS POST
// ==========================================
void testHttpsSmallManualPost() {
    Serial.println("\n========== TEST B: SMALL HTTPS POST ==========");
    const char* body = "{\"model\":\"mimo-v2.5\",\"messages\":[{\"role\":\"user\",\"content\":\"hello\"}]}";
    size_t bodyLen = strlen(body);

    memlog("small-post before");
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);
    memlog("small-post before connect");
    bool ok = client.connect("api.xiaomimimo.com", 443);
    memlog("small-post after connect");
    Serial.printf("TLS connect result=%d\n", ok);
    if (!ok) { Serial.println("========== TEST B DONE (connect fail) ==========\n"); return; }

    client.printf("POST /v1/chat/completions HTTP/1.1\r\n");
    client.printf("Host: api.xiaomimimo.com\r\n");
    client.printf("Content-Type: application/json\r\n");
    client.printf("Content-Length: %d\r\n", bodyLen);
    client.printf("Connection: close\r\n");
    client.printf("\r\n");
    Serial.println("headers sent");

    size_t written = client.write((const uint8_t*)body, bodyLen);
    Serial.printf("body write returned=%u (expected=%u)\n", written, bodyLen);
    memlog("small-post after write");

    uint32_t t0 = millis();
    while (!client.available() && millis() - t0 < 15000) { vTaskDelay(pdMS_TO_TICKS(10)); }
    Serial.printf("waited %lu ms for response\n", millis() - t0);

    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        statusLine.trim();
        Serial.printf("HTTP status: %s\n", statusLine.c_str());
        // skip headers
        while (client.available()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line.length() == 0) break;
        }
        // read up to 1024 bytes of body
        char respBuf[1025];
        int respLen = client.read((uint8_t*)respBuf, 1024);
        if (respLen > 0) {
            respBuf[respLen] = '\0';
            Serial.printf("response (%d bytes): %.512s\n", respLen, respBuf);
        }
    } else {
        Serial.println("no response (timeout)");
    }
    client.stop();
    memlog("small-post done");
    Serial.println("========== TEST B DONE ==========\n");
}

// ==========================================
// 诊断测试 C: 大 payload 分块写入（明文 HTTP）
// ==========================================
void testHttpLargeChunkWriteOnly() {
    Serial.println("\n========== TEST C: LARGE CHUNK HTTP WRITE ==========");
    size_t fakeLen = 215793;
    char* fakeJson = (char*)heap_caps_malloc(fakeLen + 1, MALLOC_CAP_SPIRAM);
    if (!fakeJson) { Serial.println("PSRAM alloc failed!"); return; }
    memset(fakeJson, 'A', fakeLen);
    fakeJson[0] = '{'; fakeJson[fakeLen - 1] = '}'; fakeJson[fakeLen] = '\0';

    memlog("large-write before");
    WiFiClient client;
    memlog("large-write before connect");
    bool ok = client.connect("api.xiaomimimo.com", 80);
    memlog("large-write after connect");
    Serial.printf("HTTP connect result=%d\n", ok);
    if (!ok) { heap_caps_free(fakeJson); Serial.println("========== TEST C DONE (connect fail) ==========\n"); return; }

    client.printf("POST /v1/chat/completions HTTP/1.1\r\n");
    client.printf("Host: api.xiaomimimo.com\r\n");
    client.printf("Content-Length: %u\r\n", fakeLen);
    client.printf("Connection: close\r\n");
    client.printf("\r\n");

    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < fakeLen) {
        if (millis() - t0 > 60000) { Serial.println("timeout 60s"); break; }
        size_t toSend = min((size_t)4096, fakeLen - sent);
        size_t w = client.write((uint8_t*)(fakeJson + sent), toSend);
        if (w == 0) { Serial.printf("write=0 at %u/%u\n", sent, fakeLen); break; }
        sent += w;
        if (sent % 32768 < 4096) Serial.printf("sent %u/%u\n", sent, fakeLen);
    }
    Serial.printf("final sent=%u/%u in %lu ms\n", sent, fakeLen, millis() - t0);
    memlog("large-write after");
    client.stop();
    heap_caps_free(fakeJson);
    Serial.println("========== TEST C DONE ==========\n");
}

// ==========================================
// 诊断测试 D: 大 payload 手写 HTTPS POST（复用真实 base64 + JSON 流程）
// ==========================================
static const size_t HTTPS_CHUNK = 1024;  // 每次只改这个值: 1024 / 2048 / 4096

void testHttpsLargeManualChunkPost() {
    Serial.println("\n========== TEST D: LARGE HTTPS MANUAL CHUNK POST ==========");
    Serial.printf("CHUNK=%u\n", HTTPS_CHUNK);
    memlog("https-large start");

    // 1. 构造 WAV + base64（复用 shared_pcm_buf，如果没有就用假数据）
    size_t pcm_len = shared_pcm_len;
    uint8_t* pcm_buf = shared_pcm_buf;
    bool usingFakePcm = false;
    if (!pcm_buf || pcm_len == 0) {
        Serial.println("⚠️ shared_pcm_buf 为空，使用 160KB 假 PCM 数据");
        pcm_len = 160000;
        pcm_buf = (uint8_t*)heap_caps_malloc(pcm_len, MALLOC_CAP_SPIRAM);
        if (!pcm_buf) { Serial.println("❌ PSRAM alloc failed"); return; }
        memset(pcm_buf, 0, pcm_len);
        usingFakePcm = true;
    }

    uint32_t wav_size = 44 + pcm_len;
    uint8_t* wav_buf = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
    if (!wav_buf) { Serial.println("❌ wav_buf alloc failed"); if (usingFakePcm) heap_caps_free(pcm_buf); return; }
    generateWavHeader(wav_buf, pcm_len);
    memcpy(wav_buf + 44, pcm_buf, pcm_len);
    if (usingFakePcm) heap_caps_free(pcm_buf);

    const char* prefix = "data:audio/wav;base64,";
    size_t prefix_len = strlen(prefix);
    size_t base64_len = 4 * ((wav_size + 2) / 3);
    char* full_base64_str = (char*)heap_caps_malloc(prefix_len + base64_len + 1, MALLOC_CAP_SPIRAM);
    if (!full_base64_str) { heap_caps_free(wav_buf); Serial.println("❌ base64 alloc failed"); return; }

    strcpy(full_base64_str, prefix);
    size_t olen = 0;
    mbedtls_base64_encode((unsigned char*)(full_base64_str + prefix_len), base64_len + 1, &olen, wav_buf, wav_size);
    full_base64_str[prefix_len + olen] = '\0';
    heap_caps_free(wav_buf);
    memlog("base64 后");

    // 2. 构造完整 JSON（复用 cloudNetworkTask 的 JSON 结构）
    Preferences prefs; prefs.begin("xiaojing", true);
    String history_pref = prefs.getString("user_pref", "无特殊偏好");
    prefs.end();

    JsonDocument doc(&mySpiRamAlloc);
    doc["model"] = "mimo-v2.5";
    JsonArray msgs = doc["messages"].to<JsonArray>();

    String sysPrompt = "你是智能洗衣管家小净。严禁Emoji。必须调用工具生成洗涤pipeline。\n";
    sysPrompt += "【用户历史偏好】：" + history_pref + "\n\n";
    sysPrompt += "【系统自动处理】：BLDC内筒归零、归零复位\n\n";
    sysPrompt += "【指令】：CMD_WATER_IN, CMD_WASH_PULSATOR, CMD_WASH_DRUM, CMD_SPIN_DRY, CMD_TILT_BARREL, CMD_TURBIDITY_CHECK, CMD_UV_STERILIZE\n";
    sysPrompt += "【任务】：根据用户意图编排洗涤流程。speech_reply限30字内。";

    JsonObject sysMsg = msgs.add<JsonObject>();
    sysMsg["role"] = "system"; sysMsg["content"] = sysPrompt;
    JsonObject userMsg = msgs.add<JsonObject>();
    userMsg["role"] = "user";
    JsonArray contentArr = userMsg["content"].to<JsonArray>();
    JsonObject audioObj = contentArr.add<JsonObject>();
    audioObj["type"] = "input_audio"; audioObj["input_audio"]["data"] = "###AUDIO_BASE64_PLACE_HOLDER###";
    JsonObject textObj = contentArr.add<JsonObject>();
    textObj["type"] = "text"; textObj["text"] = "请解析录音指令并编排洗涤程序。";

    JsonObject tool = doc["tools"].to<JsonArray>().add<JsonObject>();
    tool["type"] = "function";
    JsonObject func = tool["function"].to<JsonObject>();
    func["name"] = "generate_wash_pipeline";
    JsonObject params = func["parameters"].to<JsonObject>();
    params["type"] = "object";
    JsonObject props = params["properties"].to<JsonObject>();
    props["speech_reply"]["type"] = "string";
    props["updated_preferences"]["type"] = "string";
    props["pipeline"]["type"] = "array";
    props["pipeline"]["items"]["type"] = "object";
    JsonObject items_props = props["pipeline"]["items"]["properties"].to<JsonObject>();
    items_props["cmd"]["type"] = "string"; items_props["val"]["type"] = "integer"; items_props["time"]["type"] = "integer";
    params["required"].to<JsonArray>().add("speech_reply");
    params["required"].to<JsonArray>().add("updated_preferences");
    params["required"].to<JsonArray>().add("pipeline");

    size_t skeletonSize = measureJson(doc) + 1024;
    char* skeletonJson = (char*)heap_caps_malloc(skeletonSize, MALLOC_CAP_SPIRAM);
    serializeJson(doc, skeletonJson, skeletonSize);
    const char* placeholder = "\"###AUDIO_BASE64_PLACE_HOLDER###\"";
    char* pos = strstr(skeletonJson, placeholder);

    if (!pos) { Serial.println("❌ placeholder not found"); heap_caps_free(skeletonJson); heap_caps_free(full_base64_str); return; }

    size_t prefix_len_json = pos - skeletonJson;
    size_t finalSize = prefix_len_json + strlen(full_base64_str) + 2 + strlen(pos + strlen(placeholder)) + 1;
    char* finalJson = (char*)heap_caps_malloc(finalSize, MALLOC_CAP_SPIRAM);
    if (!finalJson) { heap_caps_free(skeletonJson); heap_caps_free(full_base64_str); return; }

    size_t fl = 0;
    memcpy(finalJson + fl, skeletonJson, prefix_len_json); fl += prefix_len_json;
    finalJson[fl++] = '"';
    size_t b64_actual = strlen(full_base64_str);
    memcpy(finalJson + fl, full_base64_str, b64_actual); fl += b64_actual;
    finalJson[fl++] = '"';
    size_t suffix_len = strlen(pos + strlen(placeholder));
    memcpy(finalJson + fl, pos + strlen(placeholder), suffix_len); fl += suffix_len;
    finalJson[fl] = '\0';
    size_t finalLen = fl;

    heap_caps_free(skeletonJson);
    heap_caps_free(full_base64_str);
    memlog("JSON 拼完后");
    Serial.printf("finalLen=%u\n", finalLen);

    // 3. TLS 连接
    memlog("TLS connect 前");
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);
    bool ok = client.connect("api.xiaomimimo.com", 443);
    memlog("TLS connect 后");
    Serial.printf("TLS connect=%d\n", ok);
    if (!ok) { heap_caps_free(finalJson); client.stop(); Serial.println("========== TEST D DONE (TLS fail) ==========\n"); return; }

    // 4. 发送 headers
    client.printf("POST /v1/chat/completions HTTP/1.1\r\n");
    client.printf("Host: api.xiaomimimo.com\r\n");
    client.printf("Content-Type: application/json\r\n");
    client.printf("api-key: %s\r\n", apiKey);
    client.printf("Content-Length: %u\r\n", finalLen);
    client.printf("Connection: close\r\n");
    client.printf("\r\n");
    memlog("headers 发送后");

    // 5. 分块发送 body
    memlog("body 开始前");
    size_t sent = 0;
    uint32_t t0 = millis();
    while (sent < finalLen) {
        if (millis() - t0 > 120000) { Serial.println("❌ body timeout 120s"); break; }
        size_t n = min(HTTPS_CHUNK, finalLen - sent);
        size_t w = client.write((const uint8_t*)finalJson + sent, n);
        if (w == 0) {
            Serial.printf("❌ body write=0 at %u/%u\n", sent, finalLen);
            memlog("https-large write failed");
            break;
        }
        sent += w;
        if (sent % 16384 < HTTPS_CHUNK) {
            Serial.printf("https sent %u/%u  INT free=%u largest=%u PSRAM=%u\n",
                sent, finalLen,
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
        vTaskDelay(1);
    }
    Serial.printf("body done: sent=%u/%u in %lu ms\n", sent, finalLen, millis() - t0);
    memlog("body 发送完成后");

    // 6. 读取响应（只读状态行 + 前 2048 字节）
    uint32_t t1 = millis();
    while (!client.available() && millis() - t1 < 30000) { vTaskDelay(pdMS_TO_TICKS(10)); }

    if (client.available()) {
        String statusLine = client.readStringUntil('\n');
        statusLine.trim();
        Serial.printf("HTTP status: %s\n", statusLine.c_str());
        while (client.available()) {
            String line = client.readStringUntil('\n');
            if (line == "\r" || line.length() == 0) break;
        }
        char respBuf[2049];
        int respLen = client.read((uint8_t*)respBuf, 2048);
        if (respLen > 0) {
            respBuf[respLen] = '\0';
            Serial.printf("response (%d bytes): %.1024s\n", respLen, respBuf);
        }
    } else {
        Serial.println("no response (timeout)");
    }
    memlog("response 读取后");

    // 7. 清理
    heap_caps_free(finalJson);
    client.stop();
    memlog("client.stop 后");
    Serial.println("========== TEST D DONE ==========\n");
}

// ==========================================
// 🔬 BLDC 正反转自动诊断（临时测试）
// ==========================================
#if DEBUG_AUTO_BLDC_DIRECTION_DIAG
void testBLDCDirectionAutoDiag() {
    Serial.println("\n========================================");
    Serial.println("[BLDC_DIAG] auto diag start");
    Serial.println("========================================\n");

    // 测试序列：(描述, ZF方向函数, speed, 持续ms)
    struct Step {
        const char* desc;
        bool isCW;       // true=CW, false=CCW
        uint8_t speed;   // 0=brake
        uint32_t durMs;
    };

    // 先 brake 稳定 2s，再依次测试 CW 低速→CW 高速→长刹车→CCW 低速→CCW 高速→收尾
    Step steps[] = {
        {"brake+prep CW",  true,   0, 2000},
        {"CW 25%",         true,  25, 3000},
        {"brake+prep CW",  true,   0, 1000},
        {"CW 50%",         true,  50, 5000},
        {"brake+prep CCW", false,  0, 3000},
        {"CCW 25%",        false, 25, 3000},
        {"brake+prep CCW", false,  0, 1000},
        {"CCW 50%",        false, 50, 5000},
        {"brake+prep CW",  true,   0, 2000},
        {"final brake",    true,   0, 1000},
    };
    int numSteps = sizeof(steps) / sizeof(steps[0]);

    for (int i = 0; i < numSteps; i++) {
        Step& s = steps[i];

        // 执行控制
        if (s.speed == 0) {
            // brake + 预置下一方向
            if (s.isCW) washerMotor.brakeThenCW();
            else        washerMotor.brakeThenCCW();
        } else {
            if (s.isCW) washerMotor.runCW(s.speed);
            else        washerMotor.runCCW(s.speed);
        }

        // 读取当前真实状态
        uint8_t zfRead = digitalRead(BLDC_ZF_PIN);
        uint8_t duty = map(s.speed, 0, 100, 0, 255);

        Serial.printf("[BLDC_DIAG] step=%d/%d desc=\"%s\" zf=%s speed=%d duty=%d duration=%lums\n",
            i + 1, numSteps, s.desc,
            zfRead ? "HIGH" : "LOW",
            s.speed, duty, (unsigned long)s.durMs);

        // 等待（允许使用 delay，这是临时诊断）
        delay(s.durMs);
    }

    washerMotor.brake();
    Serial.println("\n========================================");
    Serial.println("[BLDC_DIAG] auto diag done — motor braked");
    Serial.println("========================================\n");
}
#endif // DEBUG_AUTO_BLDC_DIRECTION_DIAG

// ==========================================
// Setup 初始化
// ==========================================
void setup() {
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_SHARED);
    Serial.begin(115200);
    delay(1000);

#if DEBUG_AUTO_BLDC_DIRECTION_DIAG
    Serial.println("[BLDC_DIAG] minimal init for BLDC direction test...");
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    EXE_Init();  // 内部调用 DRV_Init()，初始化 BLDC PWM + 方向引脚
    delay(3000); // 等待电源和串口稳定
    testBLDCDirectionAutoDiag();
    Serial.println("[BLDC_DIAG] setup done — loop() is no-op, reset board to re-run");
    return;      // 跳过所有正常初始化
#endif

    // 芯片信息
    Serial.printf("🔧 Chip: %s, PSRAM: %luKB, FreePSRAM: %luKB, Heap: %luKB, FreeHeap: %luKB\n",
        ESP.getChipModel(), ESP.getPsramSize()/1024, ESP.getFreePsram()/1024,
        ESP.getHeapSize()/1024, ESP.getFreeHeap()/1024);
    memlog("boot");

    // ⚠️ I2S 必须在 BLE 之前初始化！BLE 会抢占 DMA 内存
    i2s_combined_init();

    EXE_Init();

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    Wire.setClock(100000);
    mcp_write_register(0x00, 0x00); mcp_write_register(0x01, 0x7F);
    setAmplifierState(false);

    DRV_GestureInit();

    // 🚀 屏幕显示初始化
    DISP_Init();

    // 🚀 WiFi先启动（web服务器需要足够堆内存）
    Serial.print("🌐 连接WiFi...");
    configureReliableDns();
    WiFi.begin(ssid, password);
    uint32_t wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 10000) {
        delay(500);
        Serial.print(".");
    }

    // 🚀 BLE初始化（NimBLE，内存占用更小）
    Serial.printf("📊 BLE前: 内部堆=%dKB\n", ESP.getFreeHeap()/1024);
    if (!TLS_TEST_DISABLE_BLE) {
        BLE_Init();
    } else {
        Serial.println("[TLS_TEST] BLE_Init skipped");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println(" 成功!");
        Serial.printf("WiFi sleep current=%d\n", WiFi.getSleep());

        // 🚀 OTA 升级初始化（依赖WiFi）
        ArduinoOTA.setHostname("xiaojing");
        ArduinoOTA.onStart([]() {
            Serial.println("⬆️ [OTA] 开始升级...");
            EXE_ClearQueue();
        });
        ArduinoOTA.onEnd([]() {
            Serial.println("✅ [OTA] 升级完成，即将重启...");
        });
        ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
            Serial.printf("📊 [OTA] 进度: %u%%\r", (progress / (total / 100)));
        });
        ArduinoOTA.onError([](ota_error_t error) {
            Serial.printf("❌ [OTA] 错误 [%u]: ", error);
            if (error == OTA_AUTH_ERROR) Serial.println("认证失败");
            else if (error == OTA_BEGIN_ERROR) Serial.println("启动失败");
            else if (error == OTA_CONNECT_ERROR) Serial.println("连接失败");
            else if (error == OTA_RECEIVE_ERROR) Serial.println("接收失败");
            else if (error == OTA_END_ERROR) Serial.println("结束失败");
        });
        ArduinoOTA.begin();
        Serial.println("⬆️ [OTA] 已就绪，可通过 Arduino IDE 或 PlatformIO 无线升级");
    } else {
        Serial.println(" 失败！WiFi未连接，BLE仍可使用");
    }

    // CloudTask 改为按需创建（用完自删除），不再常驻 SRAM

    // 网络分层诊断已验证通过，暂不自动运行
    // xTaskCreatePinnedToCore(testNetworkConnectSteps, "NetTest0", 8192, (void*)0, 1, NULL, 0);
    // xTaskCreatePinnedToCore(testNetworkConnectSteps, "NetTest1", 8192, (void*)1, 1, NULL, 1);

    Serial.println("\n🎙️ 麦克风与功放就绪。说『小净小净』唤醒我！");

    // 诊断测试已验证，使用真实语音路径测试
    // 如需单独运行测试，取消下行注释:
    // delay(3000); testHttpsLargeManualChunkPost();
}

// ==========================================
// LOOP: 极速调度与交互循环
// ==========================================
void loop() {
#if DEBUG_AUTO_BLDC_DIRECTION_DIAG
    delay(1000); // 诊断模式：loop 空转，不执行任何业务逻辑
    return;
#endif

    // 💡 0. OTA 升级处理
    ArduinoOTA.handle();

    // 💡 1. 维持底层肌肉心跳与 LED 闪烁
    EXE_Handler();
    DRV_StepperManage();  // FastAccelStepper 脉冲队列填充，必须高频调用
    handleMCPHeartbeat();

    // 洗涤计时器和总步骤
    static uint32_t washStartTime = 0;
    static int totalWashSteps = 0;

    // 💡 1.5 BLE 进度上报（每秒一次）+ 洗涤结束重置计时器
    static uint32_t lastBleUpdate = 0;
    if (millis() - lastBleUpdate > 1000) {
        lastBleUpdate = millis();
        bool isW = EXE_IsWashing();
        if (!isW && washStartTime > 0) { washStartTime = 0; totalWashSteps = 0; }
        if (!TLS_TEST_DISABLE_BLE && BLE_IsConnected()) {
            BLE_SendProgress(isW ? 50 : 0);
        }
    }

    // 💡 1.6 处理BLE指令
    if (!TLS_TEST_DISABLE_BLE) {
    const char* bleCmd = BLE_GetPendingCommand();
    if (bleCmd != NULL) {
        Serial.printf("📨 [BLE] 处理指令: %s\n", bleCmd);

        // 解析JSON指令
        JsonDocument bleDoc;
        DeserializationError error = deserializeJson(bleDoc, bleCmd);

        if (!error) {
            const char* cmd = bleDoc["cmd"] | "";

            if (strcmp(cmd, "start") == 0) {
                // 启动洗涤
                if (!EXE_IsWashing()) {
                    Serial.println("🔵 [BLE] 启动洗涤程序");
                    washStartTime = millis();
                    BLE_SendStatus("启动中");

                    // 清空队列并注入安全指令
                    EXE_ClearQueue();
                    EXE_PushStep({CMD_TILT_BARREL, 0, 0});
                    EXE_PushStep({CMD_HOME_BLDC, 0, 0});

                    // 解析自定义步骤
                    JsonObject steps = bleDoc["steps"];

                    // 进水
                    EXE_PushStep({CMD_WATER_IN, 1, 30});

                    // 波轮洗
                    if (steps["wash"]["enabled"]) {
                        int washCount = steps["wash"]["count"] | 1;
                        int washTime = steps["wash"]["time"] | 5;
                        EXE_PushStep({CMD_WASH_PULSATOR, 2, (uint16_t)(washCount * washTime)});
                    }

                    // 换筒90° + 滚筒洗
                    EXE_PushStep({CMD_TILT_BARREL, 90, 0});
                    EXE_PushStep({CMD_WASH_DRUM, 2, 10});

                    // 换筒180° + 脱水
                    EXE_PushStep({CMD_TILT_BARREL, 180, 0});
                    if (steps["dry"]["enabled"]) {
                        int dryTime = steps["dry"]["time"] | 20;
                        EXE_PushStep({CMD_SPIN_DRY, 2, (uint16_t)(dryTime / 60)});
                    }

                    // 安全收尾
                    EXE_PushStep({CMD_TURBIDITY_CHECK, 0, 0});
                    EXE_PushStep({CMD_TILT_BARREL, 0, 0});

                    totalWashSteps = EXE_GetQueueSize();
                    BLE_SendStatus("洗涤中");
                } else {
                    Serial.println("⚠️ [BLE] 洗涤进行中，忽略启动指令");
                    BLE_SendStatus("忙碌");
                }
            } else if (strcmp(cmd, "stop") == 0) {
                // 停止洗涤
                Serial.println("🛑 [BLE] 停止洗涤");
                EXE_ClearQueue();
                BLE_SendStatus("已停止");
            }
        } else {
            Serial.println("❌ [BLE] JSON解析失败");
        }
    }
    } // TLS_TEST_DISABLE_BLE

    // 🔘 2. 本地交互：根据洗涤状态区分处理
    bool isWashing = EXE_IsWashing();
    bool isOfflineWashTriggered = false;

    // 手势翻页（洗涤/待机状态均可操作）
    Gesture_t gesture = DRV_GetGesture();
    if (gesture == GESTURE_LEFT) {
        DISP_SwitchPage(true);
        Serial.println("👈 [手势] 左挥 → 下一页");
    } else if (gesture == GESTURE_RIGHT) {
        DISP_SwitchPage(false);
        Serial.println("👉 [手势] 右挥 → 上一页");
    } else if (gesture == GESTURE_UP) {
        Serial.println("👆 [手势] 上挥");
    } else if (gesture == GESTURE_DOWN) {
        Serial.println("👇 [手势] 下挥");
    }

    if (isWashing) {
        // ==========================================
        // 洗涤中：接受停止指令
        // ==========================================

        // EC11 按键 → 停止洗涤
        if (DRV_IsEC11ButtonPressed()) {
            Serial.println("🛑 [洗涤中] EC11 按键 → 停止洗涤");
            EXE_ClearQueue();
        }

        // 手势下挥 → 停止洗涤
        if (gesture == GESTURE_DOWN) {
            Serial.println("🛑 [洗涤中] 手势下挥 → 停止洗涤");
            EXE_ClearQueue();
        }
    } else {
        // ==========================================
        // 待机中：接受启动指令
        // ==========================================

        // EC11 按键 → 启动洗涤
        if (DRV_IsEC11ButtonPressed()) {
            Serial.println("🔘 [本地交互] EC11 按键被按下");
            isOfflineWashTriggered = true;
        }

        // 手势上挥 → 启动洗涤
        else if (gesture == GESTURE_UP) {
            Serial.println("👋 [手势交互] 检测到向上挥手");
            isOfflineWashTriggered = true;
        }
    }

    // 如果被任何一种方式触发，执行队列拼接
    if (isOfflineWashTriggered) {
        Serial.println("========== 启动离线【比赛Demo】洗涤流程 ==========");
        washStartTime = millis();
        totalWashSteps = 9; // 比赛demo 9步

        EXE_ClearQueue(); // 紧急清空

        // ==========================================
        // 比赛 Demo 流程：快速展示完整功能
        // ==========================================

        // 【阶段1：安全防线】
        EXE_PushStep({CMD_TILT_BARREL, 0, 0});       // 外筒归零
        EXE_PushStep({CMD_HOME_BLDC, 0, 0});          // BLDC内筒归零
        Serial.println("🛡️ [安全防线] 外筒归零 + BLDC归零");

        // 【阶段2：进水 + UV 快速演示】
        EXE_PushStep({CMD_WATER_IN, 1, 3});           // 进水 + UV 3秒
        Serial.println("💧 [进水] 进水 + UV杀菌快速演示 3秒");

        // 【阶段3：波轮洗 展示正反转】
        EXE_PushStep({CMD_WASH_PULSATOR, 2, 9});      // 1个完整softstart周期
        Serial.println("🌀 [波轮洗] 标准强度 9秒，展示正反转");

        // 【阶段4：换筒90°】
        EXE_PushStep({CMD_TILT_BARREL, 90, 0});       // 换筒到90度
        Serial.println("🔄 [换筒] 90°水平位置");

        // 【阶段5：滚筒洗 展示旋转】
        EXE_PushStep({CMD_WASH_DRUM, 2, 5});          // 约3s达速，展示满速2s
        Serial.println("🔄 [滚筒洗] 标准强度 5秒，展示滚筒旋转");

        // 【阶段6：换筒180°】
        EXE_PushStep({CMD_TILT_BARREL, 180, 0});      // 换筒到180度
        Serial.println("🔄 [换筒] 180°倒立位置");

        // 【阶段7：脱水 展示高速】
        EXE_PushStep({CMD_SPIN_DRY, 2, 8});           // 约5s达速，展示满速3s
        Serial.println("💨 [脱水] 高速模式 8秒，展示高速脱水");

        // 【阶段8：归位复位】
        EXE_PushStep({CMD_TILT_BARREL, 0, 0});        // 回到0°
        Serial.println("🛡️ [安全收尾] 回到0°归位");

        Serial.println("=============================================");
    }

    // 🔄 2.4 WiFi 自动重连（每10秒检测一次）
    static uint32_t lastWifiCheck = 0;
    if (millis() - lastWifiCheck > 10000) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("⚠️ [WiFi] 连接断开，正在重连...");
            WiFi.disconnect();
            configureReliableDns();
            WiFi.begin(ssid, password);
        }
    }

    // 💡 2.5 屏幕显示刷新（每 200ms）
    static uint32_t lastDispUpdate = 0;
    if (millis() - lastDispUpdate > 200) {
        lastDispUpdate = millis();

        bool bleConn = TLS_TEST_DISABLE_BLE ? false : BLE_IsConnected();
        bool wifiConn = (WiFi.status() == WL_CONNECTED);
        bool washing = EXE_IsWashing();

        DISP_UpdateStatus(bleConn, wifiConn, washing);

        // Page 2: 洗涤进度（简化进度计算）
        DrumPosture posture = EXE_GetCurrentPosture();
        int queueSize = EXE_GetQueueSize();
        const char* stepName = washing ? "洗涤中" : "待机";
        int progress = washing ? 50 : 0;  // 简化进度，后续可精确计算
        DISP_UpdateWashProgress(progress, stepName, queueSize, posture, washing);
    }
    DISP_Handler();  // LVGL 定时器处理

    // 🔄 3. 本地交互：EC11 旋钮丝滑监听
    static long lastPrintedEncoderVal = 0;
    long currentEncoderVal = DRV_GetEncoderValue(); 
    
    if (currentEncoderVal != lastPrintedEncoderVal) {
        Serial.printf("[外设] EC11 旋钮转动 | 当前纯净逻辑格数: %ld\n", currentEncoderVal);
        lastPrintedEncoderVal = currentEncoderVal;
    }

    // 🎙️ 4. 语音神经中枢
    switch (currentState) {
        case SYS_WAKE_DETECT: {
            // 洗涤中禁用语音唤醒，防止噪音误唤醒
            if (isPlaying || EXE_IsWashing()) break;

            // 按需分配 audio_buffer 到 PSRAM（释放 32KB SRAM）
            if (!audio_buffer) {
                audio_buffer = (int16_t*)heap_caps_malloc(WINDOW_SIZE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (!audio_buffer) { Serial.println("❌ audio_buffer PSRAM分配失败!"); break; }
                buffer_idx = 0;
            }

            int32_t i2s_data[128 * 2]; // 32-bit stereo
            size_t bytes_read = 0;
            if (i2s_read(I2S_PORT, i2s_data, sizeof(i2s_data), &bytes_read, 0) == ESP_OK && bytes_read > 0) {
                int samples = bytes_read / 8; // 8 bytes per stereo frame
                for (int i = 0; i < samples; i++) {
                    int32_t amp = (i2s_data[i * 2] >> 14) << 3;
                    if (amp > 32767) amp = 32767; else if (amp < -32768) amp = -32768;
                    if (buffer_idx < WINDOW_SIZE) audio_buffer[buffer_idx++] = (int16_t)amp;
                }
            }

            if (buffer_idx >= WINDOW_SIZE) {
                signal_t signal; signal.total_length = WINDOW_SIZE; signal.get_data = &microphone_audio_signal_get_data;
                ei_impulse_result_t result = {0};
                run_classifier(&signal, &result, false);

                // 每秒输出一次实机推理诊断，避免 250ms 滑窗导致串口刷屏。
                static uint32_t last_wake_diag_ms = 0;
                uint32_t wake_diag_now = millis();
                if (wake_diag_now - last_wake_diag_ms >= 1000) {
                    last_wake_diag_ms = wake_diag_now;
                    Serial.printf("[WAKE_AI] dsp=%ums cls=%ums", result.timing.dsp, result.timing.classification);
                    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                        Serial.printf(" %s=%.3f", result.classification[i].label, result.classification[i].value);
                    }
                    Serial.println();
                }

                for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
                    if (strcmp(result.classification[i].label, "xiao_jing") == 0 && result.classification[i].value > 0.8) {
                        Serial.println("✨ [唤醒] 触发唤醒，进入录音模式");
                        Serial.flush();
                        SetAiPhase(AI_WAKE_DETECTED);
                        NotifyAiStatus();
                        DISP_SetVoiceState(VOICE_AWAKE);
                        Serial.println("[WAKE] before wake prompt audio");
                        Serial.flush();
                        if (!TLS_TEST_DISABLE_WAKE_PROMPT_AUDIO) {
                            playBase64Audio(AUDIO_WAKE_UP);
                        } else {
                            Serial.println("[WAKE_TEST] wake prompt audio skipped");
                            Serial.flush();
                        }
                        Serial.println("[WAKE] after wake prompt audio");
                        Serial.flush();
                        memset(audio_buffer, 0, WINDOW_SIZE * sizeof(int16_t));
                        buffer_idx = 0;
                        currentState = SYS_RECORDING;
                        break;
                    }
                }

                if (!isPlaying && currentState == SYS_WAKE_DETECT) {
                    memmove(audio_buffer, audio_buffer + STEP_SIZE, (WINDOW_SIZE - STEP_SIZE) * 2);
                    buffer_idx = WINDOW_SIZE - STEP_SIZE;
                }
            }
            break;
        }

        case SYS_RECORDING: {
            Serial.println("\n[1/3] 🎤 开始智能录音...");
            Serial.printf("[REC] SRAM=%u, PSRAM=%u, core=%d\n", ESP.getFreeHeap(), ESP.getFreePsram(), xPortGetCoreID());
            Serial.flush();
            SetAiPhase(AI_RECORDING);
            NotifyAiStatus();
            DISP_SetVoiceState(VOICE_LISTENING);
            // 释放 audio_buffer（32KB SRAM），给 TLS 握手腾出空间
            if (audio_buffer) { heap_caps_free(audio_buffer); audio_buffer = NULL; }
            shared_pcm_buf = (uint8_t*)heap_caps_malloc(REC_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
            i2s_zero_dma_buffer(I2S_PORT); // 清理残留缓冲，防止爆音干扰
            
            uint32_t start_time = millis();
            uint32_t last_speech_time = millis();
            bool has_spoken = false;
            shared_pcm_len = 0;
            long max_energy = 0; 
            
            float prev_raw = 0;
            float prev_clean = 0;
            bool filter_init = false; 
            
            uint32_t last_print_time = 0;
            
            cloud_task_done = false;
            shared_tts_reply = NULL;
            shared_is_pure_pref = false;
            shared_cloud_error = CLOUD_OK;

            Serial.println("[REC] before start recording");
            Serial.flush();

            while(shared_pcm_len < REC_BUFFER_SIZE) {
                EXE_Handler(); // 🚀 维持底层心跳，录音时电机不能卡
                DRV_StepperManage();
                DISP_Handler(); // 维持屏幕动画刷新
                int32_t i2s_data[128 * 2]; // 32-bit stereo: 128 frames
                size_t bytes_read = 0;
                i2s_read(I2S_PORT, i2s_data, sizeof(i2s_data), &bytes_read, portMAX_DELAY);
                int samples = bytes_read / 8; // 8 bytes per stereo frame (32-bit L + 32-bit R)

                long energy_sum = 0;
                for (int i = 0; i < samples && shared_pcm_len < REC_BUFFER_SIZE; i++) {
                    int32_t raw32 = i2s_data[i * 2]; // left channel
                    int16_t raw_amp = (int16_t)(raw32 >> 14); // INMP441 24-bit → 16-bit

                    if (!filter_init) {
                        prev_raw = raw_amp;
                        prev_clean = 0;
                        filter_init = true;
                    }

                    float clean_float = 0.995 * (prev_clean + raw_amp - prev_raw);
                    prev_raw = raw_amp;
                    prev_clean = clean_float;

                    int32_t clean_amp = (int32_t)clean_float;

                    // 将声音放大 8 倍 (位移3位) 保证收音清晰
                    int32_t amp_to_record = clean_amp << 3;
                    if (amp_to_record > 32767) amp_to_record = 32767;
                    else if (amp_to_record < -32768) amp_to_record = -32768;

                    energy_sum += abs(amp_to_record);

                    shared_pcm_buf[shared_pcm_len++] = amp_to_record & 0xFF;
                    shared_pcm_buf[shared_pcm_len++] = (amp_to_record >> 8) & 0xFF;
                }
                handleMCPHeartbeat(); 

                if (samples > 0) {
                    long avg_energy = energy_sum / samples;
                    if (avg_energy > max_energy) max_energy = avg_energy;

                    if (millis() - last_print_time > 250) {
                        Serial.printf("📊 [VAD探针] 瞬时能量 (放大后): %ld\n", avg_energy);
                        last_print_time = millis();
                    }

                    if (millis() - start_time < 400) {
                        continue; 
                    }

                    if (avg_energy > 600) { 
                        if (!has_spoken) Serial.printf("🔊 [VAD] 捕获到人声 (突破阈值: %ld)\n", avg_energy);
                        has_spoken = true;
                        last_speech_time = millis();
                    }

                    if (has_spoken && (millis() - last_speech_time > 1200)) {
                        Serial.printf("🛑 [VAD] 停顿超1.2秒，截断！(峰值: %ld)\n", max_energy);
                        break;
                    }
                    if (!has_spoken && (millis() - start_time > 2500)) {
                        Serial.printf("🛑 [VAD] 2.5秒空唤醒。(峰值: %ld)\n", max_energy);
                        shared_pcm_len = 0; 
                        break;
                    }
                }
            }

            Serial.println("[REC] after recording");
            dumpPcmStats((const int16_t*)shared_pcm_buf, shared_pcm_len);
            Serial.flush();

            if (shared_pcm_len == 0) {
                Serial.println(">>> [主核] 播放提示音: 没听到说话...");
                if (!DEBUG_DISABLE_ALL_LOCAL_AUDIO_PROMPTS) {
                    playBase64Audio(AUDIO_HEAR_NOTHING);
                } else {
                    Serial.println("[AUDIO_TEST] AUDIO_HEAR_NOTHING skipped");
                    Serial.flush();
                }
                if(shared_pcm_buf) heap_caps_free(shared_pcm_buf);
                shared_pcm_buf = NULL;
                DISP_SetVoiceState(VOICE_DONE);
                currentState = SYS_WAKE_DETECT;
            } else {
                Serial.printf("✅ [主核] 录音完毕，数据量: %d 字节\n", shared_pcm_len);
                Serial.printf("[REC-DONE] SRAM=%u, PSRAM=%u, core=%d\n", ESP.getFreeHeap(), ESP.getFreePsram(), xPortGetCoreID());
                Serial.flush();
                SetAiPhase(AI_RECORD_DONE);
                NotifyAiStatus();
                DISP_SetVoiceState(VOICE_THINKING);

                // 按需创建云端任务（用完自删除，不常驻 SRAM）
                SetAiPhase(AI_BLE_SUSPENDING);
                NotifyAiStatus();
                delay(100);
                Serial.println("[BLE_CLOUD] suspending BLE before cloud request");
                Serial.flush();
                BLE_ShutdownForCloud();
                SetAiPhase(AI_BLE_SUSPENDED);
                delay(300);
                memlog("BLE suspended before cloud");
                shared_ai_seq++;
                shared_ai_result_ready = false;

                // 播放"稍等我想一下"——必须在 CloudTask 创建之前完成，避免 TLS 前关功放截断
                if (!DEBUG_DISABLE_ALL_LOCAL_AUDIO_PROMPTS) {
                    playBase64Audio(AUDIO_THINKING);
                } else {
                    Serial.println("[AUDIO_TEST] AUDIO_THINKING skipped");
                    Serial.flush();
                }

                SetAiPhase(AI_CLOUD_PREPARING);
                Serial.printf("[2/3] 🚀 创建云端任务... SRAM=%u, PSRAM=%u, core=%d\n", ESP.getFreeHeap(), ESP.getFreePsram(), xPortGetCoreID());
                Serial.flush();
                cloud_task_done = false;
                // 栈 20KB：TLS握手栈深~10KB，20KB留2倍余量；SRAM余量~57KB足够TLS堆
                BaseType_t ret = xTaskCreatePinnedToCore(cloudNetworkTask, "CloudTask", 8192, NULL, 1, NULL, 0);
                Serial.printf("[CLOUD] xTaskCreatePinnedToCore ret=%d (pdPASS=%d)\n", ret, pdPASS);
                Serial.flush();
                if (ret != pdPASS) {
                    Serial.println("❌ 云端任务创建失败！");
                    cloud_task_done = true;
                    shared_cloud_error = CLOUD_API_TIMEOUT;
                }

                Serial.println("[3/3] ⏳ [主核] 阻塞等待后台守护进程完工...");
                uint32_t wait_start = millis();
                while (!cloud_task_done) {
                    EXE_Handler();
                    DRV_StepperManage();
                    handleMCPHeartbeat();
                    DISP_Handler();
                    vTaskDelay(pdMS_TO_TICKS(10));
                    // 30秒未完成 → 打印诊断信息
                    if (millis() - wait_start > 30000 && millis() - wait_start < 30100) {
                        Serial.printf("⚠️ [诊断] 等待已30秒, SRAM=%d, PSRAM=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());
                        UBaseType_t stackHighWaterMark = uxTaskGetStackHighWaterMark(NULL);
                        Serial.printf("⚠️ [诊断] 主核任务栈剩余: %d bytes\n", stackHighWaterMark * 4);
                    }
                    // 60秒未完成 → 再次打印诊断
                    if (millis() - wait_start > 60000 && millis() - wait_start < 60100) {
                        Serial.printf("⚠️ [诊断] 等待已60秒, SRAM=%d, PSRAM=%d\n", ESP.getFreeHeap(), ESP.getFreePsram());
                    }
                    // 120秒未完成 → 软恢复（不再强制重启）
                    if (millis() - wait_start > 120000) {
                        Serial.println("❌ 云端任务超时120秒，软恢复（不重启）");
                        shared_cloud_error = CLOUD_API_TIMEOUT;
                        shared_ai_result_ready = false;
                        SetAiPhase(AI_FAILED);
                        cloud_task_done = true; // 强制解除阻塞
                    }
                }

                // 恢复 BLE
                SetAiPhase(AI_BLE_RESUMING);
                Serial.println("[BLE_CLOUD] resuming BLE after cloud request");
                Serial.flush();
                BLE_ResumeAfterCloud();
                SetAiPhase(AI_BLE_RESUME_DONE);
                delay(300);
                NotifyAiStatus();
                memlog("BLE resumed after cloud");

                // 统一错误处理：根据错误码播放对应提示音
                Serial.printf("[AUDIO_TEST] cloud done, error=%d, is_pure_pref=%d, tts_reply=%s\n",
                    shared_cloud_error, shared_is_pure_pref, shared_tts_reply ? "yes" : "null");
                Serial.flush();
                if (!DEBUG_DISABLE_ALL_LOCAL_AUDIO_PROMPTS) {
                    switch (shared_cloud_error) {
                        case CLOUD_NET_ERROR:
                            playBase64Audio(AUDIO_NET_ERROR);
                            break;
                        case CLOUD_API_TIMEOUT:
                            playBase64Audio(AUDIO_API_TIMEOUT);
                            break;
                        case CLOUD_JSON_ERROR:
                            playBase64Audio(AUDIO_JSON_ERROR);
                            break;
                        default:
                            if (shared_is_pure_pref) {
                                playBase64Audio(AUDIO_PREF_UPDATED);
                            } else if (shared_tts_reply) {
                                // TTS 在执行器之前播放，避免电机噪声盖住语音
                                Serial.printf("[AI_FLOW] deferred_step_count before TTS=%d\n", shared_deferred_count);
                                Serial.flush();
                                if (DEBUG_ENABLE_CLOUD_TTS) {
                                    Serial.println("[AI_FLOW] TTS before executor begin");
                                    Serial.flush();
                                    // HTTP 请求期间屏幕会阻塞，先显示静态提示避免动画卡住
                                    Serial.println("[AI_FLOW] display replying before cloud TTS");
                                    Serial.flush();
                                    DISP_SetVoiceState(VOICE_REPLY, shared_tts_reply);
                                    DISP_Handler();  // 立即刷新，让文字在 HTTP 阻塞前显示
                                    executeTTS(shared_tts_reply);
                                    Serial.println("[AI_FLOW] TTS before executor end");
                                    Serial.flush();
                                } else {
                                    DISP_SetVoiceState(VOICE_REPLY, shared_tts_reply);
                                }
                                Serial.printf("[AI_FLOW] deferred_step_count after TTS=%d\n", shared_deferred_count);
                                Serial.flush();
                                free(shared_tts_reply);
                                shared_tts_reply = NULL;
                            }
                            break;
                    }
                } else {
                    Serial.println("[AUDIO_TEST] all post-cloud audio prompts skipped");
                    Serial.flush();
                    // 屏幕文字替代喇叭提示音
                    if (shared_cloud_error == CLOUD_NET_ERROR) {
                        DISP_SetVoiceState(VOICE_REPLY, "网络或识别失败，请重试");
                    } else if (shared_cloud_error == CLOUD_API_TIMEOUT) {
                        DISP_SetVoiceState(VOICE_REPLY, "云端超时，请重试");
                    } else if (shared_cloud_error == CLOUD_JSON_ERROR) {
                        DISP_SetVoiceState(VOICE_REPLY, "解析失败，请重试");
                    } else if (shared_tts_reply) {
                        DISP_SetVoiceState(VOICE_REPLY, shared_tts_reply);
                    } else {
                        DISP_SetVoiceState(VOICE_REPLY, "已理解，开始执行");
                    }
                    if (shared_tts_reply) { free(shared_tts_reply); shared_tts_reply = NULL; }
                }

                // TTS 播完后（或跳过后），将延迟缓存的 pipeline 步骤推入执行器
                if (shared_deferred_count > 0 && shared_cloud_error == CLOUD_OK) {
                    Serial.printf("[AI_FLOW] starting executor after TTS (%d steps)\n", shared_deferred_count);
                    Serial.flush();
                    EXE_ClearQueue();
                    for (int i = 0; i < shared_deferred_count; i++) {
                        Serial.printf("[AI_FLOW] push deferred step[%d] cmd=%d val=%d time=%d\n",
                            i, shared_deferred_steps[i].cmd_id, shared_deferred_steps[i].val, shared_deferred_steps[i].time);
                        Serial.flush();
                        EXE_PushStep(shared_deferred_steps[i]);
                    }
                } else if (shared_deferred_count == 0) {
                    Serial.println("[AI_FLOW_WARN] no deferred pipeline to execute");
                    Serial.flush();
                }
                shared_deferred_count = 0;  // 清理，避免下次请求残留

                if (!DEBUG_DISABLE_ALL_LOCAL_AUDIO_PROMPTS) {
                    // 第一阶段测试：云端成功后播放固定提示音（分段诊断）
                    if (DEBUG_ENABLE_POST_CLOUD_TASK_DONE_AUDIO && shared_cloud_error == CLOUD_OK) {
                        Serial.println("[AUDIO_TEST] post-cloud TASK_DONE diag begin");
                        Serial.printf("[AUDIO_TEST] diag_level=%d, free_heap=%u\n", DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL, ESP.getFreeHeap());
                        Serial.flush();

                        if (DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL == 0) {
                            Serial.println("[AUDIO_DIAG] level=0 skip all");
                        } else if (DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL == 1) {
                            // Level 1: 只测试功放 enable/disable
                            Serial.println("[AUDIO_DIAG] level=1 amp only begin");
                            Serial.printf("[AUDIO_DIAG] heap before amp on: %u\n", ESP.getFreeHeap());
                            Serial.flush();
                            setAmplifierState(true);
                            Serial.println("[AUDIO_DIAG] amp on ok");
                            Serial.flush();
                            delay(300);
                            setAmplifierState(false);
                            Serial.println("[AUDIO_DIAG] amp off ok");
                            Serial.printf("[AUDIO_DIAG] heap after amp test: %u\n", ESP.getFreeHeap());
                            Serial.println("[AUDIO_DIAG] level=1 amp only end");
                            Serial.flush();
                        } else if (DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL == 2) {
                            // Level 2: 只做 base64/WAV 解析，不开功放，不写 I2S
                            Serial.println("[AUDIO_DIAG] level=2 decode only begin");
                            Serial.printf("[AUDIO_DIAG] heap before decode: %u\n", ESP.getFreeHeap());
                            Serial.flush();

                            const char* b64 = AUDIO_TASK_DONE;
                            size_t b64_len = strlen(b64);
                            Serial.printf("[AUDIO_DIAG] b64_len=%u mod4=%u\n", b64_len, b64_len % 4);

                            size_t decoded_max_len = (b64_len / 4) * 3;
                            Serial.printf("[AUDIO_DIAG] decoded_max_len=%u\n", decoded_max_len);
                            Serial.println("[AUDIO_DIAG] before decode alloc");
                            Serial.flush();

                            uint8_t* decoded = (uint8_t*)heap_caps_malloc(decoded_max_len, MALLOC_CAP_SPIRAM);
                            if (!decoded) {
                                Serial.println("[AUDIO_DIAG] alloc FAILED");
                            } else {
                                Serial.println("[AUDIO_DIAG] after decode alloc");
                                Serial.flush();

                                size_t decoded_len = 0;
                                int ret = mbedtls_base64_decode(decoded, decoded_max_len, &decoded_len, (const unsigned char*)b64, b64_len);
                                Serial.printf("[AUDIO_DIAG] after base64 decode, ret=%d, decoded_len=%u\n", ret, decoded_len);
                                Serial.flush();

                                if (ret != 0) {
                                    Serial.printf("[AUDIO_DIAG] base64 decode error: %d\n", ret);
                                } else if (decoded_len < 44) {
                                    Serial.printf("[AUDIO_DIAG] WAV too short: %u bytes\n", decoded_len);
                                } else {
                                    Serial.printf("[AUDIO_DIAG] wav_head=%c%c%c%c\n", decoded[0], decoded[1], decoded[2], decoded[3]);
                                    Serial.printf("[AUDIO_DIAG] wav_fmt=%c%c%c%c\n", decoded[8], decoded[9], decoded[10], decoded[11]);

                                    if (decoded[0] != 'R' || decoded[1] != 'I' || decoded[2] != 'F' || decoded[3] != 'F') {
                                        Serial.println("[AUDIO_DIAG] NOT a RIFF file");
                                    } else {
                                        uint32_t sample_rate = decoded[24] | (decoded[25] << 8) | (decoded[26] << 16) | (decoded[27] << 24);
                                        uint16_t channels = decoded[22] | (decoded[23] << 8);
                                        uint16_t bits_per_sample = decoded[34] | (decoded[35] << 8);
                                        uint32_t data_size = decoded[40] | (decoded[41] << 8) | (decoded[42] << 16) | (decoded[43] << 24);

                                        // 查找 data chunk offset
                                        uint32_t data_offset = 44;
                                        for (uint32_t i = 12; i < decoded_len - 4; i++) {
                                            if (decoded[i] == 'd' && decoded[i+1] == 'a' && decoded[i+2] == 't' && decoded[i+3] == 'a') {
                                                data_offset = i + 8; break;
                                            }
                                        }

                                        Serial.printf("[AUDIO_DIAG] wav sample_rate=%u, bits=%u, channels=%u, data_offset=%u, data_size=%u\n",
                                            sample_rate, bits_per_sample, channels, data_offset, data_size);
                                    }
                                }

                                Serial.println("[AUDIO_DIAG] before free decoded buffer");
                                Serial.flush();
                                heap_caps_free(decoded);
                            }

                            Serial.println("[AUDIO_DIAG] after free decoded buffer");
                            Serial.printf("[AUDIO_DIAG] heap after decode: %u\n", ESP.getFreeHeap());
                            Serial.println("[AUDIO_DIAG] level=2 decode only end");
                            Serial.flush();
                        } else if (DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL == 3) {
                            // Level 3: 解析 + I2S 写入前 200ms，功放关闭
                            Serial.println("[AUDIO_DIAG] level=3 i2s write only begin");
                            Serial.printf("[AUDIO_DIAG] heap before level3: %u\n", ESP.getFreeHeap());
                            Serial.flush();

                            Serial.println("[AUDIO_DIAG] force amp off");
                            Serial.flush();
                            setAmplifierState(false);

                            Serial.println("[AUDIO_DIAG] decode begin");
                            Serial.flush();
                            const char* b64 = AUDIO_TASK_DONE;
                            size_t b64_len = strlen(b64);
                            size_t decoded_max_len = (b64_len / 4) * 3;
                            uint8_t* decoded = (uint8_t*)heap_caps_malloc(decoded_max_len, MALLOC_CAP_SPIRAM);
                            if (!decoded) {
                                Serial.println("[AUDIO_DIAG] alloc FAILED");
                            } else {
                                size_t decoded_len = 0;
                                int ret = mbedtls_base64_decode(decoded, decoded_max_len, &decoded_len, (const unsigned char*)b64, b64_len);
                                if (ret != 0 || decoded_len < 44) {
                                    Serial.printf("[AUDIO_DIAG] decode error: ret=%d, len=%u\n", ret, decoded_len);
                                    heap_caps_free(decoded);
                                } else {
                                    uint32_t sample_rate = decoded[24] | (decoded[25] << 8) | (decoded[26] << 16) | (decoded[27] << 24);
                                    uint16_t channels = decoded[22] | (decoded[23] << 8);
                                    uint16_t bits_per_sample = decoded[34] | (decoded[35] << 8);
                                    uint32_t data_size = decoded[40] | (decoded[41] << 8) | (decoded[42] << 16) | (decoded[43] << 24);
                                    uint32_t data_offset = 44;
                                    for (uint32_t i = 12; i < decoded_len - 4; i++) {
                                        if (decoded[i] == 'd' && decoded[i+1] == 'a' && decoded[i+2] == 't' && decoded[i+3] == 'a') {
                                            data_offset = i + 8; break;
                                        }
                                    }

                                    Serial.printf("[AUDIO_DIAG] decoded_len=%u, sample_rate=%u, bits=%u, channels=%u, data_offset=%u, data_size=%u\n",
                                        decoded_len, sample_rate, bits_per_sample, channels, data_offset, data_size);
                                    Serial.flush();

                                    // 计算 200ms 数据量
                                    uint32_t test_ms = 200;
                                    uint32_t bytes_per_sample = bits_per_sample / 8;
                                    uint32_t bytes_per_ms = sample_rate * channels * bytes_per_sample / 1000;
                                    uint32_t test_bytes = bytes_per_ms * test_ms;
                                    if (test_bytes > data_size) test_bytes = data_size;

                                    Serial.printf("[AUDIO_DIAG] level3 target_ms=%u, target_bytes=%u\n", test_ms, test_bytes);
                                    Serial.flush();

                                    int16_t* pcm_data = (int16_t*)(decoded + data_offset);
                                    size_t pcm_samples = test_bytes / 2;

                                    // 切换 I2S 到 16-bit（硬件默认 32-bit 会翻倍消费数据）
                                    i2s_set_clk(I2S_PORT, sample_rate, 16, I2S_CHANNEL_STEREO);

                                    // 写入前 200ms (mono→stereo，与 playBase64Audio 相同格式)
                                    const size_t BATCH_SAMPLES = 512;
                                    int16_t* stereo_buf = (int16_t*)heap_caps_malloc(BATCH_SAMPLES * 4, MALLOC_CAP_DMA);
                                    if (stereo_buf) {
                                        size_t total_written = 0;
                                        bool firstWrite = true;
                                        for (size_t i = 0; i < pcm_samples; i += BATCH_SAMPLES) {
                                            if (firstWrite) { Serial.println("[AUDIO_DIAG] before first i2s_write"); Serial.flush(); }
                                            size_t batch_count = min(BATCH_SAMPLES, pcm_samples - i);
                                            for (size_t j = 0; j < batch_count; j++) {
                                                stereo_buf[j * 2] = pcm_data[i + j];
                                                stereo_buf[j * 2 + 1] = pcm_data[i + j];
                                            }
                                            size_t bytes_written = 0;
                                            int wret = i2s_write(I2S_PORT, stereo_buf, batch_count * 4, &bytes_written, portMAX_DELAY);
                                            total_written += bytes_written;
                                            if (firstWrite) {
                                                Serial.printf("[AUDIO_DIAG] first i2s_write ret=%d, bytes_written=%u\n", wret, bytes_written);
                                                Serial.flush();
                                                firstWrite = false;
                                            }
                                        }
                                        Serial.printf("[AUDIO_DIAG] total i2s bytes_written=%u\n", total_written);
                                        Serial.flush();
                                        heap_caps_free(stereo_buf);
                                    } else {
                                        Serial.println("[AUDIO_DIAG] stereo_buf alloc FAILED");
                                        Serial.flush();
                                    }

                                    Serial.println("[AUDIO_DIAG] before i2s_zero_dma_buffer");
                                    Serial.flush();
                                    i2s_zero_dma_buffer(I2S_PORT);
                                    Serial.println("[AUDIO_DIAG] after i2s_zero_dma_buffer");
                                    Serial.flush();

                                    // 恢复 I2S 到麦克风使用的 32-bit 16kHz
                                    i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);

                                    Serial.println("[AUDIO_DIAG] before free decoded buffer");
                                    Serial.flush();
                                    heap_caps_free(decoded);
                                    Serial.println("[AUDIO_DIAG] after free decoded buffer");
                                    Serial.flush();
                                }
                            }

                            setAmplifierState(false);
                            Serial.printf("[AUDIO_DIAG] heap after level3: %u\n", ESP.getFreeHeap());
                            Serial.println("[AUDIO_DIAG] level=3 i2s write only end");
                            Serial.flush();
                        } else if (DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL == 4) {
                            // Level 4: 开功放 + I2S 写入前 200ms
                            Serial.println("[AUDIO_DIAG] level=4 amp+i2s 200ms begin");
                            Serial.printf("[AUDIO_DIAG] heap before level4: %u\n", ESP.getFreeHeap());
                            Serial.flush();

                            Serial.println("[AUDIO_DIAG] force amp off");
                            Serial.flush();
                            setAmplifierState(false);

                            Serial.println("[AUDIO_DIAG] decode begin");
                            Serial.flush();
                            const char* b64 = AUDIO_TASK_DONE;
                            size_t b64_len = strlen(b64);
                            size_t decoded_max_len = (b64_len / 4) * 3;
                            uint8_t* decoded = (uint8_t*)heap_caps_malloc(decoded_max_len, MALLOC_CAP_SPIRAM);
                            if (!decoded) {
                                Serial.println("[AUDIO_DIAG] alloc FAILED");
                                setAmplifierState(false);
                                i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);
                            } else {
                                size_t decoded_len = 0;
                                int ret = mbedtls_base64_decode(decoded, decoded_max_len, &decoded_len, (const unsigned char*)b64, b64_len);
                                if (ret != 0 || decoded_len < 44) {
                                    Serial.printf("[AUDIO_DIAG] decode error: ret=%d, len=%u\n", ret, decoded_len);
                                    heap_caps_free(decoded);
                                    setAmplifierState(false);
                                    i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);
                                } else {
                                    uint32_t sample_rate = decoded[24] | (decoded[25] << 8) | (decoded[26] << 16) | (decoded[27] << 24);
                                    uint16_t channels = decoded[22] | (decoded[23] << 8);
                                    uint16_t bits_per_sample = decoded[34] | (decoded[35] << 8);
                                    uint32_t data_size = decoded[40] | (decoded[41] << 8) | (decoded[42] << 16) | (decoded[43] << 24);
                                    uint32_t data_offset = 44;
                                    for (uint32_t i = 12; i < decoded_len - 4; i++) {
                                        if (decoded[i] == 'd' && decoded[i+1] == 'a' && decoded[i+2] == 't' && decoded[i+3] == 'a') {
                                            data_offset = i + 8; break;
                                        }
                                    }

                                    Serial.printf("[AUDIO_DIAG] decoded_len=%u, sample_rate=%u, bits=%u, channels=%u, data_offset=%u, data_size=%u\n",
                                        decoded_len, sample_rate, bits_per_sample, channels, data_offset, data_size);
                                    Serial.flush();

                                    uint32_t test_ms = DEBUG_POST_CLOUD_AUDIO_TEST_MS;
                                    uint32_t bytes_per_sample = bits_per_sample / 8;
                                    uint32_t bytes_per_ms = sample_rate * channels * bytes_per_sample / 1000;
                                    uint32_t test_bytes = bytes_per_ms * test_ms;
                                    if (test_bytes > data_size) test_bytes = data_size;

                                    Serial.printf("[AUDIO_DIAG] level4 test_ms=%u\n", test_ms);
                                    Serial.printf("[AUDIO_DIAG] level4 target_bytes=%u\n", test_bytes);
                                    Serial.flush();

                                    int16_t* pcm_data = (int16_t*)(decoded + data_offset);
                                    size_t pcm_samples = test_bytes / 2;

                                    Serial.println("[AUDIO_DIAG] before zero dma pre");
                                    Serial.flush();
                                    i2s_zero_dma_buffer(I2S_PORT);
                                    Serial.println("[AUDIO_DIAG] after zero dma pre");
                                    Serial.flush();

                                    uint32_t byteRate = sample_rate * channels * (bits_per_sample / 8);
                                    uint32_t duration_ms = (byteRate > 0) ? (data_size * 1000UL / byteRate) : 0;
                                    Serial.printf("[AUDIO] sr=%u bits=%u ch=%u data=%u byteRate=%u duration=%ums\n",
                                        sample_rate, bits_per_sample, channels, data_size, byteRate, duration_ms);
                                    Serial.flush();

                                    // 切换 I2S 到 16-bit（硬件默认 32-bit 会翻倍消费数据）
                                    i2s_set_clk(I2S_PORT, sample_rate, 16, I2S_CHANNEL_STEREO);
                                    delay(20);

                                    Serial.println("[AUDIO_DIAG] before amp on");
                                    Serial.flush();
                                    setAmplifierState(true);
                                    delay(80);
                                    Serial.println("[AUDIO_DIAG] after amp on");
                                    Serial.flush();

                                    const size_t BATCH_SAMPLES = 512;
                                    int16_t* stereo_buf = (int16_t*)heap_caps_malloc(BATCH_SAMPLES * 4, MALLOC_CAP_DMA);
                                    if (stereo_buf) {
                                        const char* mode_name = (DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE == 1) ? "silence" :
                                                                (DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE == 2) ? "attenuated" : "normal";
                                        Serial.printf("[AUDIO_DIAG] mode=%d(%s)\n", DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE, mode_name);
                                        Serial.flush();

                                        // 静音模式：预填充零值，后续不再改写
                                        if (DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE == 1) {
                                            memset(stereo_buf, 0, BATCH_SAMPLES * 4);
                                        }

                                        size_t total_written = 0;
                                        bool firstWrite = true;
                                        uint32_t next_progress_bytes = bytes_per_ms * 100; // 每 100ms 打一次进度
                                        for (size_t i = 0; i < pcm_samples; i += BATCH_SAMPLES) {
                                            if (firstWrite) { Serial.println("[AUDIO_DIAG] before first i2s_write"); Serial.flush(); }
                                            size_t batch_count = min(BATCH_SAMPLES, pcm_samples - i);

                                            if (DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE == 0) {
                                                // 正常音频
                                                for (size_t j = 0; j < batch_count; j++) {
                                                    stereo_buf[j * 2] = pcm_data[i + j];
                                                    stereo_buf[j * 2 + 1] = pcm_data[i + j];
                                                }
                                            } else if (DEBUG_POST_CLOUD_AUDIO_OUTPUT_MODE == 1) {
                                                // 静音：stereo_buf 已经是 0，跳过填充
                                            } else {
                                                // 降音量：PCM >> 2
                                                for (size_t j = 0; j < batch_count; j++) {
                                                    stereo_buf[j * 2] = pcm_data[i + j] >> 2;
                                                    stereo_buf[j * 2 + 1] = pcm_data[i + j] >> 2;
                                                }
                                            }

                                            size_t bytes_written = 0;
                                            int wret = i2s_write(I2S_PORT, stereo_buf, batch_count * 4, &bytes_written, portMAX_DELAY);
                                            total_written += bytes_written;
                                            if (firstWrite) {
                                                Serial.printf("[AUDIO_DIAG] first i2s_write ret=%d, bytes_written=%u\n", wret, bytes_written);
                                                Serial.flush();
                                                firstWrite = false;
                                            }
                                            // 进度日志：每 ~100ms
                                            uint32_t played_mono_bytes = total_written / 2;
                                            if (played_mono_bytes >= next_progress_bytes) {
                                                uint32_t played_ms = (byteRate > 0) ? (played_mono_bytes * 1000UL / byteRate) : 0;
                                                Serial.printf("[AUDIO_DIAG] progress played_ms=%u i2s_bytes=%u\n", played_ms, (unsigned)total_written);
                                                Serial.flush();
                                                next_progress_bytes += bytes_per_ms * 100;
                                            }
                                        }
                                        uint32_t played_mono_bytes = total_written / 2;
                                        uint32_t elapsed_ms = (byteRate > 0) ? (played_mono_bytes * 1000UL / byteRate) : 0;
                                        Serial.printf("[AUDIO] mode=%s target_ms=%u i2s_bytes=%u elapsed≈%ums\n",
                                            mode_name, test_ms, (unsigned)total_written, elapsed_ms);
                                        Serial.flush();
                                        heap_caps_free(stereo_buf);
                                    } else {
                                        Serial.println("[AUDIO_DIAG] stereo_buf alloc FAILED");
                                        Serial.flush();
                                    }

                                    Serial.println("[AUDIO_DIAG] before i2s_zero_dma_buffer post");
                                    Serial.flush();
                                    i2s_zero_dma_buffer(I2S_PORT);
                                    Serial.println("[AUDIO_DIAG] after i2s_zero_dma_buffer post");
                                    Serial.flush();

                                    delay(20);
                                    Serial.println("[AUDIO_DIAG] before amp off");
                                    Serial.flush();
                                    setAmplifierState(false);
                                    delay(50);
                                    Serial.println("[AUDIO_DIAG] after amp off");
                                    Serial.flush();

                                    // 恢复 I2S 到麦克风使用的 32-bit 16kHz
                                    i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);

                                    Serial.println("[AUDIO_DIAG] before free decoded buffer");
                                    Serial.flush();
                                    heap_caps_free(decoded);
                                    Serial.println("[AUDIO_DIAG] after free decoded buffer");
                                    Serial.flush();
                                }
                            }

                            setAmplifierState(false);
                            i2s_set_clk(I2S_PORT, 16000, 32, I2S_CHANNEL_STEREO);
                            Serial.printf("[AUDIO_DIAG] heap after level4: %u\n", ESP.getFreeHeap());
                            Serial.println("[AUDIO_DIAG] level=4 amp+i2s 200ms end");
                            Serial.flush();
                        } else {
                            // Level 5: 调用完整 playBase64Audio
                            Serial.printf("[AUDIO_DIAG] level=%d calling playBase64Audio\n", DEBUG_POST_CLOUD_AUDIO_DIAG_LEVEL);
                            Serial.flush();
                            playBase64Audio(AUDIO_TASK_DONE);
                        }

                        Serial.printf("[AUDIO_TEST] post-cloud TASK_DONE diag end, free_heap=%u\n", ESP.getFreeHeap());
                        Serial.flush();
                    }
                }

                printMemoryStatus("交互回合结束");
                // 不要立即覆盖 VOICE_REPLY，让 DISP_Handler 自动延时切换
                if (DISP_GetVoiceState() != VOICE_REPLY) {
                    DISP_SetVoiceState(VOICE_DONE);
                }
                SetAiPhase(AI_IDLE);
                NotifyAiStatus();
                currentState = SYS_WAKE_DETECT;
            }
            break;
        }
    }
}
