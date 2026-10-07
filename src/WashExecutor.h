#ifndef WASH_EXECUTOR_H
#define WASH_EXECUTOR_H

#include <Arduino.h>
#include "WashProtocol.h"

// =============================================================================
// 调度引擎与安全中枢 (大脑层)
// =============================================================================

// 最大队列长度限制，防止内存溢出
#define MAX_QUEUE_SIZE 20

// 初始化调度队列与任务
void EXE_Init();

// 接收来自小净的 JSON 指令包
void EXE_PushStep(WashStep_t step);

// 清空当前洗涤序列 (紧急停止用)
void EXE_ClearQueue();

// 核心循环函数 (需在 main.cpp loop 中高频调用)
void EXE_Handler();

// 获取当前姿态
DrumPosture EXE_GetCurrentPosture();

// 获取队列剩余步骤数
int EXE_GetQueueSize();

// 检查是否正在洗涤（有任务在执行或队列中有任务）
bool EXE_IsWashing();

// 获取当前正在执行的步骤名称（如 "波轮洗涤"、"进水" 等）
const char* EXE_GetCurrentStepName();

// 获取队列中下一个步骤的名称，队列为空返回 NULL
const char* EXE_PeekNextStepName();

// 获取队列预览字符串，如 "→波轮洗→换筒→脱水"，bufSize 建议 64+
void EXE_GetQueuePreview(char* buf, size_t bufSize);

#endif
