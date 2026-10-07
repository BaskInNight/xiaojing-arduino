#pragma once
#include <Arduino.h>
#include "HardwareDriver.h"

// 🚀 暴露给 main.cpp 的逻辑开关 (extern 声明)
extern volatile bool webControlActive;
extern volatile bool webStopActive;

// 网页子系统初始化入口
void WEB_Init();

// 系统状态结构体（供Web显示用）
typedef struct {
    // 洗涤状态
    bool isWashing;           // 是否正在洗涤
    const char* currentStage; // 当前阶段名称
    uint32_t elapsedTime;     // 已用时间(秒)
    int queueRemaining;       // 队列剩余步骤
    int totalSteps;           // 总步骤数
    const char* queuePreview; // 队列预览文本
    const char* currentStepName; // 当前步骤名称

    // 传感器数据
    bool ips_0;               // 0°传感器
    bool ips_90;              // 90°传感器
    bool ips_180;             // 180°传感器
    bool ips_bldc;            // BLDC传感器
    bool waterFull;           // 水位满
    float turbidity;          // 浊度(NTU)

    // 电机状态
    DrumPosture posture;      // 当前姿态
    bool blcdRunning;         // BLDC是否运行

    // 系统信息
    uint32_t freeHeap;        // 空闲堆内存
    uint32_t freePsram;       // 空闲PSRAM
    int wifiRSSI;             // WiFi信号强度
} WebStatus_t;

// 更新Web状态（由主循环调用）
void WEB_UpdateStatus(const WebStatus_t* status);