#ifndef WASH_PROTOCOL_H
#define WASH_PROTOCOL_H

#include <stdint.h>
#include "HardwareDriver.h"

/**
 * @file WashProtocol.h
 * @brief 智能无交叉洗衣机 指令集与任务通信协议
 * @note 连接大模型 JSON 调度与底层 C 状态机的唯一契约
 */

// =============================================================================
// 1. 原子指令集定义 (大模型 Tool Call 映射)
// =============================================================================
typedef enum {
    CMD_NONE           = 0x00,
    CMD_WATER_IN       = 0x01,  // 进水 + UV 控制 (val: 0关UV/1开UV)
    CMD_WASH_PULSATOR  = 0x02,  // 波轮洗涤 (0°, val: 1轻柔/2标准/3强力)
    CMD_TILT_BARREL    = 0x03,  // 姿态切换 (val: 0, 90, 180)
    CMD_WASH_DRUM      = 0x04,  // 滚筒洗涤 (90°, val: 1轻柔/2标准/3强力)
    CMD_SPIN_DRY       = 0x05,  // 离心脱水 (180°, val: 1沥水/2脱水)
    CMD_HOME_BLDC      = 0x06,   // BLDC 内筒对齐寻零
    CMD_TURBIDITY_CHECK = 0x07,  // 浊度检测指令 (val: 0单次检测/1持续监测)
    CMD_UV_STERILIZE   = 0x08   // 洗后紫外杀菌 (val: 0关UV/1开UV, time: 持续秒数)
} WashCmd_t;

// =============================================================================
// 2. 指令消息结构体 (FreeRTOS Queue 传输单元)
// =============================================================================
typedef struct {
    WashCmd_t cmd_id;   // 指令 ID
    uint16_t  val;      // 参数 (强度、角度、开关位)
    uint16_t  time;     // 期望持续时间 (秒)
} WashStep_t;

// =============================================================================
// 3. 物理安全生命线阈值 (绝对硬性约束)
// =============================================================================
#define MAX_WATER_IN_SEC  30    // 进水绝对超时保护 (秒)
#define MAX_TILT_SEC      7     // 换筒步进运动防卡死超时 (秒)
#define MAX_SPIN_SEC      120   // 脱水最大时长限制 (秒)
#define MAX_WASH_SEC      600   // 单步洗涤最大时长限制 (秒)

#endif
