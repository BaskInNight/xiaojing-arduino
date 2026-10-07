#ifndef HARDWARE_DRIVER_H
#define HARDWARE_DRIVER_H

#include <Arduino.h>
#include "HardwareConfig.h"

enum DrumPosture { 
    POS_UNKNOWN = 0, 
    POS_0 = 1, 
    POS_90 = 2, 
    POS_180 = 3 
};

// =============================================================================
// 1. BLDC 无刷电机驱动类 (PWM IO 版)
// =============================================================================
class BLDCMotor {
public:
    BLDCMotor(uint8_t vrPin, uint8_t zfPin, uint8_t pwmCh);
    void begin();
    void runCW(uint8_t speed);
    void runCCW(uint8_t speed);
    void brake();
    void brakeThenCW();   // PWM=0 + 预置 ZF=HIGH，供下一拍 runCW
    void brakeThenCCW();  // PWM=0 + 预置 ZF=LOW，供下一拍 runCCW
    uint8_t getCurrentSpeed();

private:
    uint8_t _vrPin;
    uint8_t _zfPin;
    uint8_t _pwmCh;
    uint8_t _currentSpeed;
    void setPWM(uint8_t percent);
};

extern BLDCMotor washerMotor;

// =============================================================================
// 2. 硬件驱动层 API (肌肉层)
// =============================================================================

// --- 系统初始化 ---
void DRV_Init();

// --- 高压负载控制 (通过 MCP23017) ---
void DRV_WaterValve(bool open);
void DRV_UVLamp(bool open);
void DRV_SetHeartbeat(bool on);

// --- 传感器读取 ---
bool DRV_IsWaterFull();
float DRV_GetTurbidity();
bool DRV_ReadIPS(int angle); // angle: 0, 90, 180, 21 (BLDC)
bool DRV_IsEC11ButtonPressed(); 
void DRV_EncoderInit();         // 初始化编码器
long DRV_GetEncoderValue();     // 获取最新且经过磁性滞回处理的逻辑值

// PAJ7620 手势识别模块
typedef enum {
    GESTURE_NONE = 0,
    GESTURE_UP,     // 向上挥
    GESTURE_DOWN,   // 向下挥
    GESTURE_LEFT,
    GESTURE_RIGHT,
    GESTURE_CW      // 顺时针画圈
} Gesture_t;

void DRV_GestureInit();         // 初始化手势传感器
Gesture_t DRV_GetGesture();     // 获取最新识别到的手势

// --- 步进电机动作 ---
void DRV_StepperRun(bool forward);
void DRV_StepperStop();
void DRV_StepperManage(); // 必须在 loop() 中高频调用，填充步进脉冲队列

// --- 底层通信封装 ---
void mcp_write_register(uint8_t reg, uint8_t value);
uint8_t mcp_read_register(uint8_t reg);

// --- 智能探针系统 ---
// 传感器状态结构体
typedef struct {
    bool ips_0;
    bool ips_90;
    bool ips_180;
    bool ips_bldc;
} SensorState_t;

// 获取当前传感器状态
SensorState_t DRV_GetSensorState();

// 智能探针：只在状态变化或异常时输出
// stage: 当前阶段描述
// expected: 预期状态（可选，传 NULL 则只做变化检测）
void DRV_SmartProbe(const char* stage, SensorState_t* expected = NULL);

#endif
