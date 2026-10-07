#include "HardwareDriver.h"
#include <Wire.h>
#include "FastAccelStepper.h"
#include <ESP32Encoder.h>
#include "paj7620.h"

#define IPS_DEBOUNCE_MS 3


// 实例化全局对象
BLDCMotor washerMotor(BLDC_VR_PIN, BLDC_ZF_PIN, BLDC_PWM_CH);

FastAccelStepperEngine stepperEngine = FastAccelStepperEngine();
FastAccelStepper *stepper = NULL;

static ESP32Encoder encoder;
static int64_t lastRawEncoderValue = 0;
static long softwareEncoderCount = 0;
static long lastRegisteredRaw = 0;
static long lastLogicalValue = 0;
static volatile bool gestureReady = false;
static void IRAM_ATTR gestureISR() { gestureReady = true; }

// --- BLDCMotor 实现 (PWM IO 版) ---
BLDCMotor::BLDCMotor(uint8_t vrPin, uint8_t zfPin, uint8_t pwmCh)
    : _vrPin(vrPin), _zfPin(zfPin), _pwmCh(pwmCh), _currentSpeed(0) {}

void BLDCMotor::setPWM(uint8_t percent) {
    _currentSpeed = percent;
    uint8_t duty = map(constrain(percent, 0, 100), 0, 100, 0, 255);
    ledcWrite(_pwmCh, duty);
}

void BLDCMotor::begin() {
    ledcSetup(_pwmCh, BLDC_PWM_FREQ, BLDC_PWM_RES);
    ledcAttachPin(_vrPin, _pwmCh);
    pinMode(_zfPin, OUTPUT);
    brake();
}

void BLDCMotor::runCW(uint8_t speed) {
    digitalWrite(_zfPin, HIGH);
    setPWM(speed);
}

void BLDCMotor::runCCW(uint8_t speed) {
    digitalWrite(_zfPin, LOW);
    setPWM(speed);
}

void BLDCMotor::brake() {
    setPWM(0);
}

void BLDCMotor::brakeThenCW() {
    setPWM(0);
    digitalWrite(_zfPin, HIGH);  // 预置方向：下一步正转
}

void BLDCMotor::brakeThenCCW() {
    setPWM(0);
    digitalWrite(_zfPin, LOW);   // 预置方向：下一步反转
}

uint8_t BLDCMotor::getCurrentSpeed() { return _currentSpeed; }

// --- 硬件驱动 API 实现 ---
void DRV_Init() {


    // 3. BLDC 初始化
    washerMotor.begin();

    // 1. GPIO 初始化
    pinMode(PIN_HALL_0, INPUT_PULLUP);
    pinMode(PIN_HALL_90, INPUT_PULLUP);
    pinMode(PIN_HALL_180, INPUT_PULLUP);
    pinMode(PIN_HALL_BLDC, INPUT_PULLUP);
    pinMode(PIN_WATER_LEVEL, INPUT_PULLUP);

    // 2. 步进电机初始化
    pinMode(PIN_STEP_ENA, OUTPUT);
    digitalWrite(PIN_STEP_ENA, LOW); 
    stepperEngine.init();
    stepper = stepperEngine.stepperConnectToPin(PIN_STEP_PUL);
    if (stepper) {
        stepper->setDirectionPin(PIN_STEP_DIR);
        stepper->setAcceleration(2500);
        stepper->setSpeedInHz(4000);
    }

    // 4. ADC 初始化
    analogReadResolution(12);
    analogSetPinAttenuation(TS300B_PIN, ADC_11db);

    // 5. 编码器初始化
    DRV_EncoderInit();
}

void DRV_WaterValve(bool open) {
    uint8_t val = mcp_read_register(0x12);
    if (open) val |= (1 << MCP_GPA0_VALVE);
    else val &= ~(1 << MCP_GPA0_VALVE);
    mcp_write_register(0x12, val);
}

void DRV_UVLamp(bool open) {
    uint8_t val = mcp_read_register(0x12);
    if (open) val |= (1 << MCP_GPA2_UV);
    else val &= ~(1 << MCP_GPA2_UV);
    mcp_write_register(0x12, val);
}

void DRV_SetHeartbeat(bool on) {
    uint8_t val = mcp_read_register(0x13);
    if (on) val |= (1 << MCP_GPB7_LED);
    else val &= ~(1 << MCP_GPB7_LED);
    mcp_write_register(0x13, val);
}

bool DRV_IsWaterFull() {
    return digitalRead(PIN_WATER_LEVEL) == HIGH;
}

float DRV_GetTurbidity() {
    int sum = 0;
    for(int i=0; i<10; i++) sum += analogRead(TS300B_PIN);
    float avgAdc = (float)sum / 10.0;
    float voltage = (avgAdc / ADC_MAX_VAL) * ADC_REF_VOLTAGE * VOLTAGE_RATIO;
    // 简化版公式，使用 25度 标定
    float ntu = -865.68 * voltage + K_VALUE_TURB;
    return (ntu < 0) ? 0.0 : ntu;
}

bool DRV_ReadIPS(int target) {
    int target_pin = -1;
    int debounce_ms = IPS_DEBOUNCE_MS; // 默认 3ms

    // 1. 确定要盯着哪个引脚看，IPS180 噪声大，单独加强消抖
    switch(target) {
        case POS_0:   target_pin = PIN_HALL_0; break;
        case POS_90:  target_pin = PIN_HALL_90; break;
        case POS_180: target_pin = PIN_HALL_180; debounce_ms = 20; break;
        case 21:      target_pin = PIN_HALL_BLDC; break;
        default:      return false;
    }

    // 2. 消抖：连续读取 debounce_ms 次，每次间隔 1ms
    //    只要有一次 HIGH，立即返回 false（毛刺判定）
    uint32_t start_us = micros();
    for (int i = 0; i < debounce_ms; i++) {
        while (micros() - start_us < (uint32_t)(i + 1) * 1000) {
            yield();
        }
        if (digitalRead(target_pin) == HIGH) {
            return false;
        }
    }
    return true;
}

void DRV_StepperRun(bool forward) {
    if (!stepper) return;  // 安全检查
    if (forward) {
        stepper->runBackward();
    } else {
        stepper->runForward();
    }
}

void DRV_StepperStop() {
    if (stepper) stepper->forceStopAndNewPosition(0);
}

void DRV_StepperManage() {
    stepperEngine.manageSteppers();
}

void mcp_write_register(uint8_t reg, uint8_t value) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg); Wire.write(value);
    Wire.endTransmission();
}

uint8_t mcp_read_register(uint8_t reg) {
    Wire.beginTransmission(MCP23017_ADDR);
    Wire.write(reg); Wire.endTransmission(false);
    Wire.requestFrom((uint16_t)MCP23017_ADDR, (uint8_t)1);
    return Wire.available() ? Wire.read() : 0;
}

void DRV_EncoderInit() {
    ESP32Encoder::useInternalWeakPullResistors = UP;
    // 注意：请确保 HardwareConfig.h 中已定义 EC11_A_PIN 和 EC11_B_PIN
    encoder.attachFullQuad(EC11_A_PIN, EC11_B_PIN);
    encoder.setFilter(1023);
    encoder.clearCount(); // 安全重置库的底层状态
    lastRawEncoderValue = (int16_t)encoder.getCount(); // 只抓取低 16 位纯净物理值
}

bool DRV_IsEC11ButtonPressed() {
    static uint32_t lastPressTime = 0;
    // MCP23017_ADDR 在 HardwareConfig.h 中定义
    uint8_t gpiob_val = mcp_read_register(0x13); 
    
    // GPB0 是低电平触发 (按下为 0)
    if (!(gpiob_val & (1 << 0))) {
        if (millis() - lastPressTime > 1000) {
            lastPressTime = millis();
            return true; 
        }
    }
    return false;
}

long DRV_GetEncoderValue() {
    int64_t currentRawValue = encoder.getCount();
    int64_t delta = currentRawValue - lastRawEncoderValue;

    // 核心修复 1: 斩杀 16382 幽灵飞点 Bug [cite: 1089-1092]
    if (abs((long)delta) > 50) {
        delta = 0;
    }

    lastRawEncoderValue = currentRawValue;
    softwareEncoderCount += delta;

    // 核心修复 2: 磁性滞回算法 [cite: 1095-1113]
    while (softwareEncoderCount - lastRegisteredRaw >= 2) {
        lastRegisteredRaw += 2;
        lastLogicalValue++;
    }
    while (softwareEncoderCount - lastRegisteredRaw <= -2) {
        lastRegisteredRaw -= 2;
        lastLogicalValue--;
    }
    return lastLogicalValue;
}

void DRV_GestureInit() {
    Wire.beginTransmission(0x73); 
    Wire.write(0x00); 
    Wire.endTransmission();
    delay(15);
    
    uint8_t error = paj7620Init();
    if (!error) {
        uint8_t dummy; 
        paj7620ReadReg(0x43, 1, &dummy); // 清除可能残留的标志位
        paj7620ReadReg(0x44, 1, &dummy);
        
        pinMode(PAJ_INT_PIN, INPUT_PULLUP);
        attachInterrupt(digitalPinToInterrupt(PAJ_INT_PIN), gestureISR, FALLING);
        Serial.println("✅ [驱动层] PAJ7620 手势传感器初始化成功");
    } else {
        Serial.printf("❌ [驱动层] PAJ7620 初始化失败, 错误码: %d\n", error);
    }
}

Gesture_t DRV_GetGesture() {
    if (gestureReady) {
        uint8_t data = 0;
        gestureReady = false;
        if (!paj7620ReadReg(0x43, 1, &data) && data != 0) {
            if (data & GES_UP_FLAG)       return GESTURE_UP;
            if (data & GES_DOWN_FLAG)     return GESTURE_DOWN;
            if (data & GES_LEFT_FLAG)     return GESTURE_LEFT;
            if (data & GES_RIGHT_FLAG)    return GESTURE_RIGHT;
            if (data & GES_CLOCKWISE_FLAG) return GESTURE_CW;
        }
    }
    return GESTURE_NONE;
}

// ==========================================
// 🧠 智能探针系统：状态变化检测 + 预期对比
// ==========================================
SensorState_t DRV_GetSensorState() {
    // 多次采样取稳定值，过滤噪声（IPS180 噪声大，10次采样8次通过）
    int cnt_0 = 0, cnt_90 = 0, cnt_180 = 0, cnt_bldc = 0;
    for (int i = 0; i < 10; i++) {
        if (digitalRead(PIN_HALL_0) == LOW) cnt_0++;
        if (digitalRead(PIN_HALL_90) == LOW) cnt_90++;
        if (digitalRead(PIN_HALL_180) == LOW) cnt_180++;
        if (digitalRead(PIN_HALL_BLDC) == LOW) cnt_bldc++;
    }
    SensorState_t state;
    state.ips_0    = (cnt_0 >= 6);
    state.ips_90   = (cnt_90 >= 6);
    state.ips_180  = (cnt_180 >= 9);
    state.ips_bldc = (cnt_bldc >= 6);
    return state;
}

static bool operator!=(const SensorState_t& a, const SensorState_t& b) {
    return a.ips_0 != b.ips_0 || a.ips_90 != b.ips_90 ||
           a.ips_180 != b.ips_180 || a.ips_bldc != b.ips_bldc;
}

void DRV_SmartProbe(const char* stage, SensorState_t* expected) {
    static SensorState_t last_state = {false, false, false, false};
    static uint32_t last_output_time = 0;

    SensorState_t current = DRV_GetSensorState();

    // 检测状态变化
    bool state_changed = (current != last_state);

    // 检测预期异常
    bool anomaly_detected = false;
    if (expected) {
        // 预期异常：某个传感器触发了但不应该触发，或者没触发但应该触发
        anomaly_detected = (current.ips_0 != expected->ips_0) ||
                          (current.ips_90 != expected->ips_90) ||
                          (current.ips_180 != expected->ips_180) ||
                          (current.ips_bldc != expected->ips_bldc);
    }

    // 只在以下情况输出：
    // 1. 状态发生变化
    // 2. 检测到预期异常
    // 3. 距离上次输出超过 5 秒（防止长时间无输出导致误判）
    bool should_output = state_changed || anomaly_detected ||
                        (millis() - last_output_time > 5000);

    if (should_output) {
        last_output_time = millis();

        if (anomaly_detected) {
            // 异常输出：用 ⚠️ 标记，显示当前状态和预期状态
            Serial.printf("⚠️ [%s] 异常! 当前: 0°%s 90°%s 180°%s BLDC%s | 预期: 0°%s 90°%s 180°%s BLDC%s\n",
                stage,
                current.ips_0 ? "🔴" : "⚪",
                current.ips_90 ? "🔴" : "⚪",
                current.ips_180 ? "🔴" : "⚪",
                current.ips_bldc ? "🔴" : "⚪",
                expected->ips_0 ? "🔴" : "⚪",
                expected->ips_90 ? "🔴" : "⚪",
                expected->ips_180 ? "🔴" : "⚪",
                expected->ips_bldc ? "🔴" : "⚪");
        } else if (state_changed) {
            // 状态变化输出：显示变化的传感器
            Serial.printf("⚡ [%s] 状态变化:", stage);
            if (current.ips_0 != last_state.ips_0)
                Serial.printf(" 0°%s→%s", last_state.ips_0 ? "🔴" : "⚪", current.ips_0 ? "🔴" : "⚪");
            if (current.ips_90 != last_state.ips_90)
                Serial.printf(" 90°%s→%s", last_state.ips_90 ? "🔴" : "⚪", current.ips_90 ? "🔴" : "⚪");
            if (current.ips_180 != last_state.ips_180)
                Serial.printf(" 180°%s→%s", last_state.ips_180 ? "🔴" : "⚪", current.ips_180 ? "🔴" : "⚪");
            if (current.ips_bldc != last_state.ips_bldc)
                Serial.printf(" BLDC%s→%s", last_state.ips_bldc ? "🔴" : "⚪", current.ips_bldc ? "🔴" : "⚪");
            Serial.println();
        } else {
            // 定期输出（5秒一次，防止长时间无输出）
            Serial.printf("📊 [%s] 0°%s 90°%s 180°%s BLDC%s\n",
                stage,
                current.ips_0 ? "🔴" : "⚪",
                current.ips_90 ? "🔴" : "⚪",
                current.ips_180 ? "🔴" : "⚪",
                current.ips_bldc ? "🔴" : "⚪");
        }

        last_state = current;
    }
}