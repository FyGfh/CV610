/**
 * @file air8000_protocol.c
 * @brief Air8000 协议实现源文件
 * @details 包含了 Air8000 通信协议的核心实现，包括 CRC 计算、帧处理、序列号生成、数据转换以及帧构建和解析辅助函数
 * 
 * 协议设计架构：
 * 1. **帧格式**：采用固定格式的二进制帧，包含帧头、数据和 CRC 校验
 * 2. **帧结构**：
 *    - 帧头：9字节，包含同步字符、版本、类型、序列号、命令码和数据长度
 *    - 数据：变长，根据命令类型不同而不同
 *    - CRC：2字节，采用 MODBUS CRC16 算法
 * 3. **命令体系**：
 *    - 系统命令（0x00xx）：PING、版本查询、复位等
 *    - 查询命令（0x01xx）：电源、状态、网络查询等
 *    - 电机命令（0x30xx）：旋转、使能、禁用等
 *    - 传感器命令（0x40xx）：温度、传感器数据读取等
 *    - 设备命令（0x50xx）：LED、风扇、激光控制等
 *    - 文件传输命令（0x60xx）：文件传输请求、数据、确认等
 * 4. **实现特点**：
 *    - 支持大端序/小端序自动转换
 *    - 提供帧构建和解析的辅助函数
 *    - 采用序列号机制确保请求-响应匹配
 *    - 提供完整的 CRC 校验实现
 */

#include "air8000_protocol.h"    /* 协议头文件，包含常量定义和函数声明 */
#include <string.h>             /* 字符串处理函数，如 memcpy、memset */
#include <stdlib.h>             /* 动态内存分配函数，如 malloc、free */
#include <stdio.h>              /* 标准输入输出，如 snprintf */

// ==================== 内部变量 ====================

/**
 * @brief 序列号计数器
 * @details 用于生成帧序列号，范围 0-255，循环递增
 */
static uint8_t g_seq_counter = 0;

// ==================== 基础函数实现 ====================

/**
 * @brief 生成下一个帧序列号
 * @return 下一个序列号，范围 0-255，循环递增
 * @details 使用内部静态变量 g_seq_counter 实现，每次调用自动递增并返回当前值
 */
uint8_t air8000_next_seq(void) {
    return g_seq_counter++;
}

/**
 * @brief CRC-16/MODBUS 校验计算
 * @param data 要计算 CRC 的数据指针
 * @param len 数据长度，单位字节
 * @return 计算得到的 CRC-16 值
 * @details 采用 MODBUS 标准的 CRC-16 算法，初始值为 0xFFFF，多项式为 0xA001
 */
uint16_t air8000_crc16_modbus(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;       /* 初始 CRC 值 */
    
    /* 遍历每个字节 */
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i]; /* 与当前字节异或 */
        
        /* 处理每个比特位 */
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                /* 最低位为 1，右移后与多项式异或 */
                crc = (crc >> 1) ^ 0xA001;
            } else {
                /* 最低位为 0，仅右移 */
                crc >>= 1;
            }
        }
    }
    
    return crc;                  /* 返回计算结果 */
}

/**
 * @brief 初始化帧结构
 * @param frame 要初始化的帧指针
 * @details 将帧结构的所有字段初始化为 0，并设置默认版本号
 */
void air8000_frame_init(air8000_frame_t *frame) {
    if (frame) {
        memset(frame, 0, sizeof(air8000_frame_t));  /* 将整个帧结构清零 */
        frame->version = AIR8000_VERSION;           /* 设置默认协议版本 */
    }
}

/**
 * @brief 释放帧内部动态分配的内存
 * @param frame 要清理的帧指针
 * @details 如果帧中包含动态分配的数据，释放该内存并重置相关字段
 */
void air8000_frame_cleanup(air8000_frame_t *frame) {
    if (frame) {
        if (frame->data) {                          /* 检查数据指针是否有效 */
            free(frame->data);                      /* 释放动态分配的内存 */
            frame->data = NULL;                     /* 将指针置为 NULL，避免野指针 */
            frame->data_len = 0;                    /* 重置数据长度为 0 */
        }
    }
}

/**
 * @brief 编码帧数据
 * @param frame 要编码的帧指针
 * @param buffer 输出缓冲区指针
 * @param max_len 输出缓冲区最大长度
 * @return 成功返回编码后的总长度，失败返回 -1
 * @details 将帧结构编码为二进制数据，包括帧头、数据和 CRC 校验
 */
int air8000_frame_encode(const air8000_frame_t *frame, uint8_t *buffer, size_t max_len) {
    /* 计算总帧长：帧头 + 数据长度 + CRC 长度 */
    size_t total_len = AIR8000_HEADER_SIZE + frame->data_len + AIR8000_CRC_SIZE;
    if (total_len > max_len) {
        return -1;  /* 缓冲区长度不足，编码失败 */
    }

    /* 填充帧头字段 */
    buffer[0] = AIR8000_SYNC1;                 /* 同步字符 1 */
    buffer[1] = AIR8000_SYNC2;                 /* 同步字符 2 */
    buffer[2] = frame->version;                /* 协议版本 */
    buffer[3] = frame->type;                   /* 帧类型 */
    buffer[4] = frame->seq;                    /* 帧序列号 */
    buffer[5] = (frame->cmd >> 8) & 0xFF;     /* 命令码高位字节 */
    buffer[6] = (frame->cmd) & 0xFF;           /* 命令码低位字节 */
    buffer[7] = (frame->data_len >> 8) & 0xFF; /* 数据长度高位字节 */
    buffer[8] = (frame->data_len) & 0xFF;       /* 数据长度低位字节 */

    /* 复制数据字段 */
    if (frame->data_len > 0 && frame->data != NULL) {
        memcpy(&buffer[AIR8000_HEADER_SIZE], frame->data, frame->data_len);
    }

    /* 计算并添加 CRC 校验 */
    /* CRC 计算范围：从版本字段开始到数据结束 */
    uint16_t crc = air8000_crc16_modbus(&buffer[2], (AIR8000_HEADER_SIZE - 2) + frame->data_len);
    
    /* CRC 存储位置：帧头 + 数据长度 */
    size_t crc_offset = AIR8000_HEADER_SIZE + frame->data_len;
    buffer[crc_offset] = (crc >> 8) & 0xFF;     /* CRC 高位字节 */
    buffer[crc_offset + 1] = (crc) & 0xFF;       /* CRC 低位字节 */

    return (int)total_len;  /* 返回编码后的总长度 */
}

/**
 * @brief 解析帧数据
 * @param buffer 要解析的原始数据指针
 * @param len 原始数据长度
 * @param frame 输出帧指针
 * @return 成功返回解析出的帧长度，失败返回负数
 * @details 从原始数据中解析出完整的帧，包括帧头、数据和 CRC 校验
 */
int air8000_frame_parse(const uint8_t *buffer, size_t len, air8000_frame_t *frame) {
    /* 检查参数有效性 */
    if (!frame) {
        return -1;  /* 帧指针为NULL，返回错误 */
    }

    /* 清理帧中可能存在的旧数据 */
    if (frame->data) {
        free(frame->data);
        frame->data = NULL;
        frame->data_len = 0;
    }

    /* 检查数据长度是否至少包含最小帧大小 */
    if (len < AIR8000_MIN_FRAME) {
        return -1;  /* 数据不完整，返回 -1 */
    }

    /* 检查同步字 */
    if (buffer[0] != AIR8000_SYNC1 || buffer[1] != AIR8000_SYNC2) {
        return -2;  /* 帧头无效，返回 -2 */
    }

    /* 解析数据长度 */
    uint16_t data_len = ((uint16_t)buffer[7] << 8) | buffer[8];
    size_t total_len = AIR8000_HEADER_SIZE + data_len + AIR8000_CRC_SIZE;

    /* 检查数据是否包含完整的帧 */
    if (len < total_len) {
        return -1;  /* 数据不完整，返回 -1 */
    }

    // 注意：此处我们不强制验证 CRC，因为 Air8000 设备通常不验证 CRC，但为了健壮性，上层可以选择验证
    // 这里只做字段解析
    
    /* 填充帧字段 */
    frame->version = buffer[2];
    frame->type = buffer[3];
    frame->seq = buffer[4];
    frame->cmd = ((uint16_t)buffer[5] << 8) | buffer[6];
    frame->data_len = data_len;
    
    /* 动态分配内存并复制数据，避免零拷贝导致的内存释放错误 */
    if (data_len > 0) {
        frame->data = (uint8_t *)malloc(data_len);
        if (frame->data) {
            memcpy(frame->data, &buffer[AIR8000_HEADER_SIZE], data_len);
        } else {
            // 修复：内存分配失败时，将data_len设为0，避免后续访问无效内存
            frame->data = NULL;
            frame->data_len = 0;
        }
    } else {
        frame->data = NULL;
    }

    return (int)total_len;  /* 返回解析出的帧长度 */
}

/**
 * @brief 浮点转换联合体
 * @details 用于在浮点数和 32 位整数之间进行类型转换
 */
typedef union {
    float f;    /* 浮点数类型 */
    uint32_t u; /* 32 位无符号整数类型 */
} float_union_t;

/**
 * @brief 主机字节序转网络字节序（浮点型）
 * @param val 主机字节序的浮点值
 * @return 网络字节序的浮点值（32 位整数表示）
 * @details 将主机字节序的浮点数转换为网络字节序（大端序）
 */
uint32_t air8000_htonf(float val) {
    float_union_t fu;
    fu.f = val;                 /* 将浮点数存入联合体 */
    uint32_t u = fu.u;           /* 获取浮点数的 32 位整数表示 */
    
    /* 检查系统字节序：1 在小端系统中是 0x01 00 00 00 */
    uint16_t check = 1;
    if (*(uint8_t*)&check == 1) {
        // 小端系统 -> 转为大端
        return ((u & 0xFF000000) >> 24) |  /* 最高字节移到最低位 */
               ((u & 0x00FF0000) >> 8)  |  /* 次高字节移到次低位 */
               ((u & 0x0000FF00) << 8)  |  /* 次低字节移到次高位 */
               ((u & 0x000000FF) << 24);   /* 最低字节移到最高位 */
    } else {
        // 大端系统，直接返回
        return u;
    }
}

/**
 * @brief 网络字节序转主机字节序（浮点型）
 * @param val 网络字节序的浮点值（32 位整数表示）
 * @return 主机字节序的浮点值
 * @details 将网络字节序（大端序）的浮点数转换为主机字节序
 */
float air8000_ntohf(uint32_t val) {
    uint32_t u = val;
    uint16_t check = 1;
    if (*(uint8_t*)&check == 1) {
        // 小端系统 -> 转换回来
        u = ((val & 0xFF000000) >> 24) |  /* 最高字节移到最低位 */
            ((val & 0x00FF0000) >> 8)  |  /* 次高字节移到次低位 */
            ((val & 0x0000FF00) << 8)  |  /* 次低字节移到次高位 */
            ((val & 0x000000FF) << 24);   /* 最低字节移到最高位 */
    }
    
    float_union_t fu;
    fu.u = u;                   /* 将转换后的 32 位整数存入联合体 */
    return fu.f;                /* 返回浮点数 */
}

// ==================== 帧构建辅助函数 ====================

/**
 * @brief 构建通用请求帧
 * @param frame 帧对象指针
 * @param cmd 命令码
 * @param data 数据内容指针
 * @param len 数据长度
 * @details 构建一个通用的请求帧，设置帧类型、序列号、命令码和数据
 */
void air8000_build_request(air8000_frame_t *frame, uint16_t cmd, const uint8_t *data, size_t len) {
    air8000_frame_init(frame);             /* 初始化帧结构 */
    frame->type = FRAME_TYPE_REQUEST;       /* 设置帧类型为请求 */
    frame->seq = air8000_next_seq();        /* 生成并设置帧序列号 */
    frame->cmd = cmd;                      /* 设置命令码 */
    frame->data_len = (uint16_t)len;        /* 设置数据长度 */
    
    /* 如果有数据，动态分配内存并复制数据 */
    if (len > 0 && data != NULL) {
        frame->data = (uint8_t *)malloc(len);
        if (frame->data) {
            memcpy(frame->data, data, len); /* 复制数据内容 */
        } else {
            frame->data_len = 0;           /* 内存分配失败，重置数据长度为 0 */
        }
    }
}

// --- 系统命令 ---

/**
 * @brief 构建 PING 请求帧
 * @param frame 帧对象指针
 * @details 构建一个 PING 请求帧，用于测试设备连接
 */
void air8000_build_ping(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_SYS_PING, NULL, 0);
}

/**
 * @brief 构建获取版本请求帧
 * @param frame 帧对象指针
 * @details 构建一个获取设备版本信息的请求帧
 */
void air8000_build_sys_version(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_SYS_VERSION, NULL, 0);
}

/**
 * @brief 构建系统复位请求帧
 * @param frame 帧对象指针
 * @details 构建一个系统复位请求帧，用于重启设备
 */
void air8000_build_sys_reset(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_SYS_RESET, NULL, 0);
}

/**
 * @brief 构建查询电源请求帧
 * @param frame 帧对象指针
 * @details 构建一个查询电源状态的请求帧
 */
void air8000_build_query_power(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_QUERY_POWER, NULL, 0);
}

/**
 * @brief 构建查询状态请求帧
 * @param frame 帧对象指针
 * @details 构建一个查询设备状态的请求帧
 */
void air8000_build_query_status(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_QUERY_STATUS, NULL, 0);
}

/**
 * @brief 构建查询网络请求帧
 * @param frame 帧对象指针
 * @details 构建一个查询网络状态的请求帧
 */
void air8000_build_query_network(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_QUERY_NETWORK, NULL, 0);
}

// --- 电机控制命令 ---

/**
 * @brief 构建电机旋转请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @param angle 目标角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 * @details 构建一个控制电机旋转到指定绝对位置的请求帧
 */
void air8000_build_motor_rotate(air8000_frame_t *frame, uint8_t motor_id, float angle, float velocity) {
    uint8_t buf[9];                          /* 命令数据缓冲区 */
    uint32_t angle_be = air8000_htonf(angle);        /* 角度转换为网络字节序 */
    uint32_t vel_be = air8000_htonf(velocity);      /* 速度转换为网络字节序 */
    
    buf[0] = motor_id;                       /* 电机 ID */
    memcpy(&buf[1], &angle_be, 4);           /* 目标角度，网络字节序 */
    memcpy(&buf[5], &vel_be, 4);             /* 旋转速度，网络字节序 */
    
    air8000_build_request(frame, CMD_MOTOR_ROTATE, buf, 9);
}

/**
 * @brief 构建电机使能请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个使能指定电机的请求帧，默认使用位置速度模式(2)
 */
void air8000_build_motor_enable(air8000_frame_t *frame, uint8_t motor_id) {
    uint8_t mode = 2; // PositionVelocity mode (匹配 Rust SDK 默认值)
    uint8_t buf[2] = { motor_id, mode };     /* 电机 ID 和工作模式 */
    air8000_build_request(frame, CMD_MOTOR_ENABLE, buf, 2);
}

/**
 * @brief 构建电机禁用请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个禁用指定电机的请求帧
 */
void air8000_build_motor_disable(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_DISABLE, &motor_id, 1);
}

/**
 * @brief 构建电机急停请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个立即停止指定电机的请求帧
 */
void air8000_build_motor_stop(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_STOP, &motor_id, 1);
}

/**
 * @brief 构建设置原点请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个将当前位置设为原点的请求帧
 */
void air8000_build_motor_set_origin(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_SET_ORIGIN, &motor_id, 1);
}

/**
 * @brief 构建获取位置请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个获取电机当前位置的请求帧
 */
void air8000_build_motor_get_pos(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_GET_POS, &motor_id, 1);
}

/**
 * @brief 构建设置速度请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @param velocity 目标速度，单位弧度/秒
 * @details 构建一个设置电机速度的请求帧
 */
void air8000_build_motor_set_vel(air8000_frame_t *frame, uint8_t motor_id, float velocity) {
    uint8_t buf[5];                          /* 命令数据缓冲区 */
    uint32_t vel_be = air8000_htonf(velocity);      /* 速度转换为网络字节序 */
    
    buf[0] = motor_id;                       /* 电机 ID */
    memcpy(&buf[1], &vel_be, 4);             /* 目标速度，网络字节序 */
    
    air8000_build_request(frame, CMD_MOTOR_SET_VEL, buf, 5);
}

/**
 * @brief 构建相对旋转请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @param angle 相对旋转角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 * @details 构建一个控制电机相对当前位置旋转的请求帧
 */
void air8000_build_motor_rotate_rel(air8000_frame_t *frame, uint8_t motor_id, float angle, float velocity) {
    uint8_t buf[9];                          /* 命令数据缓冲区 */
    uint32_t angle_be = air8000_htonf(angle);        /* 角度转换为网络字节序 */
    uint32_t vel_be = air8000_htonf(velocity);      /* 速度转换为网络字节序 */
    
    buf[0] = motor_id;                       /* 电机 ID */
    memcpy(&buf[1], &angle_be, 4);           /* 相对旋转角度，网络字节序 */
    memcpy(&buf[5], &vel_be, 4);             /* 旋转速度，网络字节序 */
    
    air8000_build_request(frame, CMD_MOTOR_ROTATE_REL, buf, 9);
}

/**
 * @brief 构建获取所有电机状态请求帧
 * @param frame 帧对象指针
 * @details 构建一个获取所有电机当前状态的请求帧
 */
void air8000_build_motor_get_all(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_MOTOR_GET_ALL, NULL, 0);
}

/**
 * @brief 构建电机供电控制请求帧
 * @param frame 帧对象指针
 * @param power_on 供电状态，true 为开启，false 为关闭
 * @details 构建一个控制电机电源开关的请求帧
 */
void air8000_build_motor_power(air8000_frame_t *frame, bool power_on) {
    uint8_t state = power_on ? 1 : 0;        /* 供电状态，1 为开启，0 为关闭 */
    air8000_build_request(frame, CMD_DEV_MOTOR_POWER, &state, 1);
}

// --- 电机参数命令 ---

/**
 * @brief 构建读取电机寄存器请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @param reg_addr 寄存器地址
 * @details 构建一个读取电机控制器寄存器的请求帧
 */
void air8000_build_motor_read_reg(air8000_frame_t *frame, uint8_t motor_id, uint8_t reg_addr) {
    uint8_t buf[2] = { motor_id, reg_addr }; /* 电机 ID 和寄存器地址 */
    air8000_build_request(frame, CMD_MOTOR_READ_REG, buf, 2);
}

/**
 * @brief 构建写入电机寄存器请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @param reg_addr 寄存器地址
 * @param value 要写入的值
 * @details 构建一个写入电机控制器寄存器的请求帧
 */
void air8000_build_motor_write_reg(air8000_frame_t *frame, uint8_t motor_id, uint8_t reg_addr, float value) {
    uint8_t buf[6];                          /* 命令数据缓冲区 */
    uint32_t val_be = air8000_htonf(value);       /* 寄存器值转换为网络字节序 */
    
    buf[0] = motor_id;                       /* 电机 ID */
    buf[1] = reg_addr;                       /* 寄存器地址 */
    memcpy(&buf[2], &val_be, 4);             /* 寄存器值，网络字节序 */
    
    air8000_build_request(frame, CMD_MOTOR_WRITE_REG, buf, 6);
}

/**
 * @brief 构建保存电机参数到 Flash 请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个将电机参数保存到 Flash 的请求帧
 */
void air8000_build_motor_save_flash(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_SAVE_FLASH, &motor_id, 1);
}

/**
 * @brief 构建刷新电机状态请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个刷新电机状态信息的请求帧
 */
void air8000_build_motor_refresh(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_REFRESH, &motor_id, 1);
}

/**
 * @brief 构建清除电机错误请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机 ID
 * @details 构建一个清除电机错误状态的请求帧
 */
void air8000_build_motor_clear_error(air8000_frame_t *frame, uint8_t motor_id) {
    air8000_build_request(frame, CMD_MOTOR_CLEAR_ERROR, &motor_id, 1);
}

// --- 传感器与设备命令 ---

/**
 * @brief 构建读取传感器请求帧
 * @param frame 帧对象指针
 * @param sensor_id 传感器 ID
 * @details 构建一个读取指定温度传感器的请求帧
 */
void air8000_build_sensor_read(air8000_frame_t *frame, uint8_t sensor_id) {
    air8000_build_request(frame, CMD_SENSOR_READ_TEMP, &sensor_id, 1);
}

/**
 * @brief 构建读取所有传感器请求帧
 * @param frame 帧对象指针
 * @details 构建一个读取所有传感器数据的请求帧
 */
void air8000_build_sensor_read_all(air8000_frame_t *frame) {
    air8000_build_request(frame, CMD_SENSOR_READ_ALL, NULL, 0);
}

/**
 * @brief 构建设备控制请求帧
 * @param frame 帧对象指针
 * @param cmd 设备控制命令码
 * @param device_id 设备 ID
 * @param state 设备状态
 * @details 构建一个控制指定设备状态的请求帧
 */
void air8000_build_dev_ctrl(air8000_frame_t *frame, uint16_t cmd, uint8_t device_id, uint8_t state) {
    uint8_t buf[2] = { device_id, state };   /* 设备 ID 和目标状态 */
    air8000_build_request(frame, cmd, buf, 2);
}

/**
 * @brief 构建获取设备状态请求帧
 * @param frame 帧对象指针
 * @param device_id 设备 ID
 * @details 构建一个获取指定设备状态的请求帧
 */
void air8000_build_dev_get_state(air8000_frame_t *frame, uint8_t device_id) {
    air8000_build_request(frame, CMD_DEV_GET_STATE, &device_id, 1);
}

// ==================== 响应解析辅助函数 ====================

/**
 * @brief 解析版本信息响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出版本信息结构体指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出设备版本信息
 */
int air8000_parse_version(const uint8_t *data, size_t len, air8000_version_t *out) {
    if (len < 3 || !out) return -1;  /* 数据长度不足或输出指针无效，返回失败 */
    
    /* 解析版本号 */
    out->major = data[0];
    out->minor = data[1];
    out->patch = data[2];
    
    /* 解析构建信息 */
    if (len > 3) {
        size_t build_len = len - 3;
        /* 确保不超过输出缓冲区大小 */
        if (build_len >= sizeof(out->build)) {
            build_len = sizeof(out->build) - 1;
        }
        memcpy(out->build, &data[3], build_len);
        out->build[build_len] = '\0';   /* 确保字符串以 NULL 结尾 */
    } else {
        out->build[0] = '\0';           /* 没有构建信息，设置为空字符串 */
    }
    
    return 0;
}

/**
 * @brief 解析网络状态响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出网络状态结构体指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出网络状态信息
 */
int air8000_parse_network_status(const uint8_t *data, size_t len, air8000_network_status_t *out) {
    if (len < 5 || !out) return -1;  /* 数据长度不足或输出指针无效，返回失败 */
    
    /* 解析基本网络参数 */
    out->csq = data[0];             /* 信号强度 */
    out->rssi = (int8_t)data[1];    /* 接收信号强度指示 */
    out->rsrp = (int8_t)data[2];    /* 参考信号接收功率 */
    out->status = data[3];          /* 网络状态码 */
    out->operator_id = data[4];     /* 运营商 ID */
    
    /* 解析 ICCID (20 字节) */
    if (len >= 25) {
        memcpy(out->iccid, &data[5], 20);
        out->iccid[20] = '\0';      /* 确保字符串以 NULL 结尾 */
    } else {
        out->iccid[0] = '\0';        /* 没有 ICCID 信息，设置为空字符串 */
    }
    
    /* 解析 IP 地址 (ICCID 之后) */
    if (len > 25) {
        size_t ip_len = len - 25;
        /* 确保不超过输出缓冲区大小 */
        if (ip_len >= sizeof(out->ip)) {
            ip_len = sizeof(out->ip) - 1;
        }
        memcpy(out->ip, &data[25], ip_len);
        out->ip[ip_len] = '\0';      /* 确保字符串以 NULL 结尾 */
    } else {
        out->ip[0] = '\0';           /* 没有 IP 信息，设置为空字符串 */
    }
    
    return 0;
}

/**
 * @brief 解析电源 ADC 响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出电源 ADC 结构体指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出电源电压信息
 */
int air8000_parse_power_adc(const uint8_t *data, size_t len, air8000_power_adc_t *out) {
    if (len < 4 || !out) return -1;  /* 数据长度不足或输出指针无效，返回失败 */
    
    /* 解析 12V 电压和电池电压，单位毫伏 */
    out->v12_mv = ((uint16_t)data[0] << 8) | data[1];
    out->vbat_mv = ((uint16_t)data[2] << 8) | data[3];
    
    return 0;
}

/**
 * @brief 解析传感器数据响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出传感器数据结构体指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出传感器采集的环境数据
 */
int air8000_parse_sensor_data(const uint8_t *data, size_t len, air8000_sensor_data_t *out) {
    if (len < 5 || !out) return -1;  /* 数据长度不足或输出指针无效，返回失败 */
    
    /* 解析温度数据，原始值单位为 0.1 摄氏度 */
    uint16_t temp_raw = ((uint16_t)data[0] << 8) | data[1];
    out->temperature = (float)temp_raw / 10.0f;
    
    /* 解析湿度、光照和电池电量 */
    out->humidity = data[2];
    out->light = data[3];
    out->battery = data[4];
    
    return 0;
}

/**
 * @brief 解析所有电机状态响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出所有电机状态结构体指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出所有电机的当前状态
 */
int air8000_parse_all_motor_status(const uint8_t *data, size_t len, air8000_all_motor_status_t *out) {
    if (len == 0 || !out) return -1;  /* 数据长度为 0 或输出指针无效，返回失败 */

    size_t count = data[0];           /* 电机数量 */
    out->count = count;

    /* 如果有电机，动态分配内存 */
    if (count > 0) {
        out->motors = (air8000_motor_state_item_t *)malloc(count * sizeof(air8000_motor_state_item_t));
        if (!out->motors) {
            out->count = 0;
            return -1;  /* 内存分配失败，返回失败 */
        }

        size_t offset = 1;           /* 数据偏移量，跳过电机数量字段 */
        /* 解析每个电机的状态 */
        for (size_t i = 0; i < count; i++) {
            if (offset + 4 > len) {
                /* 数据不完整，释放已分配的内存 */
                free(out->motors);
                out->motors = NULL;
                out->count = 0;
                return -1;
            }
            /* 解析单个电机状态：ID、动作、速度 */
            out->motors[i].motor_id = data[offset];
            out->motors[i].action = data[offset + 1];
            out->motors[i].speed = ((uint16_t)data[offset + 2] << 8) | data[offset + 3];
            offset += 4;             /* 移动到下一个电机状态数据 */
        }
    } else {
        out->motors = NULL;          /* 没有电机，设置为 NULL */
    }

    return 0;
}

/**
 * @brief 解析单个电机的浮点型响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机 ID 指针，可为 NULL
 * @param value 输出浮点值指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出单个电机的浮点型参数，如位置、温度等
 */
int air8000_parse_motor_float_resp(const uint8_t *data, size_t len, uint8_t *motor_id, float *value) {
    if (len < 5) return -1;  /* 数据长度不足，返回失败 */
    
    /* 如果需要，输出电机 ID */
    if (motor_id) *motor_id = data[0];
    
    /* 解析浮点值 */
    uint32_t val_u;
    memcpy(&val_u, &data[1], 4);
    if (value) *value = air8000_ntohf(val_u);
    
    return 0;
}

/**
 * @brief 解析电机寄存器读取响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机 ID 指针，可为 NULL
 * @param reg_id 输出寄存器 ID 指针，可为 NULL
 * @param value 输出寄存器值指针
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出电机寄存器的读取结果
 */
int air8000_parse_motor_read_reg(const uint8_t *data, size_t len, uint8_t *motor_id, uint8_t *reg_id, float *value) {
    if (len < 6) return -1;  /* 数据长度不足，返回失败 */
    
    /* 如果需要，输出电机 ID 和寄存器 ID */
    if (motor_id) *motor_id = data[0];
    if (reg_id) *reg_id = data[1];
    
    /* 解析寄存器值 */
    uint32_t val_u;
    memcpy(&val_u, &data[2], 4);
    if (value) *value = air8000_ntohf(val_u);
    
    return 0;
}

/**
 * @brief 解析电机状态刷新响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机 ID 指针，可为 NULL
 * @param pos 输出位置值指针，可为 NULL
 * @param vel 输出速度值指针，可为 NULL
 * @param torque 输出扭矩值指针，可为 NULL
 * @param temp_mos 输出 MOS 温度指针，可为 NULL
 * @param temp_rotor 输出转子温度指针，可为 NULL
 * @param error 输出错误码指针，可为 NULL
 * @param enabled 输出使能状态指针，可为 NULL
 * @return 成功返回 0，失败返回 -1
 * @details 从响应数据中解析出电机的详细状态信息
 */
int air8000_parse_motor_refresh(const uint8_t *data, size_t len, uint8_t *motor_id, float *pos, float *vel, float *torque, uint8_t *temp_mos, uint8_t *temp_rotor, uint8_t *error, bool *enabled) {
    if (len < 17) return -1;  /* 数据长度不足，返回失败 */
    
    /* 如果需要，输出电机 ID */
    if (motor_id) *motor_id = data[0];
    
    uint32_t u;
    
    /* 解析位置值 */
    memcpy(&u, &data[1], 4);
    if (pos) *pos = air8000_ntohf(u);
    
    /* 解析速度值 */
    memcpy(&u, &data[5], 4);
    if (vel) *vel = air8000_ntohf(u);
    
    /* 解析扭矩值 */
    memcpy(&u, &data[9], 4);
    if (torque) *torque = air8000_ntohf(u);
    
    /* 解析温度、错误码和使能状态 */
    if (temp_mos) *temp_mos = data[13];
    if (temp_rotor) *temp_rotor = data[14];
    if (error) *error = data[15];
    if (enabled) *enabled = (data[16] != 0);
    
    return 0;
}
