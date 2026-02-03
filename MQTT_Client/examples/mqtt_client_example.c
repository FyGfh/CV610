/**
 * @file mqtt_client_example.c
 * @brief MQTT客户端示例程序，连接到阿里云MQTT broker
 * @version 1.0
 * @date 2026-01-22
 * 
 * 本示例程序演示了如何使用MQTT客户端库连接到阿里云MQTT broker，
 * 包括初始化、连接、发布消息、订阅消息和断开连接等操作。
 * 
 * 主要功能：
 * - 连接到MQTT服务器
 * - 订阅控制命令主题
 * - 发布传感器数据
 * - 处理文件上传
 * - 支持FOTA升级
 * - 与UART进程通过消息队列通信
 *
 * 使用说明：
 * 1. 修改MQTT服务器配置（host、port等）
 * 2. 编译并运行程序
 * 3. 程序会自动连接到MQTT服务器并开始工作
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <errno.h>
#include <fcntl.h>
#include "mqtt_client.h"
#include "mqtt_fota.h"
#include "message_queue.h"
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/buffer.h>

// ==================== 函数前向声明 ====================

/**
 * @brief MQTT消息回调函数
 */
static void on_message_received(const char *topic, const void *payload, size_t payload_len, void *user_data);

/**
 * @brief MQTT二进制消息回调函数
 */
static void on_binary_message_received(const char *topic, const void *payload, size_t payload_len, void *user_data);

// ==================== 日志级别定义 ====================

/**
 * @brief 日志级别枚举
 */
typedef enum {
    LOG_LEVEL_DEBUG,   // 调试级别
    LOG_LEVEL_INFO,    // 信息级别
    LOG_LEVEL_WARNING, // 警告级别
    LOG_LEVEL_ERROR,   // 错误级别
    LOG_LEVEL_FATAL    // 致命级别
} log_level_t;

/**
 * @brief 当前日志级别
 */
static log_level_t g_log_level = LOG_LEVEL_DEBUG;

/**
 * @brief 日志输出函数
 */
static void log_output(log_level_t level, const char *file, int line, const char *format, ...) {
    if (level < g_log_level) {
        return;
    }
    
    // 获取当前时间
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    // 日志级别字符串
    const char *level_str = NULL;
    switch (level) {
        case LOG_LEVEL_DEBUG:
            level_str = "DEBUG";
            break;
        case LOG_LEVEL_INFO:
            level_str = "INFO";
            break;
        case LOG_LEVEL_WARNING:
            level_str = "WARNING";
            break;
        case LOG_LEVEL_ERROR:
            level_str = "ERROR";
            break;
        case LOG_LEVEL_FATAL:
            level_str = "FATAL";
            break;
        default:
            level_str = "UNKNOWN";
            break;
    }
    
    // 输出日志头部
    printf("[%s] [%s] [%s:%d] ", time_str, level_str, file, line);
    
    // 输出日志内容
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    
    // 确保换行
    printf("\n");
}

// ==================== 日志宏定义 ====================

#define LOG_DEBUG(format, ...) log_output(LOG_LEVEL_DEBUG, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) log_output(LOG_LEVEL_INFO, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOG_WARNING(format, ...) log_output(LOG_LEVEL_WARNING, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) log_output(LOG_LEVEL_ERROR, __FILE__, __LINE__, format, ##__VA_ARGS__)
#define LOG_FATAL(format, ...) log_output(LOG_LEVEL_FATAL, __FILE__, __LINE__, format, ##__VA_ARGS__)

/**
 * @brief 设置日志级别
 */
static void set_log_level(log_level_t level) {
    g_log_level = level;
    LOG_INFO("Log level set to %d", level);
}

/**
 * @brief 构建主题字符串
 * @param device_id 设备ID
 * @param topic_format 主题格式字符串
 * @return 构建好的主题字符串，需要手动释放
 */
static char *build_topic(const char *device_id, const char *topic_format)
{
    char *topic = NULL;
    int len = snprintf(NULL, 0, topic_format, device_id) + 1;
    if (len > 0) {
        topic = (char *)malloc(len);
        if (topic) {
            snprintf(topic, len, topic_format, device_id);
        }
    }
    return topic;
}

/**
 * @brief 设备ID
 */
static const char *g_device_id = NULL;

/**
 * @brief 全局客户端句柄
 */
static mqtt_client_t g_client = NULL;

/**
 * @brief 运行标志
 */
static bool g_running = false;

/**
 * @brief 消息队列描述符
 */
static int g_mq_uart_to_mqtt = -1;
static int g_mq_mqtt_to_uart = -1;

/**
 * @brief 序列号计数器
 */
static uint32_t g_seq_num = 0;

/**
 * @brief FOTA上下文
 */
static fota_context_t *g_fota_ctx = NULL;

/**
 * @brief 设备状态枚举
 */
typedef enum {
    DEVICE_STATUS_OFFLINE,    // 离线
    DEVICE_STATUS_ONLINE,     // 在线
    DEVICE_STATUS_BUSY,       // 忙碌
    DEVICE_STATUS_ERROR,      // 错误
    DEVICE_STATUS_UPDATING     // 更新中
} device_status_t;

/**
 * @brief 设备状态全局变量
 */
static device_status_t g_device_status = DEVICE_STATUS_OFFLINE;
static time_t g_last_status_publish = 0;
static const int STATUS_PUBLISH_INTERVAL = 30; // 30秒
static time_t program_start_time = 0;

/**
 * @brief 发布设备状态
 */
static bool publish_device_status(device_status_t status) {
    if (!g_client || !g_device_id) {
        LOG_ERROR("Cannot publish status: client or device_id is NULL");
        return false;
    }

    // 检查客户端状态，只在CONNECTED状态下发布
    mqtt_client_state_t client_state = mqtt_client_get_state(g_client);
    if (client_state != MQTT_CLIENT_STATE_CONNECTED) {
        LOG_WARNING("Cannot publish status: client is in %s state, not CONNECTED", 
                   client_state == MQTT_CLIENT_STATE_DISCONNECTED ? "DISCONNECTED" : 
                   client_state == MQTT_CLIENT_STATE_CONNECTING ? "CONNECTING" : 
                   client_state == MQTT_CLIENT_STATE_DISCONNECTING ? "DISCONNECTING" : "UNKNOWN");
        return false;
    }

    LOG_DEBUG("Publishing device status: %d", status);

    // 构建状态主题（与服务器订阅的主题格式匹配）
    char *status_topic = build_topic(g_device_id, "device/%s/cv610/status");
    if (!status_topic) {
        LOG_ERROR("Failed to build status topic");
        return false;
    }

    // 构建状态消息
    const char *status_str = NULL;
    switch (status) {
        case DEVICE_STATUS_OFFLINE:
            status_str = "offline";
            break;
        case DEVICE_STATUS_ONLINE:
            status_str = "online";
            break;
        case DEVICE_STATUS_BUSY:
            status_str = "busy";
            break;
        case DEVICE_STATUS_ERROR:
            status_str = "error";
            break;
        case DEVICE_STATUS_UPDATING:
            status_str = "updating";
            break;
        default:
            status_str = "unknown";
            break;
    }

    char payload[256] = {0};
    snprintf(payload, sizeof(payload), 
             "{\"status\": \"%s\", \"timestamp\": %u, \"device_id\": \"%s\"}",
             status_str, (unsigned int)time(NULL), g_device_id);

    // 准备MQTT消息
    mqtt_message_t mqtt_msg = {
        .topic = status_topic,
        .payload = payload,
        .payload_len = strlen(payload),
        .qos = MQTT_QOS_1,  // 至少一次
        .retain = false     // 非保留消息，避免服务器重启后显示已删除的设备
    };

    // 发布消息（带重试）
    int rc = -1;
    int retry_count = 0;
    const int max_retries = 3;

    while (retry_count < max_retries) {
        rc = mqtt_client_publish(g_client, &mqtt_msg);
        if (rc == MQTT_ERR_SUCCESS) {
            break;
        }

        retry_count++;
        LOG_ERROR("Failed to publish device status, retry %d/%d: %d", retry_count, max_retries, rc);
        usleep(500000); // 500ms
    }

    if (rc == MQTT_ERR_SUCCESS) {
        LOG_INFO("Published device status: %s", status_str);
        g_device_status = status;
        g_last_status_publish = time(NULL);
    } else {
        LOG_ERROR("Failed to publish device status after %d retries: %d", max_retries, rc);
    }

    // 释放主题内存
    free(status_topic);
    return (rc == MQTT_ERR_SUCCESS);
}

/**
 * @brief 检查并发布设备状态（心跳）
 */
static void check_and_publish_status() {
    time_t now = time(NULL);
    if (now - g_last_status_publish >= STATUS_PUBLISH_INTERVAL) {
        publish_device_status(g_device_status);
    }
}

/**
 * @brief Base64编码函数
 */
static char *base64_encode(const unsigned char *data, size_t len) {
    BIO *bio, *b64;
    BUF_MEM *buffer_ptr;

    b64 = BIO_new(BIO_f_base64());
    bio = BIO_new(BIO_s_mem());
    bio = BIO_push(b64, bio);

    BIO_set_flags(bio, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(bio, data, len);
    BIO_flush(bio);
    BIO_get_mem_ptr(bio, &buffer_ptr);

    char *encoded = (char *)malloc(buffer_ptr->length + 1);
    if (encoded) {
        memcpy(encoded, buffer_ptr->data, buffer_ptr->length);
        encoded[buffer_ptr->length] = '\0';
    }

    BIO_free_all(bio);
    return encoded;
}

/**
 * @brief 计算MD5校验和
 */
static char *calculate_md5(const unsigned char *data, size_t len) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return NULL;

    unsigned char md5_hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    if (EVP_DigestInit_ex(ctx, EVP_md5(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, md5_hash, &hash_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return NULL;
    }

    char *md5_str = (char *)malloc(hash_len * 2 + 1);
    if (md5_str) {
        for (unsigned int i = 0; i < hash_len; i++) {
            snprintf(md5_str + i * 2, 3, "%02x", md5_hash[i]);
        }
        md5_str[hash_len * 2] = '\0';
    }

    EVP_MD_CTX_free(ctx);
    return md5_str;
}

/**
 * @brief FOTA回调函数
 */
static void fota_callback_handler(fota_context_t *ctx, fota_state_t state, fota_error_t error, void *user_data) {
    (void)user_data;
    
    printf("FOTA Callback: state=%d, error=%d, progress=%d%%\n", 
           state, error, ctx->progress);
    
    switch (state) {
        case FOTA_STATE_RECEIVING:
            printf("FOTA: Receiving... %d%%\n", ctx->progress);
            break;
        case FOTA_STATE_COMPLETE:
            printf("FOTA: Receiving complete\n");
            break;
        case FOTA_STATE_SAVED:
            printf("FOTA: File saved successfully to %s\n", ctx->file_path);
            // 发送FOTA完成消息给UART
            if (g_mq_mqtt_to_uart != -1) {
                message_t complete_msg;
                memset(&complete_msg, 0, sizeof(complete_msg));
                complete_msg.type = MSG_TYPE_FOTA_COMPLETE;
                complete_msg.seq_num = g_seq_num++;
                complete_msg.timestamp = (uint32_t)time(NULL);
                complete_msg.data_len = 0;
                
                if (mq_send_msg(g_mq_mqtt_to_uart, &complete_msg, 0) != 0) {
                    perror("mq_send fota complete");
                } else {
                    printf("Sent FOTA complete notification to UART\n");
                }
            }
            break;
        case FOTA_STATE_FAILED:
            printf("FOTA: Failed with error %d\n", error);
            // 发送FOTA失败消息给UART
            if (g_mq_mqtt_to_uart != -1) {
                message_t error_msg;
                memset(&error_msg, 0, sizeof(error_msg));
                error_msg.type = MSG_TYPE_FOTA_COMPLETE;
                error_msg.seq_num = g_seq_num++;
                error_msg.timestamp = (uint32_t)time(NULL);
                error_msg.data_len = 4;
                int32_t error_code = -error;
                memcpy(error_msg.payload.data, &error_code, 4);
                
                if (mq_send_msg(g_mq_mqtt_to_uart, &error_msg, 0) != 0) {
                    perror("mq_send fota error");
                } else {
                    printf("Sent FOTA error notification to UART\n");
                }
            }
            break;
        default:
            break;
    }
}

/**
 * @brief 信号处理函数：用于优雅退出
 */
static void signal_handler(int sig) {
    printf("\nReceived signal %d, stopping MQTT client...\n", sig);
    g_running = false;
}

/**
 * @brief 处理文件分片上传
 */
static bool handle_file_chunk_upload(const char *device_id, const char *file_path) {
    // 创建上传上下文
    fota_upload_context_t *upload_ctx = fota_upload_create(file_path, 16384);
    if (!upload_ctx) {
        printf("Failed to create upload context\n");
        return false;
    }

    // 开始上传
    if (!fota_upload_start(upload_ctx)) {
        printf("Failed to start upload\n");
        fota_upload_destroy(upload_ctx);
        return false;
    }

    printf("Starting file upload: %s, size: %llu bytes, chunks: %u\n", 
           upload_ctx->filename, (unsigned long long)upload_ctx->file_size, upload_ctx->total_chunks);

    // 构建文件上传主题
    char *file_topic = build_topic(device_id, "device/%s/file/upload");
    if (!file_topic) {
        printf("Failed to build file topic\n");
        fota_upload_destroy(upload_ctx);
        return false;
    }

    // 分片上传循环
    uint8_t *chunk_data = NULL;
    size_t chunk_data_len = 0;
    uint32_t chunk_id = 0;

    while (fota_upload_get_next_chunk(upload_ctx, &chunk_data, &chunk_data_len, &chunk_id)) {
        // 编码分片数据为base64
        char *base64_data = base64_encode(chunk_data, chunk_data_len);
        if (!base64_data) {
            printf("Failed to encode chunk data\n");
            free(chunk_data);
            break;
        }

        // 计算分片校验和
        char *checksum = calculate_md5(chunk_data, chunk_data_len);
        if (!checksum) {
            printf("Failed to calculate checksum\n");
            free(base64_data);
            free(chunk_data);
            break;
        }

        // 构建分片上传消息
        char payload[4096] = {0};
        snprintf(payload, sizeof(payload), 
                 "{\"type\":\"chunk\",\"file_id\":\"%s\",\"chunk_index\":%u,\"total_chunks\":%u,\"filename\":\"%s\",\"chunk_data\":\"%s\",\"checksum\":\"%s\"}",
                 upload_ctx->file_id, chunk_id, upload_ctx->total_chunks, 
                 upload_ctx->filename, base64_data, checksum);

        // 发布分片上传消息（带重试）
        mqtt_message_t mqtt_msg = {
            .topic = file_topic,
            .payload = payload,
            .payload_len = strlen(payload),
            .qos = MQTT_QOS_1,
            .retain = false
        };

        int rc = -1;
        int retry_count = 0;
        const int max_retries = 3;

        while (retry_count < max_retries) {
            rc = mqtt_client_publish(g_client, &mqtt_msg);
            if (rc == MQTT_ERR_SUCCESS) {
                break;
            }

            retry_count++;
            printf("Failed to publish chunk %u, retry %d/%d: %d\n", chunk_id, retry_count, max_retries, rc);
            
            // 短暂延迟后重试
            usleep(500000); // 500ms
        }

        if (rc != MQTT_ERR_SUCCESS) {
            printf("Failed to publish chunk %u after %d retries: %d\n", chunk_id, max_retries, rc);
            free(checksum);
            free(base64_data);
            free(chunk_data);
            break;
        }

        printf("Published chunk %u/%u, size: %zu bytes\n", 
               chunk_id, upload_ctx->total_chunks, chunk_data_len);

        // 释放资源
        free(checksum);
        free(base64_data);
        free(chunk_data);
        chunk_data = NULL;

        // 短暂延迟，避免消息发送过快
        usleep(10000); // 10ms
    }

    // 检查上传是否完成
    if (upload_ctx->current_chunk >= upload_ctx->total_chunks) {
        // 构建合并分片消息
        char merge_payload[256] = {0};
        snprintf(merge_payload, sizeof(merge_payload), 
                 "{\"type\":\"merge\",\"file_id\":\"%s\"}",
                 upload_ctx->file_id);

        mqtt_message_t merge_msg = {
            .topic = file_topic,
            .payload = merge_payload,
            .payload_len = strlen(merge_payload),
            .qos = MQTT_QOS_1,
            .retain = false
        };

        int rc = mqtt_client_publish(g_client, &merge_msg);
        if (rc != MQTT_ERR_SUCCESS) {
            printf("Failed to publish merge message: %d\n", rc);
        } else {
            printf("File upload completed successfully\n");
        }
    }

    // 清理资源
    fota_upload_finish(upload_ctx);
    fota_upload_destroy(upload_ctx);
    free(file_topic);

    return true;
}

/**
 * @brief MQTT客户端状态变化回调
 */
static void on_client_state_change(mqtt_client_state_t state, void *user_data)
{
    (void)user_data;
    printf("Client State Changed: %d\n", state);
    
    switch (state) {
        case MQTT_CLIENT_STATE_DISCONNECTED:
            printf("Client: Disconnected\n");
            // 发布错误状态
            publish_device_status(DEVICE_STATUS_ERROR);
            // 记录断开连接原因
            printf("Network connection lost, attempting to reconnect...\n");
            break;
        case MQTT_CLIENT_STATE_CONNECTING:
            printf("Client: Connecting...\n");
            // 发布忙碌状态
            publish_device_status(DEVICE_STATUS_BUSY);
            break;
        case MQTT_CLIENT_STATE_CONNECTED:
            printf("Client: Connected\n");
            // 发布在线状态
            publish_device_status(DEVICE_STATUS_ONLINE);
            // 重新订阅主题
            printf("Re-subscribing to topics...\n");
            
            // 重新订阅CV610升级命令主题（与服务器发布的主题格式匹配）
            char *command_topic = build_topic(g_device_id, "device/%s/cv610/upgrade");
            if (command_topic) {
                int rc = mqtt_client_subscribe(g_client, command_topic, MQTT_QOS_1, on_message_received, NULL);
                if (rc != MQTT_ERR_SUCCESS) {
                    printf("Failed to re-subscribe to topic %s: %d\n", command_topic, rc);
                } else {
                    printf("Re-subscribed to topic: %s\n", command_topic);
                }
                free(command_topic);
            }
            
            // 重新订阅升级响应主题
            char *upgrade_response_topic = build_topic(g_device_id, "device/%s/cv610/upgrade/response");
            if (upgrade_response_topic) {
                int rc = mqtt_client_subscribe(g_client, upgrade_response_topic, MQTT_QOS_1, NULL, NULL);
                if (rc != MQTT_ERR_SUCCESS) {
                    printf("Failed to re-subscribe to topic %s: %d\n", upgrade_response_topic, rc);
                } else {
                    printf("Re-subscribed to topic: %s\n", upgrade_response_topic);
                }
                free(upgrade_response_topic);
            }
            break;
        case MQTT_CLIENT_STATE_DISCONNECTING:
            printf("Client: Disconnecting...\n");
            break;
        default:
            printf("Client: Unknown state\n");
            break;
    }
}

/**
 * @brief 处理FOTA主题消息
 */
static void handle_fota_topic(const char *topic, const void *payload, size_t payload_len) {
    printf("FOTA Command Received: Topic=%s, Length=%zu\n", topic, payload_len);
    
    // 检查是否是FOTA开始指令
    if (strstr(topic, "/fota/start") != NULL) {
        printf("FOTA Start Command Received\n");
        
        // 解析FOTA开始参数（文件大小和分片数）
        uint64_t file_size = 0;
        uint32_t total_chunks = 0;
        
        // 简单解析：假设payload是JSON格式 {"file_size": 123456, "total_chunks": 10}
        // 这里简化处理，实际应用中应该使用JSON解析库
        char *payload_str = (char *)payload;
        sscanf(payload_str, "{\"file_size\": %llu, \"total_chunks\": %u}", &file_size, &total_chunks);
        
        if (file_size == 0 || total_chunks == 0) {
            printf("Invalid FOTA start parameters\n");
            return;
        }
        
        // 销毁旧的FOTA上下文
        if (g_fota_ctx) {
            fota_destroy(g_fota_ctx);
            g_fota_ctx = NULL;
        }
        
        // 创建新的FOTA上下文
        g_fota_ctx = fota_create(NULL, NULL, fota_callback_handler, NULL);
        if (!g_fota_ctx) {
            printf("Failed to create FOTA context\n");
            return;
        }
        
        // 开始FOTA接收
        if (!fota_start(g_fota_ctx, file_size, total_chunks)) {
            printf("Failed to start FOTA reception\n");
            fota_destroy(g_fota_ctx);
            g_fota_ctx = NULL;
            return;
        }
        
        printf("FOTA reception started: file_size=%llu, total_chunks=%u\n", file_size, total_chunks);
    }
    
    // 检查是否是FOTA结束指令
    if (strstr(topic, "/fota/end") != NULL) {
        printf("FOTA End Command Received\n");
        
        if (!g_fota_ctx) {
            printf("No active FOTA context\n");
            return;
        }
        
        // 解析校验和
        uint32_t checksum = 0;
        char *payload_str = (char *)payload;
        sscanf(payload_str, "{\"checksum\": %u}", &checksum);
        
        // 结束FOTA接收
        if (!fota_finish(g_fota_ctx, checksum)) {
            printf("Failed to finish FOTA reception\n");
        } else {
            printf("FOTA reception finished successfully\n");
        }
    }
}

/**
 * @brief MQTT二进制消息回调函数
 * 
 * 此函数是MQTT客户端的二进制消息回调函数，当接收到二进制数据时被调用。
 * 函数会处理FOTA升级的二进制数据，并将数据保存到文件。
 * 
 * @param topic 消息主题
 * @param payload 消息内容
 * @param payload_len 消息内容长度
 * @param user_data 用户数据（未使用）
 */
static void on_binary_message_received(const char *topic, const void *payload, size_t payload_len, void *user_data) {
    (void)user_data;  // 未使用的参数
    
    // 打印接收到的二进制消息
    printf("Binary Message Received: Topic=%s, Length=%zu\n", 
           topic, payload_len);
    
    // 检查是否是FOTA数据主题
    if (strstr(topic, "/fota/data") != NULL || strstr(topic, "/command/binary") != NULL) {
        // 检查是否有活动的FOTA上下文
        if (!g_fota_ctx) {
            printf("No active FOTA context for binary data\n");
            return;
        }
        
        // 解析分片编号（假设前4字节是分片编号）
        if (payload_len < 4) {
            printf("Invalid FOTA chunk: too short\n");
            return;
        }
        
        uint32_t chunk_id = *((uint32_t *)payload);
        const uint8_t *chunk_data = (const uint8_t *)payload + 4;
        size_t chunk_data_len = payload_len - 4;
        
        if (chunk_data_len == 0) {
            printf("Invalid FOTA chunk: no data\n");
            return;
        }
        
        // 处理FOTA分片
        if (!fota_process_chunk(g_fota_ctx, chunk_id, chunk_data, chunk_data_len)) {
            printf("Failed to process FOTA chunk %u\n", chunk_id);
            // 可以在这里实现错误处理和重传逻辑
        } else {
            printf("Processed FOTA chunk %u, data length: %zu\n", chunk_id, chunk_data_len);
        }
    } else {
        // 其他二进制消息，继续转发给UART
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        
        // 设置消息基本信息
        msg.type = MSG_TYPE_RESPONSE;
        msg.seq_num = g_seq_num++;
        msg.timestamp = (uint32_t)time(NULL);
        msg.data_len = payload_len;
        memcpy(msg.payload.data, payload, payload_len);
        
        // 发送消息到UART进程
        if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
            perror("mq_send binary data");
        } else {
            printf("Sent binary data to UART process, length: %zu\n", payload_len);
        }
    }
}

/**
 * @brief MQTT消息回调函数
 * 
 * 此函数是MQTT客户端的消息回调函数，当接收到MQTT消息时被调用。
 * 函数会根据消息主题和内容进行处理，并将消息转发给UART进程。
 * 
 * @param topic 消息主题
 * @param payload 消息内容
 * @param payload_len 消息内容长度
 * @param user_data 用户数据（未使用）
 * 
 * @note 消息处理流程：
 * 1. 打印接收到的消息
 * 2. 检查是否是命令相关主题
 * 3. 根据消息内容确定消息类型
 * 4. 处理FOTA相关消息
 * 5. 处理电机控制消息
 * 6. 处理其他设备控制消息
 * 7. 将消息转发给UART进程
 * 
 * @attention 消息类型：
 * - MSG_TYPE_RESPONSE: 通用响应消息
 * - MSG_TYPE_FOTA_DATA: FOTA数据消息
 * - MSG_TYPE_FOTA_START: FOTA开始指令
 * - MSG_TYPE_MOTOR_CMD: 电机控制命令
 * - MSG_TYPE_DEVICE_CMD: 设备控制命令
 */
static void on_message_received(const char *topic, const void *payload, size_t payload_len, void *user_data)
{
    (void)user_data;  // 未使用的参数
    
    // 打印接收到的消息
    printf("Message Received: Topic=%s, Payload=%.*s\n", 
           topic, (int)payload_len, (char*)payload);
    
    // 检查是否是命令相关主题
    if (strstr(topic, "/command") != NULL) {
        // 处理控制命令消息
        message_t msg;
        memset(&msg, 0, sizeof(msg));
        
        // 设置消息基本信息
        msg.type = MSG_TYPE_RESPONSE;          // 默认消息类型
        msg.seq_num = g_seq_num++;             // 序列号
        msg.timestamp = (uint32_t)time(NULL);  // 时间戳
        
        // 尝试解析Python MQTT服务器发送的命令格式
        char *payload_str = (char *)payload;
        char *content_start = strstr(payload_str, "\"content\":");
        
        if (content_start) {
            // 找到content字段，提取实际命令内容
            content_start += 10; // 跳过"content":
            char *content_end = strrchr(content_start, '}');
            if (content_end) {
                // 计算内容长度
                size_t content_len = content_end - content_start + 1;
                if (content_len > 0 && content_len < sizeof(msg.payload.data)) {
                    // 复制实际命令内容
                    memcpy(msg.payload.data, content_start, content_len);
                    msg.data_len = content_len;
                    printf("Extracted command content: %.*s\n", (int)content_len, (char*)msg.payload.data);
                }
            }
        }
        
        // 如果没有提取到content字段，使用原始payload
        if (msg.data_len == 0) {
            msg.data_len = payload_len;
            memcpy(msg.payload.data, payload, payload_len);
        }
        
        // 根据消息内容确定消息类型
        if (strstr((char*)msg.payload.data, "fota") != NULL || strstr((char*)msg.payload.data, "FOTA") != NULL) {
            // FOTA相关消息，直接在MQTT客户端处理
            handle_fota_topic(topic, msg.payload.data, msg.data_len);
        } else if (strstr((char*)msg.payload.data, "motor") != NULL || strstr((char*)msg.payload.data, "MOTOR") != NULL) {
            // 电机控制消息
            msg.type = MSG_TYPE_MOTOR_CMD;
        } else if (strstr((char*)msg.payload.data, "status") != NULL || strstr((char*)msg.payload.data, "STATUS") != NULL) {
            // 状态查询消息，直接在MQTT客户端处理
            printf("Received status query command\n");
            // 发布当前设备状态
            publish_device_status(g_device_status);
            // 跳过发送到UART，直接响应
            return;
        } else {
            // 其他设备控制消息
            msg.type = MSG_TYPE_DEVICE_CMD;
            
            // 尝试将Python MQTT服务器发送的命令转换为与process_manager相同的格式
            // 格式：device_type (1字节) + device_id (1字节) + state (1字节)
            char *cmd_str = (char *)msg.payload.data;
            
            // 检查是否包含action字段
            char *action_start = strstr(cmd_str, "\"action\":");
            if (action_start) {
                action_start += 9; // 跳过"action":
                
                // 提取action值
                char *action_value_start = action_start;
                while (*action_value_start == ' ' || *action_value_start == '"') {
                    action_value_start++;
                }
                char *action_value_end = strchr(action_value_start, '"');
                if (action_value_end) {
                    *action_value_end = '\0'; // 临时截断字符串
                    char *action = action_value_start;
                    
                    // 根据action映射到对应的设备类型
                    uint8_t device_type = 0;
                    uint8_t device_id = 0;
                    uint8_t state = 0;
                    
                    if (strcmp(action, "led") == 0 || strcmp(action, "LED") == 0) {
                        device_type = 0; // LED
                        device_id = 0;
                        // 尝试提取亮度值
                        char *value_start = strstr(cmd_str, "\"value\":");
                        if (value_start) {
                            state = atoi(value_start + 8);
                        }
                    } else if (strcmp(action, "fan") == 0 || strcmp(action, "FAN") == 0) {
                        device_type = 1; // FAN
                        device_id = 1;
                        // 尝试提取状态值
                        char *status_start = strstr(cmd_str, "\"status\":");
                        if (status_start) {
                            state = atoi(status_start + 9);
                        }
                    } else if (strcmp(action, "heater") == 0 || strcmp(action, "HEATER") == 0) {
                        device_type = 2; // HEATER
                        device_id = 2;
                        // 尝试提取状态值
                        char *status_start = strstr(cmd_str, "\"status\":");
                        if (status_start) {
                            state = atoi(status_start + 9);
                        }
                    } else if (strcmp(action, "laser") == 0 || strcmp(action, "LASER") == 0) {
                        device_type = 3; // LASER
                        device_id = 3;
                        // 尝试提取状态值
                        char *status_start = strstr(cmd_str, "\"status\":");
                        if (status_start) {
                            state = atoi(status_start + 9);
                        }
                    } else if (strcmp(action, "pwm") == 0 || strcmp(action, "PWM") == 0) {
                        device_type = 4; // PWM LIGHT
                        device_id = 4;
                        // 尝试提取亮度值
                        char *value_start = strstr(cmd_str, "\"value\":");
                        if (value_start) {
                            state = atoi(value_start + 8);
                        }
                    }
                    
                    // 恢复字符串
                    *action_value_end = '"';
                    
                    // 如果成功映射到设备类型，构建新的消息格式
                    if (device_type <= 4) {
                        uint8_t new_payload[3];
                        new_payload[0] = device_type;
                        new_payload[1] = device_id;
                        new_payload[2] = state;
                        
                        // 复制到消息payload
                        memcpy(msg.payload.data, new_payload, 3);
                        msg.data_len = 3;
                        printf("Converted command to process_manager format: device_type=%d, device_id=%d, state=%d\n", device_type, device_id, state);
                    }
                }
            }
        }
        
        // 发送消息到UART进程（跳过已处理的FOTA消息）
        if (msg.type != MSG_TYPE_FOTA_DATA && msg.type != MSG_TYPE_FOTA_START && msg.type != MSG_TYPE_FOTA_END) {
            if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
                perror("mq_send");
            } else {
                printf("Sent control message to UART process, type: %d\n", msg.type);
            }
        }
        return;
    }
    
    // 处理非命令主题的消息
    message_t msg;
    memset(&msg, 0, sizeof(msg));
    
    // 设置消息基本信息
    msg.type = MSG_TYPE_RESPONSE;          // 消息类型
    msg.seq_num = g_seq_num++;             // 序列号
    msg.timestamp = (uint32_t)time(NULL);  // 时间戳
    msg.data_len = payload_len;            // 数据长度
    memcpy(msg.payload.data, payload, payload_len);  // 复制消息内容
    
    // 发送消息到UART进程
    if (mq_send_msg(g_mq_mqtt_to_uart, &msg, 0) != 0) {
        perror("mq_send");
    } else {
        printf("Sent control message to UART process, type: %d\n", msg.type);
    }
}

/**
 * @brief 初始化消息队列
 */
static bool init_message_queues(void)
{
    printf("Initializing message queues...\n");
    
    const int max_retries = 3;
    int retry_count = 0;
    
    // 打开UART到MQTT的消息队列（带重试）
    while (retry_count < max_retries) {
        g_mq_uart_to_mqtt = mq_open_existing(MSG_QUEUE_UART_TO_MQTT, O_RDONLY);
        if (g_mq_uart_to_mqtt != -1) {
            break;
        }
        
        retry_count++;
        perror("mq_open uart_to_mqtt");
        printf("Retrying message queue initialization (%d/%d)...\n", retry_count, max_retries);
        usleep(1000000); // 1秒
    }
    
    if (g_mq_uart_to_mqtt == -1) {
        printf("Failed to open uart_to_mqtt message queue after %d retries\n", max_retries);
        return false;
    }
    
    // 重置重试计数
    retry_count = 0;
    
    // 打开MQTT到UART的消息队列（带重试）
    while (retry_count < max_retries) {
        g_mq_mqtt_to_uart = mq_open_existing(MSG_QUEUE_MQTT_TO_UART, O_WRONLY);
        if (g_mq_mqtt_to_uart != -1) {
            break;
        }
        
        retry_count++;
        perror("mq_open mqtt_to_uart");
        printf("Retrying message queue initialization (%d/%d)...\n", retry_count, max_retries);
        usleep(1000000); // 1秒
    }
    
    if (g_mq_mqtt_to_uart == -1) {
        printf("Failed to open mqtt_to_uart message queue after %d retries\n", max_retries);
        mq_close_queue(g_mq_uart_to_mqtt);
        g_mq_uart_to_mqtt = -1;
        return false;
    }
    
    printf("Message queues initialized successfully\n");
    return true;
}

/**
 * @brief 初始化MQTT客户端
 * 
 * 此函数初始化MQTT客户端，配置连接参数，并创建客户端实例。
 * 
 * @return 成功返回true，失败返回false
 * 
 * @note 初始化流程：
 * 1. 设置日志级别
 * 2. 配置MQTT服务器连接参数
 * 3. 配置客户端参数
 * 4. 创建MQTT客户端实例
 * 5. 设置状态变化回调
 * 
 * @attention 需要根据实际情况修改以下配置：
 * - host: MQTT服务器公网IP
 * - port: MQTT服务器端口
 * - client_id: 客户端唯一标识
 * - device_id: 设备唯一标识
 */
/**
 * @brief 测试网络连接
 * @param host 主机地址
 * @param port 端口
 * @return true表示连接成功，false表示失败
 */
static bool test_network_connection(const char *host, int port)
{
    LOG_INFO("=== Network Connection Test Started ===");
    LOG_INFO("Testing network connection to %s:%d", host, port);
    
    const int max_retries = 3;
    const int retry_delay_ms = 1000;
    bool test_passed = false;
    
    // 声明serv_addr变量，使其在整个函数中可用
    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    
    for (int retry = 0; retry < max_retries; retry++) {
        if (retry > 0) {
            LOG_INFO("Network test retry %d/%d", retry + 1, max_retries);
            usleep(retry_delay_ms * 1000); // 1秒延迟
        }
        
        time_t start_time = time(NULL);
        
        // 1. 测试DNS解析（如果需要）
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(port);
        
        LOG_DEBUG("Step 1: Testing address resolution for %s", host);
        if (inet_pton(AF_INET, host, &serv_addr.sin_addr) <= 0) {
            LOG_ERROR("Address resolution failed for %s", host);
            LOG_WARNING("Assuming %s is not an IP address, skipping DNS test", host);
            // 继续测试，因为可能是主机名
        } else {
            char ip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &serv_addr.sin_addr, ip_str, sizeof(ip_str));
            LOG_INFO("Address resolution successful: %s -> %s", host, ip_str);
        }
        
        // 2. 创建socket
        LOG_DEBUG("Step 2: Creating socket");
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            LOG_ERROR("Failed to create socket: %s", strerror(errno));
            LOG_ERROR("Socket creation failed, retrying...");
            continue;
        }
        
        // 设置socket为非阻塞模式
        int flags = fcntl(sockfd, F_GETFL, 0);
        if (flags >= 0) {
            if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                LOG_WARNING("Failed to set non-blocking mode: %s", strerror(errno));
                LOG_WARNING("Continuing with blocking mode");
            } else {
                LOG_DEBUG("Socket set to non-blocking mode");
            }
        }
        
        // 3. 尝试连接
        LOG_DEBUG("Step 3: Attempting connection to %s:%d", host, port);
        int connect_rc = connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
        
        if (connect_rc < 0) {
            if (errno == EINPROGRESS) {
                // 连接正在进行中，使用select等待
                LOG_DEBUG("Connection in progress, waiting with select...");
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(sockfd, &writefds);
                
                struct timeval timeout;
                timeout.tv_sec = 5;
                timeout.tv_usec = 0;
                
                int select_rc = select(sockfd + 1, NULL, &writefds, NULL, &timeout);
                if (select_rc > 0) {
                    // 检查连接是否成功
                    int error = 0;
                    socklen_t len = sizeof(error);
                    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
                    if (error == 0) {
                        // 连接成功
                        time_t end_time = time(NULL);
                        LOG_INFO("Network connection test successful (non-blocking)");
                        LOG_INFO("Connection time: %d seconds", (int)(end_time - start_time));
                        close(sockfd);
                        test_passed = true;
                        break;
                    } else {
                        LOG_ERROR("Connection failed after select: %s", strerror(error));
                        LOG_ERROR("Error code: %d", error);
                        switch (error) {
                            case ECONNREFUSED:
                                LOG_ERROR("Connection refused - check if MQTT broker is running on port %d", port);
                                break;
                            case ETIMEDOUT:
                                LOG_ERROR("Connection timed out - check network latency or firewall");
                                break;
                            case EHOSTUNREACH:
                                LOG_ERROR("Host unreachable - check network connectivity");
                                break;
                            default:
                                LOG_ERROR("Unknown connection error");
                                break;
                        }
                    }
                } else if (select_rc == 0) {
                    LOG_ERROR("Connection timeout after 5 seconds");
                    LOG_ERROR("Possible issues: firewall blocking, broker not running, network issues");
                } else {
                    LOG_ERROR("Select failed: %s", strerror(errno));
                    LOG_ERROR("Select error, retrying...");
                }
            } else {
                LOG_ERROR("Connection failed: %s", strerror(errno));
                LOG_ERROR("Error code: %d", errno);
                switch (errno) {
                    case ECONNREFUSED:
                        LOG_ERROR("Connection refused - check if MQTT broker is running on port %d", port);
                        break;
                    case ETIMEDOUT:
                        LOG_ERROR("Connection timed out - check network latency or firewall");
                        break;
                    case EHOSTUNREACH:
                        LOG_ERROR("Host unreachable - check network connectivity");
                        break;
                    default:
                        LOG_ERROR("Unknown connection error");
                        break;
                }
            }
            close(sockfd);
            continue;
        }
        
        // 连接成功
        time_t end_time = time(NULL);
        LOG_INFO("Network connection test successful (blocking)");
        LOG_INFO("Connection time: %d seconds", (int)(end_time - start_time));
        close(sockfd);
        test_passed = true;
        break;
    }
    
    // 4. 端口扫描测试
    LOG_DEBUG("Step 4: Performing port scan for common MQTT ports");
    int common_ports[] = {1883, 8883, 8083, 80, 443};
    int port_count = sizeof(common_ports) / sizeof(common_ports[0]);
    
    for (int i = 0; i < port_count; i++) {
        int test_port = common_ports[i];
        if (test_port == port) {
            continue; // 跳过已经测试过的端口
        }
        
        LOG_DEBUG("Testing port %d...", test_port);
        int test_sock = socket(AF_INET, SOCK_STREAM, 0);
        if (test_sock < 0) {
            continue;
        }
        
        // 设置超时
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(test_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        setsockopt(test_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        
        int rc = connect(test_sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
        if (rc == 0) {
            LOG_INFO("Port %d is open", test_port);
        }
        close(test_sock);
    }
    
    if (test_passed) {
        LOG_INFO("=== Network Connection Test PASSED ===");
        return true;
    } else {
        LOG_WARNING("=== Network Connection Test FAILED ===");
        LOG_WARNING("Network connection test failed after %d retries, but will continue", max_retries);
        LOG_WARNING("Possible issues:");
        LOG_WARNING("1. MQTT broker not running on %s:%d", host, port);
        LOG_WARNING("2. Firewall blocking port %d", port);
        LOG_WARNING("3. Network connectivity issues");
        LOG_WARNING("4. Incorrect server address or port");
        return false;
    }
}

static bool init_mqtt_client(void)
{
    // 设置日志级别为DEBUG，便于调试
    mqtt_set_log_level(MQTT_LOG_LEVEL_DEBUG);
    
    // 阿里云MQTT broker配置
    // 请根据实际情况修改以下配置
    const char *host = "47.107.225.196";  // 修改为你的公网IP
    int port = 1883; // 或8883（TLS）
    const char *client_id = "hi3516cv610-device-001";  // 客户端唯一标识
    const char *username = "";  // 用户名（可选）
    const char *password = "";  // 密码（可选）
    
    // 设备ID，用于构建主题
    const char *device_id = "hi3516cv610-device-001";  // 设备唯一标识
    g_device_id = device_id;
    
    // 测试网络连接
    if (!test_network_connection(host, port)) {
        LOG_WARNING("Network connection test failed, but continuing with MQTT initialization");
    }
    
    // 配置MQTT客户端
    mqtt_client_config_t client_config = {
        .host = host,              // MQTT服务器地址
        .port = port,              // MQTT服务器端口
        .client_id = client_id,    // 客户端ID
        .username = username,      // 用户名
        .password = password,      // 密码
        .keep_alive = 60,          // 心跳间隔（秒）
        .clean_session = true,     // 清除会话标志
        // TLS相关配置已注释掉，不再使用TLS
        // .use_tls = false, // 如果使用TLS，设置为true并配置证书
        // .ca_cert_path = NULL,
        // .client_cert_path = NULL,
        // .client_key_path = NULL,
        .connect_timeout_ms = 5000,     // 连接超时（毫秒）
        .retry_interval_ms = 2000,      // 重连间隔（毫秒）
        .max_retry_count = -1           // 无限重试
    };
    
    LOG_INFO("Creating MQTT client with config: host=%s, port=%d, client_id=%s", 
             host, port, client_id);
    
    // 创建MQTT客户端实例
    g_client = mqtt_client_create(&client_config);
    if (g_client == NULL) {
        LOG_ERROR("Failed to create MQTT client");
        return false;
    }
    
    LOG_INFO("MQTT client created successfully");
    
    // 设置状态变化回调，用于接收连接状态变化通知
    mqtt_client_set_state_callback(g_client, on_client_state_change, NULL);
    LOG_INFO("State callback set");
    
    return true;
}

/**
 * @brief 从消息队列接收传感器数据并发布到MQTT
 * 
 * 此函数从UART进程通过消息队列发送的数据，根据消息类型处理并发布到相应的MQTT主题。
 * 
 * @note 消息处理流程：
 * 1. 从消息队列接收数据（超时100ms）
 * 2. 根据消息类型进行处理
 * 3. 构建相应的MQTT主题
 * 4. 发布消息到MQTT服务器
 * 5. 释放资源
 * 
 * @attention 支持的消息类型：
 * - MSG_TYPE_SENSOR_DATA: 传感器数据
 * - MSG_TYPE_FILE_INFO: 文件信息
 * - MSG_TYPE_FILE_START: 文件传输开始
 * - MSG_TYPE_FILE_DATA: 文件数据
 * - MSG_TYPE_FILE_END: 文件传输结束
 * - MSG_TYPE_FILE_COMPLETE: 文件传输完成
 * - MSG_TYPE_FOTA_COMPLETE: FOTA升级完成
 */
static void handle_sensor_data()
{
    // 检查消息队列是否有效
    if (g_mq_uart_to_mqtt == -1) {
        LOG_DEBUG("Message queue not initialized, skipping sensor data handling");
        return;
    }
    
    message_t msg;         // 消息结构体
    unsigned int priority;  // 消息优先级
    
    // 从消息队列接收数据，超时100ms
    int ret = mq_receive_msg(g_mq_uart_to_mqtt, &msg, &priority, 100);
    if (ret == 0) {
        // 成功接收到消息
        switch (msg.type) {
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_SENSOR_DATA: {
                // 处理传感器数据
                LOG_DEBUG("Processing sensor data message");
                // 构建传感器数据主题：device/{device_id}/data
                char *sensor_topic = build_topic(g_device_id, "device/%s/data");
                if (sensor_topic) {
                    // 准备MQTT消息
                    mqtt_message_t mqtt_msg = {
                        .topic = sensor_topic,        // 消息主题
                        .payload = msg.payload.data,  // 消息内容
                        .payload_len = msg.data_len,  // 消息长度
                        .qos = MQTT_QOS_0,            // QoS级别0（最多一次）
                        .retain = false               // 非保留消息
                    };
                    
                    // 检查MQTT客户端是否有效
                    if (g_client) {
                        // 发布消息
                        int rc = mqtt_client_publish(g_client, &mqtt_msg);
                        if (rc != MQTT_ERR_SUCCESS) {
                            LOG_ERROR("Failed to publish sensor data: %d", rc);
                        } else {
                            LOG_INFO("Published sensor data to %s", sensor_topic);
                        }
                    } else {
                        LOG_ERROR("MQTT client not initialized, cannot publish sensor data");
                    }
                    // 释放主题内存
                    free(sensor_topic);
                } else {
                    LOG_ERROR("Failed to build sensor topic");
                }
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////  
            case MSG_TYPE_FILE_INFO: {
                // 处理文件信息
                LOG_DEBUG("Processing file info message");
                file_transfer_metadata_t *meta = &msg.payload.file_meta;
                LOG_INFO("Received file info: file_id=%u, filename=%s, size=%llu, chunks=%u",
                       meta->file_id, meta->filename, (unsigned long long)meta->file_size, meta->total_chunks);
                
                // 可以在这里准备接收文件，比如创建文件等
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_FILE_START: {
                // 处理文件传输开始
                LOG_INFO("File transfer started");
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_FILE_DATA: {
                // 处理文件数据
                LOG_DEBUG("Processing file data message, length: %d", msg.data_len);
                // 这里假设msg.payload.data包含文件路径
                if (msg.data_len > 0 && msg.payload.data) {
                    char file_path[1024] = {0};
                    strncpy(file_path, (char *)msg.payload.data, msg.data_len);
                    file_path[msg.data_len] = '\0';
                    
                    // 使用分片上传处理文件
                    LOG_INFO("Handling file chunk upload: %s", file_path);
                    if (!handle_file_chunk_upload(g_device_id, file_path)) {
                        LOG_ERROR("Failed to handle file chunk upload");
                    }
                } else {
                    LOG_ERROR("Invalid file data message: empty payload");
                }
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_FILE_END: {
                // 处理文件传输结束
                LOG_INFO("File transfer ended");
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_FILE_COMPLETE: {
                // 处理文件传输完成
                LOG_DEBUG("Processing file complete message");
                // 构建文件上传完成主题：device/{device_id}/file/upload/response
                char *file_complete_topic = build_topic(g_device_id, "device/%s/file/upload/response");
                if (file_complete_topic) {
                    // 准备MQTT消息
                    mqtt_message_t mqtt_msg = {
                        .topic = file_complete_topic, // 消息主题
                        .payload = msg.payload.data,  // 消息内容
                        .payload_len = msg.data_len,  // 消息长度
                        .qos = MQTT_QOS_0,            // QoS级别0（最多一次）
                        .retain = false               // 非保留消息
                    };
                    
                    // 检查MQTT客户端是否有效
                    if (g_client) {
                        // 发布消息
                        int rc = mqtt_client_publish(g_client, &mqtt_msg);
                        if (rc != MQTT_ERR_SUCCESS) {
                            LOG_ERROR("Failed to publish file complete: %d", rc);
                        } else {
                            LOG_INFO("Published file complete to %s", file_complete_topic);
                        }
                    } else {
                        LOG_ERROR("MQTT client not initialized, cannot publish file complete");
                    }
                    // 释放主题内存
                    free(file_complete_topic);
                } else {
                    LOG_ERROR("Failed to build file complete topic");
                }
                break;
            }
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////            
            case MSG_TYPE_FOTA_COMPLETE: {
                // 处理FOTA升级完成
                LOG_DEBUG("Processing FOTA complete message");
                // 构建命令响应主题：device/{device_id}/command/response
                char *fota_complete_topic = build_topic(g_device_id, "device/%s/command/response");
                if (fota_complete_topic) {
                    // 准备MQTT消息
                    mqtt_message_t mqtt_msg = {
                        .topic = fota_complete_topic, // 消息主题
                        .payload = msg.payload.data,  // 消息内容
                        .payload_len = msg.data_len,  // 消息长度
                        .qos = MQTT_QOS_1,            // QoS级别1（至少一次）
                        .retain = false               // 非保留消息
                    };
                    
                    // 检查MQTT客户端是否有效
                    if (g_client) {
                        // 发布消息
                        int rc = mqtt_client_publish(g_client, &mqtt_msg);
                        if (rc != MQTT_ERR_SUCCESS) {
                            LOG_ERROR("Failed to publish FOTA complete: %d", rc);
                        } else {
                            LOG_INFO("Published FOTA complete to %s", fota_complete_topic);
                        }
                    } else {
                        LOG_ERROR("MQTT client not initialized, cannot publish FOTA complete");
                    }
                    // 释放主题内存
                    free(fota_complete_topic);
                } else {
                    LOG_ERROR("Failed to build FOTA complete topic");
                }
                break;
            }
            default:
                // 处理未知消息类型
                LOG_WARNING("Unknown message type: %d", msg.type);
                break;
        }
    } else if (ret < 0) { // 负数表示真正的错误，正数(1)表示超时/无消息
        // 消息队列错误处理
        LOG_ERROR("Message queue error: %d, %s", ret, strerror(errno));
        
        // 发布错误状态
        publish_device_status(DEVICE_STATUS_ERROR);
        
        // 尝试重新初始化消息队列
        LOG_INFO("Attempting to reinitialize message queues...");
        
        // 关闭现有队列
        if (g_mq_uart_to_mqtt != -1) {
            mq_close_queue(g_mq_uart_to_mqtt);
            g_mq_uart_to_mqtt = -1;
            LOG_DEBUG("Closed uart_to_mqtt queue");
        }
        
        if (g_mq_mqtt_to_uart != -1) {
            mq_close_queue(g_mq_mqtt_to_uart);
            g_mq_mqtt_to_uart = -1;
            LOG_DEBUG("Closed mqtt_to_uart queue");
        }
        
        // 重新初始化消息队列
        if (init_message_queues()) {
            LOG_INFO("Message queues reinitialized successfully");
            // 发布恢复状态
            publish_device_status(DEVICE_STATUS_ONLINE);
        } else {
            LOG_ERROR("Failed to reinitialize message queues");
            // 保持错误状态
        }
    }
    // ret == 1 表示超时（没有消息），这是正常情况，不需要处理
}

/**
 * @brief 主函数
 * 
 * 此函数是MQTT客户端示例程序的入口点，负责初始化和启动整个系统。
 * 
 * @param argc 命令行参数数量
 * @param argv 命令行参数数组
 * @return 成功返回EXIT_SUCCESS，失败返回EXIT_FAILURE
 * 
 * @note 程序运行流程：
 * 1. 初始化信号处理
 * 2. 初始化消息队列
 * 3. 初始化MQTT客户端
 * 4. 连接到MQTT服务器
 * 5. 订阅相关主题
 * 6. 进入主循环处理数据
 * 7. 接收信号后清理资源并退出
 */
int main(int argc, char *argv[])
{
    (void)argc;  // 未使用的参数
    (void)argv;  // 未使用的参数
    
    // 记录程序启动时间
    program_start_time = time(NULL);
    
    // 设置日志级别
    set_log_level(LOG_LEVEL_DEBUG);
    
    // 打印程序启动时间
    LOG_INFO("Program started at: %s", ctime(&program_start_time));
    
    // 打印程序信息
    LOG_INFO("MQTT Client Example for Aliyun MQTT Broker");
    LOG_INFO("===============================================");
    LOG_INFO("Debug mode enabled, detailed logs will be shown");
    LOG_INFO("Compiled for HiSilicon CV610 platform");
    LOG_INFO("Starting initialization sequence...");
    
    // 设置信号处理，用于优雅退出
    signal(SIGINT, signal_handler);   // 处理Ctrl+C信号
    signal(SIGTERM, signal_handler);  // 处理终止信号
    
    // 初始化消息队列，用于与UART进程通信
    LOG_INFO("Initializing message queues...");
    if (!init_message_queues()) {
        LOG_ERROR("Failed to initialize message queues");
        LOG_WARNING("Continuing without message queues, some functions may be limited");
        // 不直接返回失败，而是继续执行，让主循环处理错误
    } else {
        LOG_INFO("Message queues initialized successfully");
    }
    
    // 初始化MQTT客户端
    LOG_INFO("Initializing MQTT client...");
    if (!init_mqtt_client()) {
        LOG_ERROR("Failed to initialize MQTT client");
        LOG_WARNING("Continuing without MQTT client, will attempt to reinitialize in main loop");
        // 不直接返回失败，而是继续执行，让主循环处理错误
    } else {
        LOG_INFO("MQTT client initialized successfully");
    }
    
    // 连接到MQTT服务器
    LOG_INFO("Attempting to connect to MQTT broker...");
    LOG_INFO("Calling mqtt_client_connect...");
    int rc = mqtt_client_connect(g_client);
    LOG_INFO("mqtt_client_connect returned: %d (MQTT_ERR_SUCCESS=%d)", rc, MQTT_ERR_SUCCESS);
    
    if (rc != MQTT_ERR_SUCCESS) {
        LOG_ERROR("Failed to connect to MQTT broker: %d", rc);
        // 即使连接失败，也继续执行主循环，在主循环中尝试重连
        LOG_WARNING("Continuing with main loop despite connection failure, will attempt to reconnect");
    } else {
        LOG_INFO("MQTT connect initiated successfully, waiting for connection callback...");
    }
    
    // 确认连接函数返回后立即添加日志
    LOG_INFO("mqtt_client_connect function returned successfully, proceeding to main loop");
    LOG_INFO("Preparing main loop initialization...");
    
    // 主循环
    g_running = true;
    time_t last_reconnect_attempt = 0;
    const int RECONNECT_INTERVAL = 5; // 5秒
    const int CONNECTION_TIMEOUT = 30; // 30秒连接超时
    bool subscribed = false; // 标记是否已执行订阅操作
    bool published_initial_status = false; // 标记是否已发布初始状态
    time_t connection_start_time = time(NULL); // 连接开始时间
    bool connection_in_progress = false; // 标记是否有连接正在进行
    
    LOG_INFO("Initializing main loop variables...");
    int loop_count = 0;
    time_t main_loop_start_time = time(NULL);
    
    LOG_INFO("Entering main loop NOW - this should be visible in logs");
    LOG_INFO("Main loop start time: %s", ctime(&main_loop_start_time));
    LOG_INFO("Connection timeout set to %d seconds", CONNECTION_TIMEOUT);
    
    while (g_running) {
        loop_count++;
        time_t current_time = time(NULL);
        
        // 记录主循环执行日志（限制频率，避免日志过多）
        if (loop_count == 1) {
            LOG_INFO("Main loop started, uptime: %d seconds", (int)(current_time - program_start_time));
        } else if (loop_count % 200 == 0) {
            // 每200次迭代输出一次（约20秒）
            LOG_INFO("Main loop running, iteration #%d, uptime: %d seconds", loop_count, (int)(current_time - program_start_time));
        }
        
        // 检查主循环执行时间，确保没有阻塞
        if (current_time - main_loop_start_time > 30 && loop_count < 5) {
            LOG_ERROR("Main loop appears to be blocked - only %d iterations in %d seconds", loop_count, (int)(current_time - main_loop_start_time));
        }
        
        // 检查连接状态和连接超时
        mqtt_client_state_t state = MQTT_CLIENT_STATE_DISCONNECTED;
        if (g_client) {
            state = mqtt_client_get_state(g_client);
        }
        
        // 处理连接超时
        if (state == MQTT_CLIENT_STATE_CONNECTING) {
            if (!connection_in_progress) {
                connection_start_time = current_time;
                connection_in_progress = true;
                LOG_INFO("Connection started at: %s", ctime(&connection_start_time));
            } else {
                // 检查连接是否超时
                if (current_time - connection_start_time >= CONNECTION_TIMEOUT) {
                    LOG_ERROR("Connection timeout detected after %d seconds, resetting connection state", CONNECTION_TIMEOUT);
                    // 重置连接状态
                    connection_in_progress = false;
                    // 尝试重新连接
                    if (g_client) {
                        LOG_INFO("Attempting to reconnect due to connection timeout...");
                        int rc = mqtt_client_connect(g_client);
                        if (rc == MQTT_ERR_SUCCESS) {
                            LOG_INFO("Reconnect initiated successfully");
                            connection_start_time = current_time;
                            connection_in_progress = true;
                        } else {
                            LOG_ERROR("Reconnect failed: %d", rc);
                        }
                    }
                }
            }
        } else if (state == MQTT_CLIENT_STATE_CONNECTED) {
            // 连接成功，重置连接状态
            if (connection_in_progress) {
                LOG_INFO("Connection successful after %d seconds", (int)(current_time - connection_start_time));
                connection_in_progress = false;
            }
        } else if (state == MQTT_CLIENT_STATE_DISCONNECTED) {
            // 连接断开，重置连接状态
            if (connection_in_progress) {
                LOG_INFO("Connection disconnected after %d seconds", (int)(current_time - connection_start_time));
                connection_in_progress = false;
            }
        }
        
        // 处理MQTT消息
        int loop_rc = MQTT_ERR_SUCCESS;
        if (g_client) {
            loop_rc = mqtt_client_loop(g_client, 100); // 100ms超时，处理MQTT消息
            
            if (loop_rc != MQTT_ERR_SUCCESS) {
                LOG_ERROR("MQTT loop failed: %d", loop_rc);
                // 检查是否是连接错误
                if (loop_rc == MQTT_ERR_DISCONNECTED) {
                    LOG_WARNING("Client disconnected, will attempt to reconnect");
                } else if (loop_rc == MQTT_ERR_INVALID_PARAM) {
                    LOG_ERROR("Invalid parameter passed to mqtt_client_loop");
                } else if (loop_rc == MQTT_ERR_LOOP_FAILED) {
                    LOG_ERROR("MQTT loop internal error");
                } else {
                    LOG_ERROR("Unknown MQTT loop error: %d", loop_rc);
                }
            }
        } else {
            LOG_ERROR("MQTT client is NULL, cannot process messages");
        }
        
        // 检查连接状态（使用之前定义的state变量）
        mqtt_client_state_t prev_state = state;
        if (g_client) {
            state = mqtt_client_get_state(g_client);
        } else {
            state = MQTT_CLIENT_STATE_DISCONNECTED;
        }
        
        // 只在状态变化时输出日志
        if (state != prev_state) {
            const char* state_str = NULL;
            switch (state) {
                case MQTT_CLIENT_STATE_DISCONNECTED:
                    state_str = "DISCONNECTED";
                    break;
                case MQTT_CLIENT_STATE_CONNECTING:
                    state_str = "CONNECTING";
                    break;
                case MQTT_CLIENT_STATE_CONNECTED:
                    state_str = "CONNECTED";
                    break;
                case MQTT_CLIENT_STATE_DISCONNECTING:
                    state_str = "DISCONNECTING";
                    break;
                default:
                    state_str = "UNKNOWN";
                    break;
            }
            LOG_INFO("MQTT state changed to: %s (%d)", state_str, state);
        }
        
        // 当客户端进入CONNECTED状态时执行订阅和初始状态发布
        if (state == MQTT_CLIENT_STATE_CONNECTED && g_client) {
            // 执行订阅操作
            if (!subscribed) {
                LOG_INFO("Client connected, performing subscription...");
                
                // 订阅CV610升级命令主题（与服务器发布的主题格式匹配）
                // 主题格式：device/{device_id}/cv610/upgrade
                char *command_topic = build_topic(g_device_id, "device/%s/cv610/upgrade");
                if (command_topic) {
                    int sub_rc = mqtt_client_subscribe(g_client, command_topic, MQTT_QOS_1, on_message_received, NULL);
                    if (sub_rc != MQTT_ERR_SUCCESS) {
                        LOG_ERROR("Failed to subscribe to topic %s: %d", command_topic, sub_rc);
                    } else {
                        LOG_INFO("Subscribed to topic: %s", command_topic);
                    }
                    free(command_topic);
                }

                // 订阅升级响应主题
                // 主题格式：device/{device_id}/cv610/upgrade/response
                char *upgrade_response_topic = build_topic(g_device_id, "device/%s/cv610/upgrade/response");
                if (upgrade_response_topic) {
                    int sub_rc = mqtt_client_subscribe(g_client, upgrade_response_topic, MQTT_QOS_1, NULL, NULL);
                    if (sub_rc != MQTT_ERR_SUCCESS) {
                        LOG_ERROR("Failed to subscribe to topic %s: %d", upgrade_response_topic, sub_rc);
                    } else {
                        LOG_INFO("Subscribed to topic: %s", upgrade_response_topic);
                    }
                    free(upgrade_response_topic);
                }
                
                subscribed = true;
            }
            
            // 发布初始状态
            if (!published_initial_status) {
                LOG_INFO("Client connected, publishing initial online status...");
                publish_device_status(DEVICE_STATUS_ONLINE);
                published_initial_status = true;
            }
        } else {
            // 当客户端离开CONNECTED状态时重置标志
            subscribed = false;
            published_initial_status = false;
        }
        
        // 检查MQTT客户端是否有效
        if (!g_client) {
            LOG_ERROR("MQTT client is NULL, attempting to reinitialize...");
            if (init_mqtt_client()) {
                LOG_INFO("MQTT client reinitialized successfully");
            } else {
                LOG_ERROR("Failed to reinitialize MQTT client");
                usleep(1000000); // 1秒后重试
                continue;
            }
        }
        
        // 如果断开连接，尝试重新连接
        if (state == MQTT_CLIENT_STATE_DISCONNECTED) {
            time_t now = time(NULL);
            if (now - last_reconnect_attempt >= RECONNECT_INTERVAL) {
                LOG_INFO("MQTT connection lost, attempting to reconnect...");
                if (g_client) {
                    LOG_INFO("Calling mqtt_client_connect for reconnect...");
                    int rc = mqtt_client_connect(g_client);
                    LOG_INFO("mqtt_client_connect returned: %d", rc);
                    
                    if (rc == MQTT_ERR_SUCCESS) {
                        LOG_INFO("MQTT reconnect initiated successfully");
                        last_reconnect_attempt = now;
                        // 重置连接状态跟踪
                        connection_start_time = now;
                        connection_in_progress = true;
                    } else if (rc == MQTT_ERR_INVALID_PARAM) {
                        LOG_ERROR("Invalid parameter for mqtt_client_connect");
                        last_reconnect_attempt = now;
                    } else if (rc == MQTT_ERR_CONNECT_FAILED) {
                        LOG_ERROR("Connect failed, will retry in %d seconds", RECONNECT_INTERVAL);
                        last_reconnect_attempt = now;
                    } else if (rc == MQTT_ERR_INTERNAL) {
                        LOG_ERROR("Internal error during connect");
                        last_reconnect_attempt = now;
                    } else {
                        LOG_ERROR("Unknown connect error: %d", rc);
                        last_reconnect_attempt = now;
                    }
                } else {
                    LOG_ERROR("MQTT client is NULL, cannot reconnect");
                    // 尝试重新初始化客户端
                    LOG_INFO("Attempting to reinitialize MQTT client...");
                    if (init_mqtt_client()) {
                        LOG_INFO("MQTT client reinitialized successfully");
                        last_reconnect_attempt = now;
                    } else {
                        LOG_ERROR("Failed to reinitialize MQTT client");
                        last_reconnect_attempt = now;
                    }
                }
            } else {
                LOG_DEBUG("Reconnect interval not elapsed yet, waiting %d more seconds", (int)(RECONNECT_INTERVAL - (now - last_reconnect_attempt)));
            }
        } else if (state == MQTT_CLIENT_STATE_CONNECTED) {
            // 连接成功，重置重连时间
            static bool connected_logged = false;
            if (!connected_logged) {
                LOG_INFO("Client connected, resetting reconnect timer");
                connected_logged = true;
            }
            last_reconnect_attempt = time(NULL);
        } else {
            // 非连接状态，重置标志
            static bool connected_logged = false;
            connected_logged = false;
        }
        
        // 处理消息队列中的传感器数据
        handle_sensor_data();
        
        // 检查并发布设备状态（心跳）
        check_and_publish_status();
        
        // 短暂休眠，降低CPU占用
        usleep(100000); // 100ms
    }
    
    LOG_INFO("Exiting main loop");
    LOG_INFO("Main loop executed %d times, ran for %d seconds", loop_count, (int)(time(NULL) - program_start_time));
    
    // 设备关闭，发布offline状态
    LOG_INFO("Device shutting down, publishing offline status...");
    publish_device_status(DEVICE_STATUS_OFFLINE);
    
    // 清理资源
    // 断开MQTT连接
    mqtt_client_disconnect(g_client);
    
    // 销毁MQTT客户端
    mqtt_client_destroy(g_client);
    
    // 清理FOTA上下文
    if (g_fota_ctx) {
        fota_destroy(g_fota_ctx);
        g_fota_ctx = NULL;
    }
    
    // 关闭消息队列
    mq_close_queue(g_mq_uart_to_mqtt);
    mq_close_queue(g_mq_mqtt_to_uart);
    
    printf("MQTT Client stopped\n");
    
    return EXIT_SUCCESS;
}