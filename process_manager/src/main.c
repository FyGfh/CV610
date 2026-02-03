/**
 * @file main.c
 * @brief 进程管理主程序
 * @version 1.0
 * @date 2026-01-22
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

#include "process_manager.h"
#include "shared_memory.h"
#include "message_queue.h"

/**
 * @brief 全局变量，用于控制主循环
 */
static volatile bool running = true;

/**
 * @brief 共享内存句柄
 */
static shm_handle_t g_shm_handle;

/**
 * @brief 消息队列描述符
 */
static int g_mq_uart_to_mqtt = -1;
static int g_mq_mqtt_to_uart = -1;

/**
 * @brief 进程指针
 */
static process_t *g_uart_process = NULL;
static process_t *g_mqtt_process = NULL;

/**
 * @brief 序列号计数器
 */
static uint32_t g_seq_num = 0;

/**
 * @brief 辅助函数：从标准输入读取字符串
 * @param buffer 输出缓冲区，用于存储读取的字符串
 * @param len 缓冲区最大长度
 */
void read_input(char *buffer, size_t len) {
    // 清空缓冲区
    memset(buffer, 0, len);
    
    // 读取用户输入，直到成功读取为止
    while (fgets(buffer, len, stdin) == NULL) {
        // 读取失败，重试
        printf("读取输入失败，请重新输入: ");
        // 清空错误标志
        clearerr(stdin);
    }
    
    // 去除末尾的换行符
    size_t l = strlen(buffer);
    if (l > 0 && buffer[l-1] == '\n') {
        buffer[l-1] = '\0';
    }
    
    // 清理输入缓冲区中剩余的字符
    int c;
    while ((c = getchar()) != '\n' && c != EOF) {
        // 读取并丢弃剩余字符
    }
}

/**
 * @brief 角度转弧度
 * @param deg 角度值
 * @return 对应的弧度值
 */
float to_rad(float deg) {
    return deg * 3.1415926535f / 180.0f;
}

/**
 * @brief 弧度转角度
 * @param rad 弧度值
 * @return 对应的角度值
 */
float to_deg(float rad) {
    return rad * 180.0f / 3.1415926535f;
}

/**
 * @brief 打印交互式菜单
 */
void print_menu() {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║           Air8000 MCU 控制中心                           ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [系统命令]                                              ║\n");
    printf("║    1. PING 测试           2. 获取版本                    ║\n");
    printf("║    3. 网络状态            4. 电源查询                    ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [看门狗]                                                ║\n");
    printf("║    10. 查询状态           11. 启用看门狗                 ║\n");
    printf("║    12. 禁用看门狗         13. 发送心跳                   ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [电机控制]                                              ║\n");
    printf("║    20. 开启电机供电       21. 关闭电机供电               ║\n");
    printf("║    22. 电机使能           23. 电机禁用                   ║\n");
    printf("║    24. 电机旋转           25. 电机急停                   ║\n");
    printf("║    26. 获取位置           27. 获取所有状态               ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [设备控制]                                              ║\n");
    printf("║    30. LED 控制           31. 风扇控制                   ║\n");
    printf("║    32. 加热器控制         33. 激光控制                   ║\n");
    printf("║    34. PWM 补光灯         35. 设备状态                   ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [传感器]                                                ║\n");
    printf("║    40. 读取温度           41. 读取所有传感器             ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [文件传输]                                              ║\n");
    printf("║    50. 请求传输文件       51. 取消文件传输               ║\n");
    printf("║    53. 获取传输状态       54. 传输文件                   ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  [FOTA升级]                                              ║\n");
    printf("║    60. 开始FOTA升级       61. 取消FOTA升级               ║\n");
    printf("║    62. 获取FOTA状态                                      ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║    0. 退出                                               ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("请输入选项: ");
}

/**
 * @brief 发送电机控制命令
 */
static void send_motor_command(uint8_t motor_id, float angle, float speed) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    
    msg.type = MSG_TYPE_MOTOR_CMD;
    msg.seq_num = g_seq_num++;
    msg.timestamp = (uint32_t)time(NULL);
    
    // 构建电机控制数据：motor_id(1字节) + angle(4字节float) + speed(4字节float)
    if (msg.data_len >= 9) {
        msg.payload.data[0] = motor_id;
        memcpy(&msg.payload.data[1], &angle, sizeof(float));
        memcpy(&msg.payload.data[5], &speed, sizeof(float));
        msg.data_len = 9;
        
        if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
            perror("mq_send motor command");
        } else {
            printf("电机控制命令已发送\n");
        }
    }
}

/**
 * @brief 发送设备控制命令
 */
static void send_device_command(uint8_t cmd, uint8_t device_id, uint8_t state) {
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    
    msg.type = MSG_TYPE_DEVICE_CMD;
    msg.seq_num = g_seq_num++;
    msg.timestamp = (uint32_t)time(NULL);
    
    // 构建设备控制数据：cmd(1字节) + device_id(1字节) + state(1字节)
    msg.payload.data[0] = cmd;
    msg.payload.data[1] = device_id;
    msg.payload.data[2] = state;
    msg.data_len = 3;
    
    if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
        perror("mq_send device command");
        printf("设备控制命令发送失败，请检查连接\n");
    } else {
        printf("设备控制命令已发送，等待响应...\n");
        // 打印详细的控制信息
        switch (cmd) {
            case 0x50:
                printf("控制设备: LED, 状态: ");
                break;
            case 0x51:
                printf("控制设备: 风扇, 状态: ");
                break;
            case 0x52:
                printf("控制设备: 加热器, 状态: ");
                break;
            case 0x53:
                printf("控制设备: 激光, 状态: ");
                break;
            case 0x54:
                printf("控制设备: PWM补光灯, 亮度: ");
                break;
            default:
                printf("控制设备: 未知, 状态: ");
                break;
        }
        if (cmd == 0x54) {
            // 补光灯只支持亮度调节
            printf("%d\n", state);
        } else if (cmd == 0x50) {
            // LED支持开关和闪烁状态
            switch (state) {
                case 0:
                    printf("关闭\n");
                    break;
                case 1:
                    printf("开启\n");
                    break;
                case 2:
                    printf("闪烁\n");
                    break;
                default:
                    printf("未知\n");
                    break;
            }
        } else {
            // 其他设备（风扇、加热器、激光）只支持开关状态
            switch (state) {
                case 0:
                    printf("关闭\n");
                    break;
                case 1:
                    printf("开启\n");
                    break;
                default:
                    printf("未知\n");
                    break;
            }
        }
        printf("请等待命令执行结果...\n");
    }
}



/**
 * @brief 处理交互式菜单选择
 */
static void handle_menu_selection(int choice) {
    char buffer[64];
    message_t msg;
    
    switch (choice) {
        case 0: {
            // 退出程序
            running = false;
            break;
        }
        case 1: {
            // PING 测试
            printf("发送PING测试命令...\n");
            // 构建并发送PING消息
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x01; // PING命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send ping command");
            } else {
                printf("PING命令已发送，等待响应...\n");
            }
            break;
        }
        case 2: {
            // 获取版本
            printf("发送获取版本命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x02; // 获取版本命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send version command");
            } else {
                printf("获取版本命令已发送，等待响应...\n");
            }
            break;
        }
        case 3: {
            // 网络状态
            printf("发送查询网络状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x03; // 查询网络状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send network status command");
            } else {
                printf("查询网络状态命令已发送，等待响应...\n");
            }
            break;
        }
        case 4: {
            // 电源查询
            printf("发送查询电源状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x04; // 查询电源状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send power query command");
            } else {
                printf("查询电源状态命令已发送，等待响应...\n");
            }
            break;
        }
        case 10: {
            // 查询看门狗状态
            printf("发送查询看门狗状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x10; // 查询看门狗状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send watchdog status command");
            } else {
                printf("查询看门狗状态命令已发送，等待响应...\n");
            }
            break;
        }
        case 11: {
            // 启用看门狗
            printf("发送启用看门狗命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x11; // 启用看门狗命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send enable watchdog command");
            } else {
                printf("启用看门狗命令已发送，等待响应...\n");
            }
            break;
        }
        case 12: {
            // 禁用看门狗
            printf("发送禁用看门狗命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x12; // 禁用看门狗命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send disable watchdog command");
            } else {
                printf("禁用看门狗命令已发送，等待响应...\n");
            }
            break;
        }
        case 13: {
            // 发送心跳
            printf("发送心跳命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_HEARTBEAT;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.data_len = 0;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send heartbeat command");
            } else {
                printf("心跳命令已发送\n");
            }
            break;
        }
        case 20: {
            // 开启电机供电
            printf("发送开启电机供电命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x20; // 开启电机供电命令
            msg.payload.data[1] = 1;     // 开启状态
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send motor power on command");
            } else {
                printf("开启电机供电命令已发送，等待响应...\n");
            }
            break;
        }
        case 21: {
            // 关闭电机供电
            printf("发送关闭电机供电命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x21; // 关闭电机供电命令
            msg.payload.data[1] = 0;     // 关闭状态
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send motor power off command");
            } else {
                printf("关闭电机供电命令已发送，等待响应...\n");
            }
            break;
        }
        case 22: {
            // 电机使能
            printf("请输入电机 ID (1=Y, 2=X, 3=Z): ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送电机使能命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_MOTOR_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x22; // 电机使能命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send motor enable command");
            } else {
                printf("电机使能命令已发送，等待响应...\n");
            }
            break;
        }
        case 23: {
            // 电机禁用
            printf("请输入电机 ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送电机禁用命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_MOTOR_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x23; // 电机禁用命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send motor disable command");
            } else {
                printf("电机禁用命令已发送，等待响应...\n");
            }
            break;
        }
        case 24: {
            // 电机旋转
            printf("请输入电机 ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("请输入角度 (度): ");
            read_input(buffer, sizeof(buffer));
            float ang = atof(buffer);
            printf("请输入速度 (度/秒): ");
            read_input(buffer, sizeof(buffer));
            float spd = atof(buffer);
            
            // 发送电机旋转命令
            send_motor_command(id, to_rad(ang), to_rad(spd));
            break;
        }
        case 25: {
            // 电机急停
            printf("请输入电机 ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送电机急停命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_MOTOR_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x25; // 电机急停命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send motor stop command");
            } else {
                printf("电机急停命令已发送，等待响应...\n");
            }
            break;
        }
        case 26: {
            // 获取电机位置
            printf("请输入电机 ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送获取电机位置命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_MOTOR_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x26; // 获取电机位置命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send get motor position command");
            } else {
                printf("获取电机位置命令已发送，等待响应...\n");
            }
            break;
        }
        case 27: {
            // 获取所有电机状态
            printf("发送获取所有电机状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_MOTOR_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x27; // 获取所有电机状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send get all motor status command");
            } else {
                printf("获取所有电机状态命令已发送，等待响应...\n");
            }
            break;
        }
        case 30: {
            // LED 控制
            printf("LED 状态 (0=关, 1=开, 2=闪烁): ");
            read_input(buffer, sizeof(buffer));
            int s = atoi(buffer);
            printf("发送LED控制命令...\n");
            send_device_command(0x50, 0, s); // CMD_DEV_LED = 0x50, DEVICE_ID_LED = 0
            break;
        }
        case 31: {
            // 风扇控制
            printf("风扇状态 (0=关, 1=开): ");
            read_input(buffer, sizeof(buffer));
            int s = atoi(buffer);
            printf("发送风扇控制命令...\n");
            send_device_command(0x51, 1, s); // CMD_DEV_FAN = 0x51, DEVICE_ID_FAN1 = 1
            break;
        }
        case 32: {
            // 加热器控制
            printf("加热器状态 (0=关, 1=开): ");
            read_input(buffer, sizeof(buffer));
            int s = atoi(buffer);
            printf("发送加热器控制命令...\n");
            send_device_command(0x52, 2, s); // CMD_DEV_HEATER = 0x52, DEVICE_ID_HEATER1 = 2
            break;
        }
        case 33: {
            // 激光控制
            printf("激光状态 (0=关, 1=开): ");
            read_input(buffer, sizeof(buffer));
            int s = atoi(buffer);
            printf("发送激光控制命令...\n");
            send_device_command(0x53, 3, s); // CMD_DEV_LASER = 0x53, DEVICE_ID_LASER = 3
            break;
        }
        case 34: {
            // PWM 补光灯
            printf("补光灯亮度 (0-255): ");
            read_input(buffer, sizeof(buffer));
            int s = atoi(buffer);
            printf("发送PWM补光灯控制命令...\n");
            send_device_command(0x54, 4, s); // CMD_DEV_PWM_LIGHT = 0x54, DEVICE_ID_PWM_LIGHT = 4
            break;
        }
        case 35: {
            // 设备状态查询
            printf("设备ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送设备状态查询命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x35; // 设备状态查询命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send device status query command");
            } else {
                printf("设备状态查询命令已发送，等待响应...\n");
            }
            break;
        }
        case 40: {
            // 读取温度
            printf("传感器 ID: ");
            read_input(buffer, sizeof(buffer));
            int id = atoi(buffer);
            printf("发送读取温度命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x40; // 读取温度命令
            msg.payload.data[1] = (uint8_t)id;
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send read temperature command");
            } else {
                printf("读取温度命令已发送，等待响应...\n");
            }
            break;
        }
        case 41: {
            // 读取所有传感器
            printf("发送读取所有传感器命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_DEVICE_CMD;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x41; // 读取所有传感器命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send read all sensors command");
            } else {
                printf("读取所有传感器命令已发送，等待响应...\n");
            }
            break;
        }
        case 50: {
            // 请求传输文件
            printf("发送请求传输文件命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FILE_INFO;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x50; // 请求传输文件命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send file transfer request command");
            } else {
                printf("请求传输文件命令已发送，等待响应...\n");
            }
            break;
        }
        case 51: {
            // 取消文件传输
            printf("发送取消文件传输命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FILE_COMPLETE;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x51; // 取消文件传输命令
            msg.payload.data[1] = 1;     // 取消标志
            msg.data_len = 2;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send cancel file transfer command");
            } else {
                printf("取消文件传输命令已发送，等待响应...\n");
            }
            break;
        }
        case 53: {
            // 获取传输状态
            printf("发送获取传输状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FILE_INFO;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x53; // 获取传输状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send get transfer status command");
            } else {
                printf("获取传输状态命令已发送，等待响应...\n");
            }
            break;
        }
        case 54: {
            // 54. 传输文件
            printf("发送传输文件命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FILE_START;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.data_len = 0;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send transfer file command");
            } else {
                printf("传输文件命令已发送，等待响应...\n");
            }
            break;
        }
        case 60: {
            // 开始FOTA升级
            printf("发送开始FOTA升级命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FOTA_START;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.data_len = 0;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send fota start");
            } else {
                printf("开始FOTA升级命令已发送，等待响应...\n");
            }
            break;
        }
        case 61: {
            // 取消FOTA升级
            printf("发送取消FOTA升级命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FOTA_START;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x61; // 取消FOTA升级命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send cancel fota command");
            } else {
                printf("取消FOTA升级命令已发送，等待响应...\n");
            }
            break;
        }
        case 62: {
            // 获取FOTA状态
            printf("发送获取FOTA状态命令...\n");
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_FOTA_START;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            msg.payload.data[0] = 0x62; // 获取FOTA状态命令
            msg.data_len = 1;
            
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send get fota status command");
            } else {
                printf("获取FOTA状态命令已发送，等待响应...\n");
            }
            break;
        }
        default:
            printf("未知命令\n");
            break;
    }
}

/**
 * @brief 信号处理函数
 */
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        running = false;
        printf("\nReceived signal %d, exiting...\n", sig);
    }
}

/**
 * @brief 初始化信号处理
 */
static void init_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    
    // 忽略SIGPIPE信号
    signal(SIGPIPE, SIG_IGN);
    
    // 处理SIGINT和SIGTERM信号
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/**
 * @brief 初始化共享内存
 * @return 成功返回0，失败返回-1
 */
static int init_shared_memory() {
    printf("Initializing shared memory...\n");
    
    // 创建共享内存
    if (shm_create(&g_shm_handle) == -1) {
        fprintf(stderr, "Failed to create shared memory\n");
        return -1;
    }
    
    printf("Shared memory initialized successfully\n");
    return 0;
}

/**
 * @brief 初始化消息队列
 * @return 成功返回0，失败返回-1
 */
static int init_message_queues() {
    printf("Initializing message queues...\n");
    
    // 创建UART到MQTT的消息队列
    g_mq_uart_to_mqtt = mq_create(MSG_QUEUE_UART_TO_MQTT, NULL);
    if (g_mq_uart_to_mqtt == -1) {
        fprintf(stderr, "Failed to create uart_to_mqtt message queue\n");
        return -1;
    }
    
    // 创建MQTT到UART的消息队列
    g_mq_mqtt_to_uart = mq_create(MSG_QUEUE_MQTT_TO_UART, NULL);
    if (g_mq_mqtt_to_uart == -1) {
        fprintf(stderr, "Failed to create mqtt_to_uart message queue\n");
        mq_close_queue(g_mq_uart_to_mqtt);
        mq_delete_queue(MSG_QUEUE_UART_TO_MQTT);
        return -1;
    }
    
    printf("Message queues initialized successfully\n");
    return 0;
}

/**
 * @brief 创建并启动UART进程
 * @return 成功返回0，失败返回-1
 */
static int create_uart_process() {
    printf("Creating UART process...\n");
    
    // 设置UART进程配置
    process_config_t uart_config = {
        .name = "air8000_uart",
        .type = PROC_TYPE_UART,
        .cmd = "./air8000_test",
        .args = NULL,
        .auto_restart = true,
        .restart_delay = 1000,
        .private_data = NULL
    };
    
    // 创建UART进程
    g_uart_process = process_create(&uart_config);
    if (g_uart_process == NULL) {
        fprintf(stderr, "Failed to create UART process\n");
        return -1;
    }
    
    // 启动UART进程
    if (process_start(g_uart_process) == -1) {
        fprintf(stderr, "Failed to start UART process\n");
        process_destroy(g_uart_process);
        g_uart_process = NULL;
        return -1;
    }
    
    printf("UART process started successfully, PID: %d\n", process_get_pid(g_uart_process));
    return 0;
}

/**
 * @brief 创建并启动MQTT进程
 * @return 成功返回0，失败返回-1
 */
static int create_mqtt_process() {
    printf("Creating MQTT process...\n");
    
    // 设置MQTT进程配置
    process_config_t mqtt_config = {
        .name = "air8000_mqtt",
        .type = PROC_TYPE_MQTT,
        .cmd = "./mqtt_client_test",
        .args = NULL,
        .auto_restart = true,
        .restart_delay = 1000,
        .private_data = NULL
    };
    
    // 创建MQTT进程
    g_mqtt_process = process_create(&mqtt_config);
    if (g_mqtt_process == NULL) {
        fprintf(stderr, "Failed to create MQTT process\n");
        return -1;
    }
    
    // 启动MQTT进程
    if (process_start(g_mqtt_process) == -1) {
        fprintf(stderr, "Failed to start MQTT process\n");
        process_destroy(g_mqtt_process);
        g_mqtt_process = NULL;
        return -1;
    }
    
    printf("MQTT process started successfully, PID: %d\n", process_get_pid(g_mqtt_process));
    return 0;
}

/**
 * @brief 监控进程状态
 */
static void monitor_processes() {
    if (g_uart_process != NULL) {
        process_state_t prev_state = process_get_state(g_uart_process);
        process_update_state(g_uart_process);
        process_state_t curr_state = process_get_state(g_uart_process);
        
        // 如果状态发生变化，打印日志
        if (prev_state != curr_state) {
            printf("UART process state changed: %d -> %d\n", prev_state, curr_state);
        }
        
        if (!process_is_running(g_uart_process)) {
            printf("UART process is not running, PID: %d, exit code: %d, restarting...\n", 
                   process_get_pid(g_uart_process), g_uart_process->exit_code);
            process_restart(g_uart_process, 1000);
        }
    }
    
    if (g_mqtt_process != NULL) {
        process_state_t prev_state = process_get_state(g_mqtt_process);
        process_update_state(g_mqtt_process);
        process_state_t curr_state = process_get_state(g_mqtt_process);
        
        // 如果状态发生变化，打印日志
        if (prev_state != curr_state) {
            printf("MQTT process state changed: %d -> %d\n", prev_state, curr_state);
        }
        
        if (!process_is_running(g_mqtt_process)) {
            printf("MQTT process is not running, PID: %d, exit code: %d, restarting...\n", 
                   process_get_pid(g_mqtt_process), g_mqtt_process->exit_code);
            process_restart(g_mqtt_process, 1000);
        }
    }
}

/**
 * @brief 清理资源
 */
static void cleanup() {
    printf("Cleaning up resources...\n");
    
    // 停止UART进程
    if (g_uart_process != NULL) {
        printf("Stopping UART process...\n");
        process_stop(g_uart_process, 5000);
        process_destroy(g_uart_process);
        g_uart_process = NULL;
    }
    
    // 停止MQTT进程
    if (g_mqtt_process != NULL) {
        printf("Stopping MQTT process...\n");
        process_stop(g_mqtt_process, 5000);
        process_destroy(g_mqtt_process);
        g_mqtt_process = NULL;
    }
    
    // 关闭消息队列
    if (g_mq_uart_to_mqtt != -1) {
        mq_close_queue(g_mq_uart_to_mqtt);
        mq_delete_queue(MSG_QUEUE_UART_TO_MQTT);
        g_mq_uart_to_mqtt = -1;
    }
    
    if (g_mq_mqtt_to_uart != -1) {
        mq_close_queue(g_mq_mqtt_to_uart);
        mq_delete_queue(MSG_QUEUE_MQTT_TO_UART);
        g_mq_mqtt_to_uart = -1;
    }
    
    // 销毁共享内存
    shm_destroy(&g_shm_handle);
    
    printf("Cleanup completed\n");
}

/**
 * @brief 主函数
 */
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Air8000 Process Manager v1.0\n");
    printf("================================\n");
    
    // 初始化信号处理
    init_signal_handlers();
    
    // 初始化共享内存
    if (init_shared_memory() != 0) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    // 初始化消息队列
    if (init_message_queues() != 0) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    // 创建并启动UART进程
    if (create_uart_process() != 0) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    // 创建并启动MQTT进程
    if (create_mqtt_process() != 0) {
        cleanup();
        return EXIT_FAILURE;
    }
    
    printf("\nAll processes started successfully!\n");
    printf("Press Ctrl+C to exit...\n\n");
    
    char buffer[64];                    /* 用于存储用户输入 */
    int choice;                        /* 用户选择的命令选项 */
    
    // 主循环
    while (running) {
        // 监控进程状态
        monitor_processes();
        
        // 显示菜单并处理用户输入
        print_menu();                  /* 打印命令菜单 */
        read_input(buffer, sizeof(buffer));  /* 读取用户输入 */
        if (sscanf(buffer, "%d", &choice) == 1) {
            handle_menu_selection(choice);  /* 处理用户选择 */
        }
        
        // 短暂休眠
        usleep(1000); // 1ms，避免CPU占用过高
        
        // 接收并处理响应消息
        message_t resp_msg;
        unsigned int priority;
        int ret = mq_receive_msg(g_mq_uart_to_mqtt, &resp_msg, &priority, 0);
        if (ret == 0) {
            // 成功接收到响应消息
            if (resp_msg.type == MSG_TYPE_RESPONSE) {
                // 解析响应数据
                int result = *((int*)resp_msg.payload.data);
                printf("\n命令执行结果: ");
                if (result == 0) {
                    printf("成功\n");
                } else {
                    printf("失败，错误码: %d\n", result);
                }
            } else if (resp_msg.type == MSG_TYPE_SENSOR_DATA) {
                // 传感器数据响应
                printf("\n收到传感器数据\n");
                // 解析传感器数据
                if (resp_msg.data_len > 0 && resp_msg.payload.data) {
                    // 假设传感器数据格式：温度(4字节float) + 湿度(2字节) + 光照(2字节) + 电池(2字节)
                    if (resp_msg.data_len >= 10) {
                        float temperature;
                        uint16_t humidity, light, battery;
                        memcpy(&temperature, &resp_msg.payload.data[0], sizeof(float));
                        memcpy(&humidity, &resp_msg.payload.data[4], sizeof(uint16_t));
                        memcpy(&light, &resp_msg.payload.data[6], sizeof(uint16_t));
                        memcpy(&battery, &resp_msg.payload.data[8], sizeof(uint16_t));
                        printf("传感器数据解析: 温度=%.2f°C, 湿度=%d%%, 光照=%d, 电池=%d%%\n", 
                               temperature, humidity, light, battery);
                    } else {
                        printf("传感器数据长度不足，无法完全解析\n");
                    }
                }
            } else if (resp_msg.type == MSG_TYPE_FILE_INFO) {
                // 文件信息响应
                printf("\n收到文件信息\n");
                // 解析文件信息
                if (resp_msg.data_len > 0 && resp_msg.payload.data) {
                    printf("文件信息数据长度: %d\n", resp_msg.data_len);
                    // 假设文件信息格式：文件名 + 文件大小
                    printf("文件名: %s\n", (char*)resp_msg.payload.data);
                    if (resp_msg.data_len > strlen((char*)resp_msg.payload.data) + 1) {
                        uint32_t file_size;
                        memcpy(&file_size, &resp_msg.payload.data[strlen((char*)resp_msg.payload.data) + 1], sizeof(uint32_t));
                        printf("文件大小: %d bytes\n", file_size);
                    }
                }
            } else if (resp_msg.type == MSG_TYPE_FOTA_COMPLETE) {
                // FOTA升级完成响应
                printf("\nFOTA升级完成\n");
                // 解析FOTA升级结果
                if (resp_msg.data_len > 0 && resp_msg.payload.data) {
                    int fota_result = resp_msg.payload.data[0];
                    printf("FOTA升级结果: %s\n", fota_result == 0 ? "成功" : "失败");
                }
            } else {
                // 其他类型响应
                printf("\n收到响应消息，类型: %d\n", resp_msg.type);
                // 打印响应数据
                if (resp_msg.data_len > 0 && resp_msg.payload.data) {
                    printf("响应数据长度: %d\n", resp_msg.data_len);
                    printf("响应数据: ");
                    for (size_t i = 0; i < resp_msg.data_len && i < 32; i++) {
                        printf("%02X ", resp_msg.payload.data[i]);
                    }
                    printf("\n");
                }
            }
        } else if (ret == 1) {
            // 超时，没有收到消息，继续循环
        } else {
            // 接收消息失败
            printf("\n接收响应消息失败，错误码: %d\n", ret);
        }
    }
    
    // 清理资源
    cleanup();
    
    printf("\nProcess manager exited successfully\n");
    return EXIT_SUCCESS;
}
