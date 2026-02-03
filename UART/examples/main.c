/**
 * @file main.c
 * @brief 与air8000串口通信自动运行示例程序
 */

#include "air8000.h"              /* Air8000 主头文件 */
#include "air8000_file_transfer.h" /* 文件传输功能头文件 */
#include "air8000_fota.h"          /* FOTA升级功能头文件 */
// 暂时注释掉图片处理头文件
// #include "air8000_image_process.h" /* 图片处理头文件 */
#include <stdio.h>           /* 标准输入输出 */
#include <stdlib.h>          /* 标准库函数 */
#include <string.h>          /* 字符串处理函数 */
#include <unistd.h>          /* UNIX 标准函数，如 sleep */
#include <math.h>            /* 数学函数，如弧度角度转换 */
#include <sys/stat.h>        /* 文件状态相关函数 */
#include <dirent.h>          /* 目录处理相关函数 */
#include <fcntl.h>           /* 文件控制函数 */
#include <errno.h>           /* 错误码定义 */
#include <signal.h>          /* 信号处理函数 */
#include <time.h>            /* 时间函数 */
#include "message_queue.h" /* 消息队列头文件 */

#define DEFAULT_DEVICE "/dev/ttyACM2"  /* 默认串口设备路径 */
#define DEFAULT_TIMEOUT 2000           /* 默认超时时间，单位毫秒 */

/**
 * @brief 全局变量
 */
static volatile bool running = true;                /* 运行标志 */
static air8000_t *g_ctx = NULL;                     /* Air8000上下文 */
static int g_mq_uart_to_mqtt = -1;                /* UART到MQTT的消息队列 */
static int g_mq_mqtt_to_uart = -1;                /* MQTT到UART的消息队列 */
static uint32_t g_seq_num = 0;                      /* 序列号计数器 */





/**
 * @brief 文件传输回调函数
 * @param ctx Air8000上下文指针
 * @param event 事件类型
 * @param data 事件数据
 * @param user_data 用户数据
 */
static void file_transfer_callback(air8000_t *ctx, air8000_file_transfer_event_t event, void *data, void *user_data) {
    (void)ctx;
    (void)user_data;
    
    switch (event) {
        case FILE_TRANSFER_EVENT_NOTIFY_ACKED:
            printf("[文件传输] 通知被确认\n");
            break;
        case FILE_TRANSFER_EVENT_STARTED:
            printf("[文件传输] 传输开始\n");
            if (data) {
                air8000_file_info_t *file_info = (air8000_file_info_t *)data;
                printf("[文件传输] 文件名: %s, 文件大小: %llu, 分片大小: %u\n", 
                       file_info->filename, file_info->file_size, file_info->block_size);
            }
            break;
        case FILE_TRANSFER_EVENT_DATA_SENT:
            if (data) {
                uint8_t *progress = (uint8_t *)data;
                printf("[文件传输] 分片发送成功，进度: %d%%\n", *progress);
            } else {
                printf("[文件传输] 分片发送成功\n");
            }
            break;
        case FILE_TRANSFER_EVENT_COMPLETED:
            printf("[文件传输] 传输完成\n");
            break;
        case FILE_TRANSFER_EVENT_ERROR:
            if (data) {
                int *error = (int *)data;
                printf("[文件传输] 传输错误，错误码: %d\n", *error);
            } else {
                printf("[文件传输] 传输错误\n");
            }
            break;
        case FILE_TRANSFER_EVENT_CANCELLED:
            printf("[文件传输] 传输取消\n");
            break;
        case FILE_TRANSFER_EVENT_REQUEST_RECEIVED:
            if (data) {
                printf("[文件传输] 收到传输请求: %s\n", (char *)data);
            } else {
                printf("[文件传输] 收到传输请求\n");
            }
            break;
        default:
            printf("[文件传输] 未知事件: %d\n", event);
            break;
    }
}

/**
 * @brief FOTA升级回调函数
 * @param ctx Air8000上下文指针
 * @param event 事件类型
 * @param data 事件数据
 * @param user_data 用户数据
 */
static void fota_callback(air8000_t *ctx, air8000_fota_event_t event, void *data, void *user_data) {
    (void)ctx;
    (void)user_data;
    
    switch (event) {
        case FOTA_EVENT_STARTED:
            printf("[FOTA] 升级开始\n");
            break;
        case FOTA_EVENT_DATA_SENT:
            {
                uint32_t *sent_size = (uint32_t *)data;
                printf("[FOTA] 数据发送成功，已发送: %u字节\n", *sent_size);
            }
            break;
        case FOTA_EVENT_COMPLETED:
            printf("[FOTA] 升级完成\n");
            break;
        case FOTA_EVENT_ERROR:
            {
                int *error = (int *)data;
                printf("[FOTA] 升级错误，错误码: %d\n", *error);
            }
            break;
        case FOTA_EVENT_ABORTED:
            printf("[FOTA] 升级取消\n");
            break;
        case FOTA_EVENT_STATUS_UPDATED:
            {
                uint8_t *progress = (uint8_t *)data;
                printf("[FOTA] 状态更新，进度: %d%%\n", *progress);
            }
            break;
        default:
            printf("[FOTA] 未知事件\n");
            break;
    }
}

/**
 * @brief 检查串口设备是否存在
 * @param path 串口设备路径
 * @return 成功返回0，失败返回-1
 */
int check_port(const char *path) {
    struct stat st;                    /* 用于存储文件状态 */
    return stat(path, &st);            /* 调用stat函数检查文件是否存在 */
}

/**
 * @brief 自动探测可用的串口设备
 * @param buffer 输出缓冲区，用于存储探测到的串口路径
 * @param len 缓冲区最大长度
 * @details 依次检查常用的串口设备路径，返回第一个存在的设备
 */
void auto_detect_port(char *buffer, size_t len) {
    /* 常用的串口设备路径候选列表 */
    const char *candidates[] = {
        "/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyACM2", "/dev/ttyACM3",  /* USB CDC ACM 设备 */
        "/dev/ttyUSB0", "/dev/ttyUSB1", "/dev/ttyUSB2", "/dev/ttyUSB3",  /* USB 转串口设备 */
        NULL                                                              /* 结束标志 */
    };

    buffer[0] = '\0';                 /* 初始化缓冲区为空字符串 */

    /* 遍历候选列表，检查每个设备是否存在 */
    for (int i = 0; candidates[i] != NULL; i++) {
        if (check_port(candidates[i]) == 0) {
            /* 找到可用设备，保存路径并返回 */
            snprintf(buffer, len, "%s", candidates[i]);
            return;
        }
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
 * @brief 信号处理函数
 * @param sig 收到的信号值
 * @details 处理SIGINT和SIGTERM信号，设置running标志为false，退出程序
 */
static void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        running = false;
        printf("\nReceived signal %d, exiting...\n", sig);
    }
}

/**
 * @brief 初始化信号处理
 * @details 配置SIGINT和SIGTERM信号的处理函数，忽略SIGPIPE信号
 */
static void init_signal_handlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sa.sa_flags = 0;
    
    /* 忽略SIGPIPE信号 */
    signal(SIGPIPE, SIG_IGN);
    
    /* 处理SIGINT和SIGTERM信号 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

/**
 * @brief 初始化消息队列
 * @return 成功返回true，失败返回false
 */
static bool init_message_queues() {
    /* 打开UART到MQTT的消息队列 */
    g_mq_uart_to_mqtt = mq_open_existing(MSG_QUEUE_UART_TO_MQTT, O_WRONLY);
    if (g_mq_uart_to_mqtt == -1) {
        // 独立运行模式，继续执行
        return true;
    }
    
    /* 打开MQTT到UART的消息队列 */
    g_mq_mqtt_to_uart = mq_open_existing(MSG_QUEUE_MQTT_TO_UART, O_RDONLY);
    if (g_mq_mqtt_to_uart == -1) {
        mq_close_queue(g_mq_uart_to_mqtt);
        g_mq_uart_to_mqtt = -1;
        // 独立运行模式，继续执行
        return true;
    }
    
    printf("Message queues initialized successfully\n");
    return true;
}

/**
 * @brief 检查FOTA文件是否存在
 */
static bool check_fota_file_exists() {
    char firmware_path[1024] = {0};
    snprintf(firmware_path, sizeof(firmware_path), "/appfs/nfs/AIR8000.bin");
    
    struct stat file_stat;
    return stat(firmware_path, &file_stat) == 0 && S_ISREG(file_stat.st_mode);
}

/**
 * @brief 获取FOTA文件路径
 */
static void get_fota_file_path(char *buffer, size_t buffer_size) {
    snprintf(buffer, buffer_size, "/appfs/nfs/AIR8000.bin");
}

/**
 * @brief 执行FOTA升级
 */
static void execute_fota_upgrade() {
    printf("Starting FOTA upgrade to Air8000\n");
    
    // 获取FOTA文件路径
    char firmware_path[1024] = {0};
    get_fota_file_path(firmware_path, sizeof(firmware_path));
    printf("Firmware file path: %s\n", firmware_path);
    
    // 检查升级文件是否存在
    struct stat file_stat;
    if (stat(firmware_path, &file_stat) != 0) {
        printf("Firmware file not found: %s\n", firmware_path);
        return;
    }
    
    printf("Firmware file found, size: %llu bytes\n", (unsigned long long)file_stat.st_size);
    
    // 创建FOTA上下文
    air8000_fota_ctx_t *fota_ctx = air8000_fota_create(g_ctx, firmware_path, fota_callback, NULL);
    if (!fota_ctx) {
        printf("Failed to create FOTA context\n");
        return;
    }
    
    // 开始FOTA升级
    int ret = air8000_fota_start(fota_ctx);
    if (ret != AIR8000_OK) {
        printf("Failed to start FOTA upgrade: %d\n", ret);
        air8000_fota_destroy(fota_ctx);
        return;
    }
    
    printf("FOTA upgrade started successfully\n");
    
    // 升级完成，发送通知
    message_t complete_msg;
    memset(&complete_msg, 0, sizeof(complete_msg));
    complete_msg.type = MSG_TYPE_FOTA_COMPLETE;
    complete_msg.seq_num = g_seq_num++;
    complete_msg.timestamp = (uint32_t)time(NULL);
    complete_msg.data_len = 4; // 简单的结果代码
    uint32_t result_code = ret; // 使用升级结果作为代码
    memcpy(complete_msg.payload.data, &result_code, complete_msg.data_len);
    
    if (g_mq_uart_to_mqtt != -1) {
        if (mq_send_msg(g_mq_uart_to_mqtt, &complete_msg, 0) != 0) {
            perror("mq_send fota complete");
        } else {
            printf("Sent FOTA complete notification\n");
        }
    } else {
        printf("Running in standalone mode, skipping FOTA complete notification\n");
    }
    
    // 销毁FOTA上下文
    air8000_fota_destroy(fota_ctx);
    printf("FOTA context destroyed\n");
}

/**
 * @brief 发送命令执行结果反馈
 */
static void send_command_response(uint32_t seq_num, int result, const uint8_t *data, size_t len) {
    // 检查消息队列是否已初始化
    if (g_mq_uart_to_mqtt == -1) {
        // 独立运行模式，只打印结果不发送消息
        return;
    }
    
    message_t resp_msg;
    memset(&resp_msg, 0, sizeof(resp_msg));
    
    resp_msg.type = MSG_TYPE_RESPONSE;
    resp_msg.seq_num = seq_num;
    resp_msg.timestamp = (uint32_t)time(NULL);
    
    // 结果数据格式：result(4字节) + 数据(len字节)
    if (len > 0 && data != NULL) {
        if (4 + len <= sizeof(resp_msg.payload.data)) {
            memcpy(resp_msg.payload.data, &result, 4);
            memcpy(resp_msg.payload.data + 4, data, len);
            resp_msg.data_len = 4 + len;
        } else {
            return;
        }
    } else {
        memcpy(resp_msg.payload.data, &result, 4);
        resp_msg.data_len = 4;
    }
    
    if (mq_send_msg(g_mq_uart_to_mqtt, &resp_msg, 0) != 0) {
        perror("mq_send response");
    }
}

/**
 * @brief 处理从MQTT接收到的控制指令
 */
static void handle_mqtt_commands() {
    // 检查消息队列是否已初始化
    if (g_mq_mqtt_to_uart == -1) {
        // 独立运行模式，不处理MQTT命令
        return;
    }
    
    message_t msg;
    unsigned int priority;
    
    // 从消息队列接收数据，超时10ms，确保设备控制命令及时处理
    int ret = mq_receive_msg(g_mq_mqtt_to_uart, &msg, &priority, 10);
    if (ret == 0) {
        // 成功接收到消息
        printf("Received command from MQTT, type: %d, seq: %u\n", msg.type, msg.seq_num);
        
        int result = 0; // 默认成功
        
        // 优先处理设备控制命令
        if (msg.type == MSG_TYPE_DEVICE_CMD) {
            if (msg.data_len >= 1) {
                uint8_t cmd_code = msg.payload.data[0];
                
                // 响应数据缓冲区
                uint8_t resp_data[64];
                size_t resp_len = 0;
                
                switch (cmd_code) {
                    case 0x01: // PING命令
                        result = air8000_ping(g_ctx, DEFAULT_TIMEOUT);
                        break;
                    case 0x02: // 获取版本命令
                        {
                            air8000_version_t version;
                            result = air8000_get_version(g_ctx, &version, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存版本数据到响应
                                memcpy(resp_data, &version, sizeof(air8000_version_t));
                                resp_len = sizeof(air8000_version_t);
                            }
                        }
                        break;
                    case 0x03: // 查询网络状态命令
                        {
                            air8000_network_status_t net;
                            result = air8000_query_network(g_ctx, &net, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存网络状态到响应
                                memcpy(resp_data, &net, sizeof(air8000_network_status_t));
                                resp_len = sizeof(air8000_network_status_t);
                            }
                        }
                        break;
                    case 0x04: // 查询电源状态命令
                        {
                            air8000_power_adc_t pwr;
                            result = air8000_query_power(g_ctx, &pwr, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存电源状态到响应
                                memcpy(resp_data, &pwr, sizeof(air8000_power_adc_t));
                                resp_len = sizeof(air8000_power_adc_t);
                            }
                        }
                        break;
                    case 0x10: // 查询看门狗状态命令
                        {
                            air8000_wdt_status_t wdt_status;
                            result = air8000_wdt_status(g_ctx, &wdt_status, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存看门狗状态到响应
                                memcpy(resp_data, &wdt_status, sizeof(air8000_wdt_status_t));
                                resp_len = sizeof(air8000_wdt_status_t);
                            }
                        }
                        break;
                    case 0x11: // 启用看门狗命令
                        {
                            air8000_wdt_config_t cfg = {true, 480, 2};
                            result = air8000_wdt_config(g_ctx, &cfg, DEFAULT_TIMEOUT);
                        }
                        break;
                    case 0x12: // 禁用看门狗命令
                        {
                            air8000_wdt_config_t cfg = {false, 480, 2};
                            result = air8000_wdt_config(g_ctx, &cfg, DEFAULT_TIMEOUT);
                        }
                        break;
                    case 0x20: // 开启电机供电命令
                        result = air8000_device_control(g_ctx, CMD_DEV_MOTOR_POWER, 0, 1, DEFAULT_TIMEOUT);
                        break;
                    case 0x21: // 关闭电机供电命令
                        result = air8000_device_control(g_ctx, CMD_DEV_MOTOR_POWER, 0, 0, DEFAULT_TIMEOUT);
                        break;
                    case 0x35: // 设备状态查询命令
                        {
                            // 设备状态查询，参考早期工程 main.c:495-515
                            if (msg.data_len >= 2) {
                                uint8_t device_id = msg.payload.data[1];
                                air8000_frame_t req, resp;
                                air8000_frame_init(&req);
                                air8000_frame_init(&resp);
                                req.type = FRAME_TYPE_REQUEST;
                                req.seq = air8000_next_seq();
                                req.cmd = CMD_DEV_GET_STATE;
                                uint8_t data[1] = {device_id};
                                req.data = data;
                                req.data_len = 1;
                                result = air8000_send_and_wait(g_ctx, &req, &resp, DEFAULT_TIMEOUT);
                                if (result == 0) {
                                    // 保存设备状态到响应
                                    if (resp.data_len > 0 && resp.data) {
                                        memcpy(resp_data, resp.data, resp.data_len);
                                        resp_len = resp.data_len;
                                    }
                                }
                                air8000_frame_cleanup(&resp);
                            } else {
                                result = -1;
                            }
                        }
                        break;
                    case 0x40: // 读取温度命令
                        if (msg.data_len >= 2) {
                            uint8_t sensor_id = msg.payload.data[1];
                            float temp;
                            result = air8000_sensor_read_temp(g_ctx, sensor_id, &temp, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存温度数据到响应
                                memcpy(resp_data, &temp, sizeof(float));
                                resp_len = sizeof(float);
                            }
                        } else {
                            result = -1;
                        }
                        break;
                    case 0x41: // 读取所有传感器命令
                        {
                            air8000_sensor_data_t sensor_data;
                            result = air8000_sensor_read_all(g_ctx, &sensor_data, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存传感器数据到响应
                                memcpy(resp_data, &sensor_data, sizeof(air8000_sensor_data_t));
                                resp_len = sizeof(air8000_sensor_data_t);
                            }
                        }
                        break;
                    case 0x50: // LED控制命令 (from process_manager)
                    case 0x51: // FAN控制命令 (from process_manager)
                    case 0x52: // HEATER控制命令 (from process_manager)
                    case 0x53: // LASER控制命令 (from process_manager)
                    case 0x54: // PWM LIGHT控制命令 (from process_manager)
                        // 设备控制命令，包含设备ID和状态
                        if (msg.data_len >= 3) {
                            uint8_t device_type = msg.payload.data[0];
                            uint8_t device_id = msg.payload.data[1];
                            uint8_t state = msg.payload.data[2];
                            
                            air8000_command_t cmd;
                            switch (device_type) {
                                case 0: // LED
                                    cmd = CMD_DEV_LED;
                                    break;
                                case 1: // FAN
                                    cmd = CMD_DEV_FAN;
                                    break;
                                case 2: // HEATER
                                    cmd = CMD_DEV_HEATER;
                                    break;
                                case 3: // LASER
                                    cmd = CMD_DEV_LASER;
                                    break;
                                case 4: // PWM LIGHT
                                    cmd = CMD_DEV_PWM_LIGHT;
                                    break;
                                default:
                                    cmd = CMD_DEV_LED;
                                    break;
                            }
                            
                            // 优先处理设备控制命令
                            result = air8000_device_control(g_ctx, cmd, device_id, state, DEFAULT_TIMEOUT);
                            printf("Device control command executed, result: %d\n", result);
                        } else {
                            result = -1;
                        }
                        break;
                    default:
                        // 其他命令
                        result = -1;
                        printf("Unknown command code: 0x%02X\n", cmd_code);
                        break;
                }
                
                if (result != 0) {
                    printf("Failed to execute device command, result: %d\n", result);
                } else {
                    printf("Device command executed successfully\n");
                }
                
                // 发送带有数据的响应
                send_command_response(msg.seq_num, result, resp_data, resp_len);
            } else {
                result = -1;
                printf("Invalid device command data length\n");
                send_command_response(msg.seq_num, result, NULL, 0);
            }
        } else if (msg.type == MSG_TYPE_MOTOR_CMD) {
            // 电机控制指令
            if (msg.data_len >= 2) {
                uint8_t cmd_code = msg.payload.data[0];
                uint8_t motor_id = msg.payload.data[1];
                
                // 响应数据缓冲区
                uint8_t resp_data[64];
                size_t resp_len = 0;
                
                switch (cmd_code) {
                    case 0x22: // 电机使能
                        result = air8000_motor_enable(g_ctx, motor_id, 2, DEFAULT_TIMEOUT);
                        break;
                    case 0x23: // 电机禁用
                        result = air8000_motor_disable(g_ctx, motor_id, DEFAULT_TIMEOUT);
                        break;
                    case 0x25: // 电机急停
                        result = air8000_motor_stop(g_ctx, motor_id, DEFAULT_TIMEOUT);
                        break;
                    case 0x26: // 获取电机位置
                        {
                            float pos;
                            result = air8000_motor_get_pos(g_ctx, motor_id, &pos, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存位置数据到响应
                                memcpy(resp_data, &pos, sizeof(float));
                                resp_len = sizeof(float);
                            }
                        }
                        break;
                    case 0x27: // 获取所有电机状态
                        {
                            air8000_all_motor_status_t status;
                            result = air8000_motor_get_all(g_ctx, &status, DEFAULT_TIMEOUT);
                            if (result == 0) {
                                // 保存电机数量和状态到响应
                                memcpy(resp_data, &status.count, sizeof(size_t));
                                if (status.count > 0 && status.motors != NULL) {
                                    memcpy(resp_data + sizeof(size_t), status.motors, status.count * sizeof(air8000_motor_state_item_t));
                                    resp_len = sizeof(size_t) + status.count * sizeof(air8000_motor_state_item_t);
                                } else {
                                    resp_len = sizeof(size_t);
                                }
                                // 释放内存
                                air8000_free_motor_status(&status);
                            }
                        }
                        break;
                    default:
                        // 电机旋转命令，包含角度和速度
                        if (msg.data_len >= 9) {
                            float angle = *((float*)&msg.payload.data[1]);
                            float speed = *((float*)&msg.payload.data[5]);
                            result = air8000_motor_rotate(g_ctx, motor_id, angle, speed, DEFAULT_TIMEOUT);
                        } else {
                            result = -1;
                        }
                        break;
                }
                
                if (result != 0) {
                    printf("Failed to execute motor command, result: %d\n", result);
                } else {
                    printf("Motor command executed successfully\n");
                }
                
                // 发送带有数据的响应
                send_command_response(msg.seq_num, result, resp_data, resp_len);
            } else {
                result = -1;
                printf("Invalid motor command data length\n");
                send_command_response(msg.seq_num, result, NULL, 0);
            }
        } else {
            // 其他类型命令
            switch (msg.type) {
                case MSG_TYPE_HEARTBEAT: {
                    // 心跳命令
                    // 心跳命令只需要回复成功
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                }
                case MSG_TYPE_FOTA_DATA: {
                    // FOTA数据 - 现在在MQTT Client中处理
                    printf("FOTA data received, handled by MQTT Client\n");
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                }
                case MSG_TYPE_FOTA_START: {
                    // 开始FOTA升级 - 现在在MQTT Client中处理
                    printf("FOTA start command received, handled by MQTT Client\n");
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                }
                case MSG_TYPE_FOTA_END: {
                    // FOTA数据传输结束 - 现在在MQTT Client中处理
                    printf("FOTA end command received, handled by MQTT Client\n");
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                }
                case MSG_TYPE_FOTA_COMPLETE: {
                    // FOTA完成，执行升级
                    printf("FOTA complete command received, starting upgrade\n");
                    if (check_fota_file_exists()) {
                        execute_fota_upgrade();
                        result = 0;
                    } else {
                        result = -1;
                        printf("No FOTA file found\n");
                    }
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                }
                case MSG_TYPE_FILE_INFO:
                    // 文件传输相关命令
                    if (msg.data_len >= 1) {
                        uint8_t cmd_code = msg.payload.data[0];
                        switch (cmd_code) {
                            case 0x50: // 50. 请求传输文件
                                printf("[文件传输] 请求从 Air8000 传输文件 AIR8000.jpg\n");
                                // 请求 Air8000 传输文件
                                result = air8000_file_transfer_request(g_ctx, "AIR8000.jpg", "/tmp/AIR8000.jpg");
                                break;
                            case 0x53: // 53. 获取传输状态
                                printf("[文件传输] 获取传输状态\n");
                                // 获取传输状态
                                air8000_file_transfer_state_t state = air8000_file_transfer_get_state();
                                printf("[文件传输] 当前状态: %d\n", state);
                                // 发送状态信息
                                send_command_response(msg.seq_num, result, (uint8_t*)&state, sizeof(state));
                                break;
                            default:
                                result = -1;
                                break;
                        }
                    } else {
                        result = -1;
                    }
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                case MSG_TYPE_FILE_START:
                    // 54. 传输文件
                    printf("[文件传输] 开始传输文件到 Air8000\n");
                    // 传输文件到 Air8000
                    result = air8000_file_transfer_start(g_ctx, "cv610.jpg", "/appfs/nfs/cv610.jpg", 0);
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                case MSG_TYPE_FILE_END:
                    // 文件传输结束
                    printf("[文件传输] 文件传输结束\n");
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                case MSG_TYPE_FILE_ACK:
                case MSG_TYPE_FILE_NACK:
                    // 文件传输确认
                    printf("[文件传输] 收到文件传输确认\n");
                    result = 0;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                case MSG_TYPE_FILE_COMPLETE:
                    // 51. 取消文件传输
                    if (msg.data_len >= 2 && msg.payload.data[0] == 0x51) {
                        printf("[文件传输] 取消文件传输\n");
                        result = air8000_file_transfer_cancel();
                    } else {
                        printf("[文件传输] 文件传输完成\n");
                        result = 0;
                    }
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
                default:
                    result = -1;
                    send_command_response(msg.seq_num, result, NULL, 0);
                    break;
            }
        }
    }
}

/**
 * @brief 读取传感器数据并发送到消息队列
 */
static void __attribute__((unused)) read_sensor_data() {
    // 读取所有传感器数据
    air8000_sensor_data_t sensor_data;
    if (air8000_sensor_read_all(g_ctx, &sensor_data, DEFAULT_TIMEOUT) == 0) {
        // 参考老版本工程，打印传感器数据
        printf("所有传感器 - 温度: %.2f C, 湿度: %d%%, 光照: %d, 电池: %d%%\n", 
               sensor_data.temperature, sensor_data.humidity, sensor_data.light, sensor_data.battery);
        
        // 检查消息队列是否已初始化
        if (g_mq_uart_to_mqtt != -1) {
            // 创建传感器数据消息
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            
            // 设置消息基本信息
            msg.type = MSG_TYPE_SENSOR_DATA;
            msg.seq_num = g_seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            
            // 将传感器数据格式化为JSON或其他格式
            // 这里简化处理，直接将结构体数据复制到消息中
            msg.data_len = sizeof(air8000_sensor_data_t);
            memcpy(msg.payload.data, &sensor_data, msg.data_len);
            
            // 发送消息到MQTT进程
            if (mq_send_msg(g_mq_uart_to_mqtt, &msg, 0) != 0) {
                perror("mq_send sensor data");
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
// 模拟接收UART文件数据函数已移除，使用真实的文件传输实现
// static void simulate_uart_file_receive() {
//     // 检查消息队列是否已初始化
//     // if (g_mq_uart_to_mqtt == -1) {
//     //     // 独立运行模式，不处理文件传输
//     //     return;
//     // }
//     
//     // 模拟接收到文件数据
//     // 实际应用中，这里应该从UART读取文件数据
//     
//     // 示例：创建一个简单的测试文件
//     // static const char *test_file_content = "This is a test file from UART process.\n";
//     // static bool file_sent = false;
//     
//     // 只发送一次文件，避免频繁发送
//     // if (!file_sent) {
//     //     // 1. 发送文件信息
//     //     message_t file_info_msg;
//     //     memset(&file_info_msg, 0, sizeof(file_info_msg));
//     //     
//     //     // 设置文件信息
//     //     file_info_msg.type = MSG_TYPE_FILE_INFO;
//     //     file_info_msg.seq_num = g_seq_num++;
//     //     file_info_msg.timestamp = (uint32_t)time(NULL);
//     //     
//     //     // 填充文件传输元数据
//     //     file_transfer_metadata_t *meta = &file_info_msg.payload.file_meta;
//     //     meta->file_id = 1; // 生成唯一文件ID
//     //     meta->file_size = strlen(test_file_content);
//     //     meta->total_chunks = 1; // 只有一个分片
//     //     meta->current_chunk = 0;
//     //     meta->chunk_size = strlen(test_file_content);
//     //     meta->chunk_offset = 0;
//     //     strncpy(meta->filename, "test.txt", sizeof(meta->filename) - 1);
//     //     
//     //     file_info_msg.data_len = sizeof(file_transfer_metadata_t);
//     //     
//     //     // 发送文件信息到MQTT进程
//     //     if (mq_send_msg(g_mq_uart_to_mqtt, &file_info_msg, 0) != 0) {
//     //         perror("mq_send file info");
//     //         return;
//     //     }
//     //     
//     //     // 2. 发送文件开始通知
//     //     message_t start_msg;
//     //     memset(&start_msg, 0, sizeof(start_msg));
//     //     start_msg.type = MSG_TYPE_FILE_START;
//     //     start_msg.seq_num = g_seq_num++;
//     //     start_msg.timestamp = (uint32_t)time(NULL);
//     //     
//     //     if (mq_send_msg(g_mq_uart_to_mqtt, &start_msg, 0) != 0) {
//     //         perror("mq_send file start");
//     //         return;
//     //     }
//     //     
//     //     // 3. 发送文件数据
//     //     message_t data_msg;
//     //     memset(&data_msg, 0, sizeof(data_msg));
//     //     
//     //     // 设置消息基本信息
//     //     data_msg.type = MSG_TYPE_FILE_DATA;
//     //     data_msg.seq_num = g_seq_num++;
//     //     data_msg.timestamp = (uint32_t)time(NULL);
//     //     data_msg.data_len = strlen(test_file_content);
//     //     memcpy(data_msg.payload.data, test_file_content, data_msg.data_len);
//     //     
//     //     // 发送文件数据到MQTT进程
//     //     if (mq_send_msg(g_mq_uart_to_mqtt, &data_msg, 0) != 0) {
//     //         perror("mq_send file data");
//     //         return;
//     //     }
//     //     
//     //     // 4. 发送文件结束通知
//     //     message_t end_msg;
//     //     memset(&end_msg, 0, sizeof(end_msg));
//     //     end_msg.type = MSG_TYPE_FILE_END;
//     //     end_msg.seq_num = g_seq_num++;
//     //     end_msg.timestamp = (uint32_t)time(NULL);
//     //     
//     //     if (mq_send_msg(g_mq_uart_to_mqtt, &end_msg, 0) != 0) {
//     //         perror("mq_send file end");
//     //         return;
//     //     }
//     //     
//     //     // 5. 发送文件完成通知
//     //     message_t complete_msg;
//     //     memset(&complete_msg, 0, sizeof(complete_msg));
//     //     complete_msg.type = MSG_TYPE_FILE_COMPLETE;
//     //     complete_msg.seq_num = g_seq_num++;
//     //     complete_msg.timestamp = (uint32_t)time(NULL);
//     //     complete_msg.data_len = 0;
//     //     
//     //     if (mq_send_msg(g_mq_uart_to_mqtt, &complete_msg, 0) != 0) {
//     //         perror("mq_send file complete");
//     //         return;
//     //     }
//     //     
//     //     file_sent = true;
//     // }
// }

/**
 * @brief 主函数，程序入口点
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 成功返回0，失败返回1
 * @details 初始化 Air8000 SDK，处理命令行参数，进入自动运行模式
 */
int main(int argc, char **argv) {
    /* 确保标准输出和标准错误被正确设置 */
    setvbuf(stdout, NULL, _IONBF, 0); // 禁用标准输出缓冲
    setvbuf(stderr, NULL, _IONBF, 0); // 禁用标准错误缓冲
    
    printf("[UART] 进程启动，PID: %d\n", getpid());
    
    char device_buf[64] = {0};         /* 用于存储自动探测到的设备路径 */
    const char *device = NULL;         /* 设备路径指针 */

    /* 处理命令行参数，如果提供了设备路径则使用，否则自动探测 */
    if (argc > 1) {
        device = argv[1];              /* 使用命令行参数指定的设备路径 */
        printf("[UART] 使用命令行指定的设备: %s\n", device);
    } else {
        /* 尝试使用默认设备，如果不存在则自动探测 */
        printf("[UART] 未指定设备，尝试使用默认设备: %s\n", DEFAULT_DEVICE);
        if (check_port(DEFAULT_DEVICE) == 0) {
            device = DEFAULT_DEVICE;   /* 默认设备存在，使用默认设备 */
            printf("[UART] 默认设备可用: %s\n", device);
        } else {
            /* 默认设备不存在，尝试自动探测 */
            printf("[UART] 默认设备不可用，尝试自动探测...\n");
            auto_detect_port(device_buf, sizeof(device_buf));
            if (strlen(device_buf) > 0) {
                /* 自动探测到设备，使用探测到的设备路径 */
                device = device_buf;
                printf("[UART] 已自动选择串口: %s\n", device);
            } else {
                /* 自动探测失败，仍使用默认设备，让 SDK 去处理错误 */
                device = DEFAULT_DEVICE;
                printf("[UART] 未找到可用串口，将尝试默认串口: %s\n", device);
            }
        }
    }

    /* 初始化信号处理 */
    printf("[UART] 初始化信号处理...\n");
    init_signal_handlers();
    
    /* 初始化消息队列 */
    printf("[UART] 初始化消息队列...\n");
    if (!init_message_queues()) {
        fprintf(stderr, "初始化消息队列失败\n");
        return 1;
    }
    printf("[UART] 消息队列初始化成功\n");

    /* 初始化 Air8000 SDK */
    printf("[UART] 正在初始化 Air8000 (设备: %s)...\n", device);
    g_ctx = air8000_init(device);  /* 初始化上下文，打开串口 */
    if (!g_ctx) {
        /* 初始化失败，打印错误信息并退出 */
        fprintf(stderr, "Air8000 初始化失败 (请检查权限或连接)\n");
        mq_close_queue(g_mq_uart_to_mqtt);
        mq_close_queue(g_mq_mqtt_to_uart);
        return 1;
    }
    printf("[UART] Air8000 初始化成功!\n");
    
    /* 暂时注释掉图片处理模块初始化 */
    // /* 初始化图片处理模块（包含自动处理） */
    // if (air8000_image_process_init(g_ctx, g_mq_uart_to_mqtt) != 0) {
    //     fprintf(stderr, "[UART] 图片处理模块初始化失败\n");
    //     // 图片处理模块初始化失败不影响主程序运行，继续执行
    // }
    
    /* 初始化文件传输模块 */
    if (air8000_file_transfer_init(g_ctx) != 0) {
        fprintf(stderr, "[UART] 文件传输模块初始化失败\n");
        // 文件传输模块初始化失败不影响主程序运行，继续执行
    } else {
        printf("[UART] 文件传输模块初始化成功\n");
    }
    
    /* 注册文件传输回调函数 */
    air8000_file_transfer_register_callback(file_transfer_callback, NULL);
    

    
    int sensor_read_count = 0;
    
    /* 进入自动运行循环 */
    while (running) {
        // 处理MQTT控制指令
        handle_mqtt_commands();
        
        // 每5秒读取一次传感器数据
        if (sensor_read_count % 50 == 0) {
            // read_sensor_data();
            // simulate_uart_file_receive(); // 模拟接收文件数据
        }
        
        sensor_read_count++;
        
        // 短暂休眠
        usleep(100000); // 100ms
    }

    /* 暂时注释掉图片处理模块清理 */
    // /* 清理图片处理模块 */
    // air8000_image_process_deinit();
    
    /* 清理文件传输模块 */
    air8000_file_transfer_deinit();
    printf("[UART] 文件传输模块已清理\n");
    
    /* 程序退出，销毁Air8000上下文，释放资源 */
    air8000_deinit(g_ctx);
    
    /* 关闭消息队列 */
    if (g_mq_uart_to_mqtt != -1) {
        mq_close_queue(g_mq_uart_to_mqtt);
    }
    if (g_mq_mqtt_to_uart != -1) {
        mq_close_queue(g_mq_mqtt_to_uart);
    }
    
    printf("UART process exited successfully\n");
    return 0;
}
