// 路径：lib/Driver_ESC/ESC_Driver.h
#ifndef ESC_DRIVER_H
#define ESC_DRIVER_H

#include <Arduino.h>

// --- 通讯协议常量定义 ---
#define ESC_START_BYTE 0xBC
#define ESC_END_BYTE   0x16

#define CMD_WRITE      102  // 0x66 写命令
#define CMD2_SET_DATA  3    // 0x03 设置数据

// 电机方向枚举
enum MotorDir {
    DIR_CW = 0,  // 正转 (例如：波轮顺时针)
    DIR_CCW = 1  // 反转 (例如：波轮逆时针/脱水)
};

class BrushlessESC {
private:
    HardwareSerial* serialPort;

    // 计算校验和 (CS 前面所有数据的累加和取低8位)
    uint8_t calculateChecksum(uint8_t* frame, uint8_t length) {
        uint32_t sum = 0;
        for (int i = 0; i < length; i++) {
            sum += frame[i];
        }
        return (uint8_t)(sum & 0xFF);
    }

public:
    // 初始化串口
    void begin(HardwareSerial* port, uint32_t baudrate, int rxPin, int txPin) {
        serialPort = port;
        serialPort->begin(baudrate, SERIAL_8N1, rxPin, txPin); 
    }

    /**
     * @brief 发送控制指令给电调
     * @param pwm_percent 目标转速百分比 (0-100)
     * @param dir 方向 DIR_CW 或 DIR_CCW
     * @param brake 是否强制刹车 (true: 刹车, false: 释放)
     */
    void sendControlCmd(uint8_t pwm_percent, MotorDir dir, bool brake) {
        uint8_t frame[14];
        
        frame[0] = ESC_START_BYTE;
        frame[1] = CMD_WRITE;
        frame[2] = CMD2_SET_DATA;
        frame[3] = 0x08; // 后续有效+无效数据长度固定为 8
        
        // Data1 解析 (Bit0:串口控制=1, Bit1:方向, Bit2:刹车)
        uint8_t data1 = 0x01; // bit0 = 1 默认接管串口控制权
        if (dir == DIR_CCW) data1 |= (1 << 1);
        if (brake)          data1 |= (1 << 2);
        frame[4] = data1;
        
        // Data2, Data3: 延时启动时间低位与高位 (设为0，由主控掌握时序)
        frame[5] = 0x00; 
        frame[6] = 0x00; 
        
        // Data4: PWM 设置值 (0-100%)
        frame[7] = (pwm_percent > 100) ? 100 : pwm_percent;
        
        // Data5 ~ Data8: 无效数据占位符 (参考官方文档)
        frame[8] = 0x7C; 
        frame[9] = 0xF9;
        frame[10] = 0x19;
        frame[11] = 0x00;
        
        // 计算校验和 (计算索引 0~11 的和)
        frame[12] = calculateChecksum(frame, 12);
        
        // 结束位
        frame[13] = ESC_END_BYTE;

        // 通过串口发送
        serialPort->write(frame, 14);
    }

    // 快捷方法：急停刹车
    void emergencyStop() {
        sendControlCmd(0, DIR_CW, true);
    }
};

#endif