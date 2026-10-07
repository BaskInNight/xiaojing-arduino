#ifndef DISPLAY_DRIVER_H
#define DISPLAY_DRIVER_H

#include <Arduino.h>
#include "HardwareDriver.h"
#include "WashProtocol.h"

// 页面索引
#define PAGE_STATUS   0  // 系统状态总览
#define PAGE_WASH     1  // 洗涤进度
#define PAGE_VOICE    2  // 语音助手
#define PAGE_COUNT    3

// 语音助手状态
typedef enum {
    VOICE_IDLE,       // 待命：等待唤醒
    VOICE_AWAKE,      // 已唤醒：播放提示音
    VOICE_LISTENING,  // 录音中：VAD 截断
    VOICE_THINKING,   // 云端处理中
    VOICE_REPLY,      // TTS 回复中
    VOICE_DONE        // 交互结束，准备回退
} VoiceState_t;

// 初始化 TFT + LVGL
void DISP_Init();

// 主循环调用（lv_timer_handler）
void DISP_Handler();

// 手势翻页
void DISP_SwitchPage(bool next);

// 更新 Page 1: 系统状态
void DISP_UpdateStatus(bool bleConnected, bool wifiConnected, bool isWashing);

// 更新 Page 2: 洗涤进度
void DISP_UpdateWashProgress(int progress, const char* stepName, int queueSize, DrumPosture posture, bool isWashing);

// 更新 Page 3: 语音助手状态
void DISP_SetVoiceState(VoiceState_t state, const char* text = NULL);
VoiceState_t DISP_GetVoiceState();

// 获取当前步骤名称
const char* DISP_GetCmdName(WashCmd_t cmd);

#endif
