#include "WashExecutor.h"
#include "HardwareDriver.h"
#include <queue>

// 全局状态变量
static std::queue<WashStep_t> stepQueue;
static WashStep_t currentStep;
static bool isStepActive = false;
static uint32_t stepStartMillis = 0;
static DrumPosture realPosture = POS_UNKNOWN;
static bool isTiltFired = false;
static bool idleCleanupDone = false;  // 队列空闲时 UV/水阀清理标记

// 跨核互斥锁：保护队列与状态变量，防止 Core 0 / Core 1 并发读写
static SemaphoreHandle_t exeMutex = NULL;

// val (角度: 0/90/180) 到 DrumPosture (枚举: 1/2/3) 的映射
static DrumPosture valToPosture(uint16_t val) {
    if (val == 0)   return POS_0;
    if (val == 90)  return POS_90;
    if (val == 180) return POS_180;
    return POS_UNKNOWN;
}

void EXE_Init() {
    // 0. 创建跨核互斥锁
    exeMutex = xSemaphoreCreateMutex();

    // 1. 调用底层驱动初始化（配置引脚、电平等）
    DRV_Init();

    // 2. 压入第一步：步进电机姿态归零 (寻找外筒 0 度 IPS 传感器)
    // 参数说明：{指令ID, 目标参数, 运行时间}
    WashStep_t homeTilt = {CMD_TILT_BARREL, 0, 0};  // val 必须用角度值 0，不是枚举 POS_0(=1)
    EXE_PushStep(homeTilt);

    // 3. 压入第二步：BLDC 内筒对齐 (寻找内筒磁铁寻零)
    // 只有在外筒回到 0 度位置后，进行内筒对齐才是最平稳、安全的
    WashStep_t homeBLDC = {CMD_HOME_BLDC, 0, 0};
    EXE_PushStep(homeBLDC);

    Serial.println("⚙️ [系统初始化] 已成功排队“外筒归零”与“内筒对齐”自检任务。");
}

void EXE_PushStep(WashStep_t step) {
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        if (stepQueue.size() >= MAX_QUEUE_SIZE) {
            Serial.printf("⚠️ [队列已满] 丢弃指令 0x%02X，队列上限 %d 条\n", step.cmd_id, MAX_QUEUE_SIZE);
        } else {
            stepQueue.push(step);
        }
        xSemaphoreGive(exeMutex);
    }
}

void EXE_ClearQueue() {
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        while(!stepQueue.empty()) stepQueue.pop();
        isStepActive = false;
        isTiltFired = false;
        xSemaphoreGive(exeMutex);
    }
    DRV_WaterValve(false);
    DRV_UVLamp(false);
    washerMotor.brake();
    DRV_StepperStop();
}

void EXE_Handler() {
    // ==========================================
    // 1. 取新任务逻辑 (已移除安全前置拦截，方便干跑测试)
    // ==========================================
    if (exeMutex && xSemaphoreTake(exeMutex, 0) == pdTRUE) {
        if (!isStepActive && !stepQueue.empty()) {
            WashStep_t nextStep = stepQueue.front();
            currentStep = nextStep;
            stepQueue.pop();
            isStepActive = true;
            stepStartMillis = millis();

            Serial.printf("\n🚀 [执行器] 开始执行指令: 0x%02X, 参数: %d, 时长: %d\n",
                            currentStep.cmd_id, currentStep.val, currentStep.time);

            char msg[32]; snprintf(msg, sizeof(msg), "新指令 0x%02X", currentStep.cmd_id);
            DRV_SmartProbe(msg);

            if (currentStep.cmd_id == CMD_TILT_BARREL) {
                washerMotor.brake();
            }
        }
        xSemaphoreGive(exeMutex);
    }

    // 🛡️ 盲区修复 1：任务队列为空、机器完全待机时
    if (!isStepActive) {
        washerMotor.brake(); // 死死锁住内筒，防止电调被空间噪声唤醒
        // 安全兜底：队列空闲时确保 UV 和水阀关闭（只执行一次）
        if (!idleCleanupDone) {
            DRV_UVLamp(false);
            DRV_WaterValve(false);
            Serial.println("[UV] cleanup off (queue empty)");
            idleCleanupDone = true;
        }
        return;
    }
    // 有任务执行中，重置清理标记
    idleCleanupDone = false;

    // 💉 智能探针：只在状态变化或异常时输出
    static uint32_t last_probe_time = 0;
    if (millis() - last_probe_time > 500) {  // 500ms 检查一次，但只在变化时输出
        DRV_SmartProbe("执行中");
        last_probe_time = millis();
    }

    uint32_t elapsed_ms = millis() - stepStartMillis;

    if (currentStep.cmd_id == CMD_WATER_IN || 
        currentStep.cmd_id == CMD_TILT_BARREL || 
        currentStep.cmd_id == CMD_TURBIDITY_CHECK) {
        
        washerMotor.brake(); 
    }

    switch (currentStep.cmd_id) {
        
        case CMD_WATER_IN: {
            DRV_WaterValve(true);
            DRV_UVLamp(currentStep.val == 1);
            
            // 结束条件：水位满 或 强制超时 (增加 500ms 防抖，防止水波纹误触)
            if ((DRV_IsWaterFull() && elapsed_ms > 500) || elapsed_ms >= (uint32_t)MAX_WATER_IN_SEC * 1000) {
                DRV_WaterValve(false);
                isStepActive = false;
                Serial.println("✅ [执行器] 进水结束");
            }
            break;
        }

        case CMD_TILT_BARREL: {
            DrumPosture targetPosture = valToPosture(currentStep.val);

            // 构建预期状态：只有目标传感器应该触发
            SensorState_t expected = {false, false, false, false};
            if (targetPosture == POS_0) expected.ips_0 = true;
            else if (targetPosture == POS_90) expected.ips_90 = true;
            else if (targetPosture == POS_180) expected.ips_180 = true;

            // 点火前先检查：目标传感器已触发则直接跳过，无需移动
            if (!isTiltFired) {
                if (DRV_ReadIPS(targetPosture)) {
                    Serial.printf("✅ [执行器] 目标姿态 %d 已到达，跳过步进。\n", currentStep.val);
                    realPosture = targetPosture;
                    isStepActive = false;
                    isTiltFired = false;
                    break;
                }
                DRV_SmartProbe("步进点火前夕", &expected);
                bool isForward = (targetPosture == POS_0) ? false : (targetPosture > realPosture);
                DRV_StepperRun(isForward);
                Serial.printf("🚀 [执行器] 点火 目标姿态:%d, 方向:%s\n", currentStep.val, isForward ? "正向" : "反向");
                isTiltFired = true;
            }

            // 直接检查 IPS 传感器，消抖已能抗噪，无需额外 shield 延迟
            if (DRV_ReadIPS(targetPosture)) {
                DRV_SmartProbe("到达目标", &expected);
                DRV_StepperStop();
                realPosture = targetPosture;
                isStepActive = false;
                isTiltFired = false;
                Serial.println("🎯 [执行器] 到达目标姿态，电机刹停。");
            }

            uint32_t max_timeout = (currentStep.val == 0) ? 80000 : 40000;
            if (elapsed_ms >= max_timeout) {
                DRV_SmartProbe("超时卡死！", &expected);
                DRV_StepperStop();
                isStepActive = false;
                isTiltFired = false;
                Serial.println("❌ [执行器] 换筒超时卡死");
                EXE_ClearQueue();
            }
            break;
        }

        case CMD_WASH_PULSATOR: {
            uint32_t target_ms = (uint32_t)currentStep.time * 1000;

            if (elapsed_ms < target_ms) {
                // [执行期] 6 阶段软起步换向周期 (总 9000ms)
                uint32_t cycle = elapsed_ms % 9000;
                uint8_t speed = (currentStep.val == 1) ? 35 : (currentStep.val == 3) ? 65 : 50;

                // 阶段定义
                // 0–1000ms:   CW_SOFT   — 低速起步 25%
                // 1000–3000ms: CW_RUN   — 目标速度正转
                // 3000–4500ms: PREP_CCW — 刹车 + 预置反转方向
                // 4500–5500ms: CCW_SOFT  — 低速起步 25%
                // 5500–7500ms: CCW_RUN  — 目标速度反转
                // 7500–9000ms: PREP_CW  — 刹车 + 预置正转方向

                static uint8_t lastPulsatorPhase = 0xFF;
                uint8_t phase;
                if      (cycle < 1000) phase = 0; // CW_SOFT
                else if (cycle < 3000) phase = 1; // CW_RUN
                else if (cycle < 4500) phase = 2; // PREP_CCW
                else if (cycle < 5500) phase = 3; // CCW_SOFT
                else if (cycle < 7500) phase = 4; // CCW_RUN
                else                   phase = 5; // PREP_CW

                // 先执行电机控制
                switch (phase) {
                    case 0: washerMotor.runCW(25);          break; // CW_SOFT
                    case 1: washerMotor.runCW(speed);       break; // CW_RUN
                    case 2: washerMotor.brakeThenCCW();     break; // PREP_CCW
                    case 3: washerMotor.runCCW(25);         break; // CCW_SOFT
                    case 4: washerMotor.runCCW(speed);      break; // CCW_RUN
                    case 5: washerMotor.brakeThenCW();      break; // PREP_CW
                }

                // 控制之后打印，zf 反映的是刚设置后的真实状态
                if (phase != lastPulsatorPhase) {
                    lastPulsatorPhase = phase;
                    const char* phaseName[] = {"CW_SOFT", "CW_RUN", "PREP_CCW", "CCW_SOFT", "CCW_RUN", "PREP_CW"};
                    uint8_t dispSpeed = (phase <= 1) ? (phase == 0 ? 25 : speed)
                                    : (phase >= 3 && phase <= 4) ? (phase == 3 ? 25 : speed) : 0;
                    uint8_t zfRead = digitalRead(BLDC_ZF_PIN);
                    Serial.printf("[PULSATOR] phase=%s cycle=%lu elapsed=%lu speed=%d duty=%d zf=%s\n",
                        phaseName[phase], (unsigned long)cycle, (unsigned long)elapsed_ms,
                        dispSpeed, map(dispSpeed, 0, 100, 0, 255),
                        zfRead ? "HIGH" : "LOW");
                }
            } else {
                // 🛑 [刹车冷却期] 核心修复：强制等待物理惯性消除！
                washerMotor.brake(); // 持续发送刹车指令
                
                if (elapsed_ms >= target_ms + 3000) { // 强制等待 3 秒钟
                    isStepActive = false; // 只有 3 秒完全停稳后，才允许进行下一个动作
                    Serial.println("✅ [执行器] 波轮洗完全刹停");
                }
            }
            break;
        }

        case CMD_WASH_DRUM: {

            uint32_t target_ms = (uint32_t)currentStep.time * 1000;

            if (elapsed_ms < target_ms) {

                uint8_t target_speed = (currentStep.val == 1) ? 25 : 40;
                uint8_t current_speed = target_speed;

                // 🚀 恢复你之前验证过【完美生效】的 90度 起步逻辑！
                if (elapsed_ms < 500) {
                    // 阶段 1：解锁期。用 15 骗过电调的防飞车保护，建立初始磁场
                    current_speed = 15;
                } else if (elapsed_ms < 3000) {
                    // 阶段 2：阶梯攀升期。从 20 开始克服重力，每 500ms 上一个台阶
                    uint8_t base_speed = 20;
                    uint32_t step = (elapsed_ms - 500) / 500; // 0, 1, 2, 3, 4
                    uint8_t speed_increment = ((target_speed - base_speed) * step) / 4;
                    current_speed = base_speed + speed_increment;
                }

                washerMotor.runCW(current_speed);

            } else {

                washerMotor.brake();
                if (elapsed_ms >= target_ms + 3000) {
                    isStepActive = false;
                    Serial.println("✅ [执行器] 滚筒洗完全刹停");
                }
            }

            break;
        }

        case CMD_SPIN_DRY: {
            uint32_t target_ms = (uint32_t)currentStep.time * 1000;
            
            if (elapsed_ms < target_ms) {
                // 脱水目标转速：根据参数选择 40 或 85 (最高可调至 90)
                uint8_t target_speed = (currentStep.val == 1) ? 40 : 85; 
                uint8_t current_speed = target_speed;
                
                // 🚀 完全克隆滚筒洗的“三段式”成功逻辑
                if (elapsed_ms < 500) {
                    // 阶段 1：解锁期。用 15 骗过电调保护，这对你的硬件环境至关重要
                    current_speed = 15;
                } else if (elapsed_ms < 5000) { 
                    // 阶段 2：斜坡爬升期。
                    // 考虑到 180度 摩擦力大，起步基准从 20 提到 25
                    uint8_t base_speed = 25; 
                    
                    // 将爬升期稍微拉长到 4.5 秒 (500ms~5000ms)，给重载脱水更多缓冲
                    // 分成 9 个台阶，每 500ms 迈一步
                    uint32_t step = (elapsed_ms - 500) / 500; 
                    uint8_t speed_increment = ((target_speed - base_speed) * step) / 8;
                    current_speed = base_speed + speed_increment;
                }
                
                washerMotor.runCW(current_speed);
            } else {
                // 🛑 刹车期：脱水动能大，维持原有的 5 秒刹车保护
                washerMotor.brake();
                if (elapsed_ms >= target_ms + 5000) { 
                    isStepActive = false;
                    Serial.println("✅ [执行器] 脱水完全刹停");
                }
            }
            break;
        }

        case CMD_HOME_BLDC: {
            static uint32_t bldc_trigger_ms = 0; // 用独立变量存触发时刻，避免覆盖 time 字段

            // 预期状态：寻零过程中，BLDC 传感器应该最终触发
            SensorState_t expected_bldc = {false, false, false, true};

            if (currentStep.val == 0) {
                if (elapsed_ms < 1000) {
                    washerMotor.runCW(25);
                } else {
                    washerMotor.runCW(15);
                }

                if (elapsed_ms < 50 || elapsed_ms % 2000 < 50) {
                    Serial.printf("🔧 [BLDC] runCW called, elapsed=%lu, speed=%d, PWM duty=%d\n",
                        elapsed_ms, elapsed_ms < 1000 ? 25 : 15,
                        map(elapsed_ms < 1000 ? 25 : 15, 0, 100, 0, 255));
                }

                // 每 500ms 检查一次 BLDC 传感器状态
                static uint32_t last_bldc_probe = 0;
                if (millis() - last_bldc_probe > 500) {
                    DRV_SmartProbe("BLDC寻零中", &expected_bldc);
                    last_bldc_probe = millis();
                }

                if (DRV_ReadIPS(21)) {
                    if (elapsed_ms > 200) {
                        currentStep.val = 1;
                        bldc_trigger_ms = elapsed_ms;
                        DRV_SmartProbe("BLDC磁铁触发", &expected_bldc);
                        Serial.println("🎯 [执行器] 触发磁铁，开始强力刹车锁定...");
                    }
                } else if (elapsed_ms >= 15000) {
                    washerMotor.brake();
                    isStepActive = false;
                    DRV_SmartProbe("BLDC寻零超时！", &expected_bldc);
                    Serial.println("❌ [执行器] BLDC寻零超时");
                }
            }
            else if (currentStep.val == 1) {
                washerMotor.brake();
                if (elapsed_ms - bldc_trigger_ms >= 2000) {
                    isStepActive = false;
                    DRV_SmartProbe("BLDC锁定完成", &expected_bldc);
                    Serial.println("✅ [执行器] BLDC内筒对齐刹停");
                }
            }
            break;
        }

        case CMD_TURBIDITY_CHECK: {
            Serial.println("🔍 [执行器] 开始检测浊度...");
            float ntu = DRV_GetTurbidity();
            Serial.printf("   -> 当前浊度: %.1f NTU\n", ntu);

            // 浊度阈值可配置，val 字段表示是否强制重洗（1=强制，0=自动判断）
            float threshold = 800.0;
            bool force_rewash = (currentStep.val == 1);

            if (ntu > threshold || force_rewash) {
                if (ntu > threshold) {
                    Serial.printf("⚠️ [执行器] 浊度 %.1f 超过阈值 %.1f，触发重洗！\n", ntu, threshold);
                } else {
                    Serial.println("⚠️ [执行器] 强制重洗模式！");
                }

                // 重洗序列：转回0度 → 进水+UV → 强力波轮洗 → 转180度 → 脱水
                EXE_PushStep({CMD_TILT_BARREL, 0, 0});
                EXE_PushStep({CMD_WATER_IN, 1, 30});
                EXE_PushStep({CMD_WASH_PULSATOR, 3, 30});
                EXE_PushStep({CMD_TILT_BARREL, 180, 0});
                EXE_PushStep({CMD_SPIN_DRY, 2, 30});
            } else {
                Serial.printf("✅ [执行器] 浊度 %.1f 合格，继续下一步。\n", ntu);
            }

            isStepActive = false;
            break;
        }

        case CMD_UV_STERILIZE: {
            // val=0: 关闭 UV，立即完成
            // val=1: 打开 UV，持续 time 秒后关闭
            uint32_t duration_sec = currentStep.time;
            if (currentStep.val == 1 && duration_sec == 0) {
                duration_sec = 60;  // 默认 60 秒，避免一直亮
                Serial.printf("[UV] time=0, using default %us\n", duration_sec);
            }

            if (currentStep.val == 0) {
                // 关闭 UV，立即完成
                DRV_UVLamp(false);
                Serial.println("[UV] lamp OFF (val=0)");
                isStepActive = false;
            } else {
                // val=1: 打开 UV，持续 time 秒后关闭
                if (elapsed_ms == 0) {
                    DRV_UVLamp(true);
                    Serial.printf("[UV] CMD_UV_STERILIZE start val=1 time=%us\n", duration_sec);
                    Serial.println("[UV] lamp ON");
                }
                if (elapsed_ms >= (uint32_t)duration_sec * 1000) {
                    DRV_UVLamp(false);
                    Serial.println("[UV] lamp OFF (duration complete)");
                    isStepActive = false;
                }
            }
            break;
        }

        default:
            isStepActive = false;
            break;
    }
}

DrumPosture EXE_GetCurrentPosture() {
    return realPosture;
}

int EXE_GetQueueSize() {
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        int size = stepQueue.size();
        xSemaphoreGive(exeMutex);
        return size;
    }
    return 0;
}

bool EXE_IsWashing() {
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        bool washing = isStepActive || !stepQueue.empty();
        xSemaphoreGive(exeMutex);
        return washing;
    }
    return false;
}

// 步骤名称映射
static const char* cmdToName(WashCmd_t cmd) {
    switch (cmd) {
        case CMD_WATER_IN:       return "进水";
        case CMD_WASH_PULSATOR:  return "波轮洗";
        case CMD_TILT_BARREL:    return "换筒";
        case CMD_WASH_DRUM:      return "滚筒洗";
        case CMD_SPIN_DRY:       return "脱水";
        case CMD_HOME_BLDC:      return "BLDC归位";
        case CMD_TURBIDITY_CHECK: return "浊度检测";
        case CMD_UV_STERILIZE:   return "UV杀菌";
        default:                 return "待机";
    }
}

const char* EXE_GetCurrentStepName() {
    if (!isStepActive) return "待机";
    return cmdToName(currentStep.cmd_id);
}

const char* EXE_PeekNextStepName() {
    const char* name = NULL;
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        if (!stepQueue.empty()) {
            name = cmdToName(stepQueue.front().cmd_id);
        }
        xSemaphoreGive(exeMutex);
    }
    return name;
}

void EXE_GetQueuePreview(char* buf, size_t bufSize) {
    buf[0] = '\0';
    if (exeMutex && xSemaphoreTake(exeMutex, portMAX_DELAY) == pdTRUE) {
        // 复制队列内容用于预览（不修改原队列）
        std::queue<WashStep_t> temp = stepQueue;
        xSemaphoreGive(exeMutex);

        size_t pos = 0;
        int count = 0;
        while (!temp.empty() && count < 4) {
            const char* name = cmdToName(temp.front().cmd_id);
            int written = snprintf(buf + pos, bufSize - pos, "%s%s",
                count > 0 ? " " : "", name);
            if (written > 0 && (size_t)written < bufSize - pos) pos += written;
            temp.pop();
            count++;
        }
        if (!temp.empty()) {
            snprintf(buf + pos, bufSize - pos, " +%d", (int)temp.size());
        }
    }
}