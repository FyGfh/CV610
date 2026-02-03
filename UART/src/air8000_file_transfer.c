/**
 * @file air8000_file_transfer.c
 * @brief Air8000 文件传输功能核心实现
 * @details 基于 FOTA 功能实现的文件传输功能，使用 FOTA 命令进行文件传输
 */

#include "air8000_file_transfer.h"
#include "air8000_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <arpa/inet.h>

// 实现 htobe64 函数，用于将 64 位主机字节序转换为大端字节序
static uint64_t htobe64(uint64_t host64) {
    union {
        uint64_t u64;
        uint8_t  u8[8];
    } host, be;

    host.u64 = host64;
    be.u8[0] = host.u8[7];
    be.u8[1] = host.u8[6];
    be.u8[2] = host.u8[5];
    be.u8[3] = host.u8[4];
    be.u8[4] = host.u8[3];
    be.u8[5] = host.u8[2];
    be.u8[6] = host.u8[1];
    be.u8[7] = host.u8[0];

    return be.u64;
}

// ==================== 常量定义 ====================

/**
 * @brief 默认数据包大小
 */
#define DEFAULT_PACKET_SIZE  1024

/**
 * @brief 响应超时时间
 */
#define RESPONSE_TIMEOUT_MS  5000

/**
 * @brief 最大重试次数
 */
#define MAX_RETRY_COUNT      3

// ==================== 内部函数声明 ====================

/**
 * @brief 计算CRC32校验和
 */
static uint32_t calculate_crc32(const uint8_t *data, size_t len);

/**
 * @brief 发送文件分片
 */
static int send_file_block(air8000_t *ctx, FILE *file, uint32_t block_index, uint32_t block_size);

/**
 * @brief 处理Air8000发送的文件传输开始命令
 */
static int handle_file_transfer_start(air8000_t *ctx, const air8000_frame_t *req_frame);

/**
 * @brief 处理Air8000发送的文件分片数据
 */
static int handle_file_transfer_data(air8000_t *ctx, const air8000_frame_t *req_frame);

/**
 * @brief 发送文件传输确认
 */
static int send_file_transfer_ack(air8000_t *ctx, uint32_t block_index, bool success);

/**
 * @brief 发送文件传输完成通知
 */
static int send_file_transfer_complete(air8000_t *ctx, bool success);

/**
 * @brief 清理接收文件资源
 */
static void cleanup_recv_file(void);

// ==================== 内部数据结构定义 ====================

/**
 * @brief 文件传输方向枚举
 */
typedef enum {
    FILE_TRANSFER_DIR_CV610_TO_AIR8000, ///< CV610向Air8000传输
    FILE_TRANSFER_DIR_AIR8000_TO_CV610  ///< Air8000向CV610传输
} file_transfer_direction_t;

/**
 * @brief 文件传输上下文结构体
 * @details 支持双向文件传输的上下文结构
 */
typedef struct {
    air8000_t *air8000_ctx;           ///< Air8000上下文指针
    air8000_file_transfer_state_t state; ///< 文件传输状态
    air8000_file_transfer_cb_t callback; ///< 回调函数
    void *user_data;                  ///< 用户数据
    char filename[256];               ///< 文件名
    uint64_t file_size;               ///< 文件大小
    pthread_mutex_t mutex;            ///< 互斥锁
    file_transfer_direction_t direction; ///< 文件传输方向
    uint32_t current_block;           ///< 当前分片索引
    uint32_t total_blocks;            ///< 总分片数
    uint32_t block_size;              ///< 分片大小
    FILE *recv_file;                  ///< 接收文件指针（用于Air8000→CV610）
    char recv_file_path[512];         ///< 接收文件路径
    FILE *send_file;                  ///< 发送文件指针（用于CV610→Air8000）
    char send_file_path[512];         ///< 发送文件路径
    uint32_t sent_blocks;             ///< 已发送的块数
} file_transfer_ctx_t;

// ==================== 全局变量 ====================

/**
 * @brief 文件传输上下文指针
 */
static file_transfer_ctx_t *g_file_transfer_ctx = NULL;

// ==================== 内部函数实现 ====================

/**
 * @brief 计算CRC32校验和
 */
static uint32_t calculate_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/**
 * @brief 发送文件分片
 */
static int send_file_block(air8000_t *ctx, FILE *file, uint32_t block_index, uint32_t block_size) {
    if (!ctx || !file) {
        return AIR8000_ERR_PARAM;
    }
    
    // 计算当前块的偏移量
    size_t offset = block_index * block_size;
    
    // 定位到文件偏移量
    if (fseek(file, offset, SEEK_SET) != 0) {
        return AIR8000_ERR_IO;
    }
    
    // 读取块数据
    uint8_t data[block_size];
    size_t read_len = fread(data, 1, block_size, file);
    if (read_len == 0) {
        return AIR8000_ERR_IO;
    }
    
    // 计算CRC32
    uint32_t crc32 = calculate_crc32(data, read_len);
    
    // 构建块数据
    uint8_t block_data[sizeof(uint32_t) * 3 + block_size];
    uint32_t *p_block_index = (uint32_t *)block_data;
    uint32_t *p_data_len = (uint32_t *)(block_data + sizeof(uint32_t));
    uint32_t *p_crc32 = (uint32_t *)(block_data + sizeof(uint32_t) * 2);
    uint8_t *p_data = block_data + sizeof(uint32_t) * 3;
    
    *p_block_index = htonl(block_index);
    *p_data_len = htonl(read_len);
    *p_crc32 = htonl(crc32);
    memcpy(p_data, data, read_len);
    
    // 构建请求帧
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    
    // 构建请求
    air8000_build_request(&frame, CMD_FILE_TRANSFER_DATA, block_data, sizeof(uint32_t) * 3 + read_len);
    
    // 发送请求
    int ret = air8000_send_and_wait(ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

/**
 * @brief 处理Air8000发送的文件传输开始命令
 */
static int handle_file_transfer_start(air8000_t *ctx, const air8000_frame_t *req_frame) {
    if (!ctx || !req_frame || !g_file_transfer_ctx) {
        return AIR8000_ERR_PARAM;
    }
    
    // 解析文件信息
    if (req_frame->data_len < sizeof(air8000_file_info_t)) {
        return AIR8000_ERR_PARAM;
    }
    
    air8000_file_info_t *file_info = (air8000_file_info_t *)req_frame->data;
    
    // 构建接收文件路径
    char recv_path[512] = {0};
    snprintf(recv_path, sizeof(recv_path), "/tmp/%s", file_info->filename);
    
    // 打开接收文件
    FILE *recv_file = fopen(recv_path, "wb");
    if (!recv_file) {
        log_error("file_transfer", "Failed to open receive file: %s", recv_path);
        return AIR8000_ERR_IO;
    }
    
    // 更新上下文
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 清理之前的资源
    cleanup_recv_file();
    
    // 更新文件信息
    strncpy(g_file_transfer_ctx->filename, file_info->filename, sizeof(g_file_transfer_ctx->filename) - 1);
    g_file_transfer_ctx->file_size = file_info->file_size;
    g_file_transfer_ctx->block_size = file_info->block_size;
    g_file_transfer_ctx->total_blocks = (file_info->file_size + file_info->block_size - 1) / file_info->block_size;
    g_file_transfer_ctx->current_block = 0;
    g_file_transfer_ctx->direction = FILE_TRANSFER_DIR_AIR8000_TO_CV610;
    g_file_transfer_ctx->state = FILE_TRANSFER_STARTED;
    g_file_transfer_ctx->recv_file = recv_file;
    strncpy(g_file_transfer_ctx->recv_file_path, recv_path, sizeof(g_file_transfer_ctx->recv_file_path) - 1);
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    // 触发开始事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_STARTED, file_info, g_file_transfer_ctx->user_data);
    }
    
    // 发送确认
    return send_file_transfer_ack(ctx, 0, true);
}

/**
 * @brief 处理Air8000发送的文件分片数据
 */
static int handle_file_transfer_data(air8000_t *ctx, const air8000_frame_t *req_frame) {
    if (!ctx || !req_frame || !g_file_transfer_ctx || !g_file_transfer_ctx->recv_file) {
        return AIR8000_ERR_PARAM;
    }
    
    // 解析分片数据
    if (req_frame->data_len < sizeof(air8000_file_block_t)) {
        return AIR8000_ERR_PARAM;
    }
    
    air8000_file_block_t *block = (air8000_file_block_t *)req_frame->data;
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 检查分片索引
    if (block->block_index != g_file_transfer_ctx->current_block) {
        log_error("file_transfer", "Block index mismatch: expected %u, got %u", 
                 g_file_transfer_ctx->current_block, block->block_index);
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return send_file_transfer_ack(ctx, block->block_index, false);
    }
    
    // 写入分片数据
    size_t written = fwrite(block->data, 1, block->data_len, g_file_transfer_ctx->recv_file);
    if (written != block->data_len) {
        log_error("file_transfer", "Failed to write block data: expected %u, wrote %zu", 
                 block->data_len, written);
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return send_file_transfer_ack(ctx, block->block_index, false);
    }
    
    // 更新当前分片索引
    g_file_transfer_ctx->current_block++;
    g_file_transfer_ctx->state = FILE_TRANSFER_TRANSMITTING;
    
    // 计算进度
    uint8_t progress = (g_file_transfer_ctx->current_block * 100) / g_file_transfer_ctx->total_blocks;
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    // 触发数据发送事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_DATA_SENT, &progress, g_file_transfer_ctx->user_data);
    }
    
    // 发送确认
    int ret = send_file_transfer_ack(ctx, block->block_index, true);
    
    // 检查是否传输完成
    if (g_file_transfer_ctx->current_block >= g_file_transfer_ctx->total_blocks) {
        // 关闭接收文件
        fclose(g_file_transfer_ctx->recv_file);
        g_file_transfer_ctx->recv_file = NULL;
        
        // 更新状态
        g_file_transfer_ctx->state = FILE_TRANSFER_COMPLETED;
        
        // 触发完成事件
        if (g_file_transfer_ctx->callback) {
            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_COMPLETED, NULL, g_file_transfer_ctx->user_data);
        }
        
        // 发送完成通知
        send_file_transfer_complete(ctx, true);
    }
    
    return ret;
}

/**
 * @brief 发送文件传输确认
 */
static int send_file_transfer_ack(air8000_t *ctx, uint32_t block_index, bool success) {
    if (!ctx) {
        return AIR8000_ERR_PARAM;
    }
    
    // 构建确认帧
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    
    // 准备确认数据（block_index + success）
    uint8_t ack_data[5] = {0};
    *((uint32_t *)ack_data) = htonl(block_index);
    ack_data[4] = success ? 1 : 0;
    
    // 构建请求
    air8000_build_request(&frame, CMD_FILE_TRANSFER_ACK, ack_data, sizeof(ack_data));
    
    // 发送请求
    int ret = air8000_send_and_wait(ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

/**
 * @brief 发送文件传输完成通知
 */
static int send_file_transfer_complete(air8000_t *ctx, bool success) {
    if (!ctx) {
        return AIR8000_ERR_PARAM;
    }
    
    // 构建完成通知帧
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    
    // 准备完成数据（success）
    uint8_t complete_data[1] = {success ? 1 : 0};
    
    // 构建请求
    air8000_build_request(&frame, CMD_FILE_TRANSFER_COMPLETE, complete_data, sizeof(complete_data));
    
    // 发送请求
    int ret = air8000_send_and_wait(ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

/**
 * @brief 清理接收文件资源
 */
static void cleanup_recv_file(void) {
    if (g_file_transfer_ctx->recv_file) {
        fclose(g_file_transfer_ctx->recv_file);
        g_file_transfer_ctx->recv_file = NULL;
    }
    
    if (strlen(g_file_transfer_ctx->recv_file_path) > 0) {
        unlink(g_file_transfer_ctx->recv_file_path);
        g_file_transfer_ctx->recv_file_path[0] = '\0';
    }
}

/**
 * @brief 清理发送文件资源
 */
static void cleanup_send_file(void) {
    if (g_file_transfer_ctx->send_file) {
        fclose(g_file_transfer_ctx->send_file);
        g_file_transfer_ctx->send_file = NULL;
    }
    
    g_file_transfer_ctx->send_file_path[0] = '\0';
}

// ==================== API 函数实现 ====================

/**
 * @brief 初始化文件传输模块
 * @param ctx AIR8000上下文指针
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_init(air8000_t *ctx) {
    if (!ctx) {
        return AIR8000_ERR_PARAM;
    }
    
    // 检查是否已经初始化
    if (g_file_transfer_ctx) {
        return AIR8000_ERR_BUSY;
    }
    
    // 分配文件传输上下文
    g_file_transfer_ctx = (file_transfer_ctx_t *)malloc(sizeof(file_transfer_ctx_t));
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_NOMEM;
    }
    
    // 初始化上下文
    memset(g_file_transfer_ctx, 0, sizeof(file_transfer_ctx_t));
    g_file_transfer_ctx->air8000_ctx = ctx;
    g_file_transfer_ctx->state = FILE_TRANSFER_IDLE;
    pthread_mutex_init(&g_file_transfer_ctx->mutex, NULL);
    
    return AIR8000_OK;
}

/**
 * @brief 释放文件传输模块资源
 */
void air8000_file_transfer_deinit(void) {
    if (!g_file_transfer_ctx) {
        return;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 清理接收文件资源
    cleanup_recv_file();
    
    // 清理发送文件资源
    cleanup_send_file();
    
    // 销毁互斥锁
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    pthread_mutex_destroy(&g_file_transfer_ctx->mutex);
    
    // 释放内存
    free(g_file_transfer_ctx);
    g_file_transfer_ctx = NULL;
    
    log_info("file_transfer", "文件传输模块已销毁");
}

/**
 * @brief 注册文件传输回调函数
 * @param cb 回调函数指针
 * @param user_data 用户数据指针
 */
void air8000_file_transfer_register_callback(air8000_file_transfer_cb_t cb, 
                                             void *user_data) {
    if (g_file_transfer_ctx) {
        pthread_mutex_lock(&g_file_transfer_ctx->mutex);
        g_file_transfer_ctx->callback = cb;
        g_file_transfer_ctx->user_data = user_data;
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    }
}

/**
 * @brief 发送文件通知帧
 * @param ctx AIR8000上下文指针
 * @param filename 文件名
 * @param file_size 文件大小
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_notify(air8000_t *ctx, 
                                 const char *filename, 
                                 uint64_t file_size) {
    if (!ctx || !filename) {
        return AIR8000_ERR_PARAM;
    }
    
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 检查当前状态
    if (g_file_transfer_ctx->state != FILE_TRANSFER_IDLE) {
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_BUSY;
    }
    
    // 更新文件信息
    strncpy(g_file_transfer_ctx->filename, filename, sizeof(g_file_transfer_ctx->filename) - 1);
    g_file_transfer_ctx->file_size = file_size;
    
    // 更新状态
    g_file_transfer_ctx->state = FILE_TRANSFER_NOTIFIED;
    
    // 触发通知事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_NOTIFY_ACKED, NULL, g_file_transfer_ctx->user_data);
    }
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    return AIR8000_OK;
}

/**
 * @brief 开始发送文件
 * @param ctx AIR8000上下文指针
 * @param filename 文件名
 * @param file_path 文件路径
 * @param block_size 分片大小（0表示默认）
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_start(air8000_t *ctx, 
                                const char *filename, 
                                const char *file_path, 
                                uint32_t block_size) {
    if (!ctx || !filename || !file_path) {
        return AIR8000_ERR_PARAM;
    }
    
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 检查当前状态
    if (g_file_transfer_ctx->state != FILE_TRANSFER_IDLE && 
        g_file_transfer_ctx->state != FILE_TRANSFER_NOTIFIED) {
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_BUSY;
    }
    
    // 清理之前的资源
    cleanup_send_file();
    
    // 打开要发送的文件
    FILE *send_file = fopen(file_path, "rb");
    if (!send_file) {
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_IO;
    }
    
    // 获取文件大小
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        fclose(send_file);
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_IO;
    }
    uint64_t file_size = file_stat.st_size;
    
    // 设置默认分片大小
    if (block_size == 0) {
        block_size = DEFAULT_PACKET_SIZE;
    }
    
    // 计算总分片数
    uint32_t total_blocks = (file_size + block_size - 1) / block_size;
    
    // 更新上下文
    strncpy(g_file_transfer_ctx->filename, filename, sizeof(g_file_transfer_ctx->filename) - 1);
    strncpy(g_file_transfer_ctx->send_file_path, file_path, sizeof(g_file_transfer_ctx->send_file_path) - 1);
    g_file_transfer_ctx->file_size = file_size;
    g_file_transfer_ctx->block_size = block_size;
    g_file_transfer_ctx->total_blocks = total_blocks;
    g_file_transfer_ctx->current_block = 0;
    g_file_transfer_ctx->sent_blocks = 0;
    g_file_transfer_ctx->direction = FILE_TRANSFER_DIR_CV610_TO_AIR8000;
    g_file_transfer_ctx->state = FILE_TRANSFER_STARTED;
    g_file_transfer_ctx->send_file = send_file;
    
    // 构建文件信息数据
    size_t filename_len = strlen(filename);
    size_t file_info_len = sizeof(uint32_t) + filename_len + sizeof(uint64_t) + sizeof(uint32_t) * 2;
    uint8_t *file_info_data = (uint8_t *)malloc(file_info_len);
    if (!file_info_data) {
        cleanup_send_file();
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_NOMEM;
    }
    
    // 填充文件信息数据
    uint32_t *p_filename_len = (uint32_t *)file_info_data;
    uint8_t *p_filename = file_info_data + sizeof(uint32_t);
    uint64_t *p_file_size = (uint64_t *)(p_filename + filename_len);
    uint32_t *p_block_size = (uint32_t *)(p_file_size + 1);
    uint32_t *p_crc32 = (uint32_t *)(p_block_size + 1);
    
    *p_filename_len = htonl(filename_len);
    memcpy(p_filename, filename, filename_len);
    *p_file_size = htobe64(file_size);
    *p_block_size = htonl(block_size);
    *p_crc32 = htonl(0); // 简化处理，实际应计算整个文件的CRC32
    
    // 构建请求帧
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    air8000_build_request(&frame, CMD_FILE_TRANSFER_START, file_info_data, file_info_len);
    
    // 发送文件传输开始命令
    int ret = air8000_send_and_wait(ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧和内存
    air8000_frame_cleanup(&frame);
    free(file_info_data);
    
    if (ret != AIR8000_OK) {
        log_error("file_transfer", "发送文件传输开始命令失败: %d", ret);
        cleanup_send_file();
        g_file_transfer_ctx->state = FILE_TRANSFER_ERROR;
        
        if (g_file_transfer_ctx->callback) {
            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_ERROR, &ret, g_file_transfer_ctx->user_data);
        }
        
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return ret;
    }
    
    // 触发开始事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_STARTED, NULL, g_file_transfer_ctx->user_data);
    }
    
    // 开始发送文件分片
    g_file_transfer_ctx->state = FILE_TRANSFER_TRANSMITTING;
    
    while (g_file_transfer_ctx->current_block < g_file_transfer_ctx->total_blocks) {
        // 发送文件分片
        ret = send_file_block(ctx, g_file_transfer_ctx->send_file, 
                             g_file_transfer_ctx->current_block, 
                             g_file_transfer_ctx->block_size);
        
        if (ret != AIR8000_OK) {
            log_error("file_transfer", "发送文件分片失败: %d", ret);
            cleanup_send_file();
            g_file_transfer_ctx->state = FILE_TRANSFER_ERROR;
            
            if (g_file_transfer_ctx->callback) {
                g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_ERROR, &ret, g_file_transfer_ctx->user_data);
            }
            
            pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
            return ret;
        }
        
        // 更新状态
        g_file_transfer_ctx->current_block++;
        g_file_transfer_ctx->sent_blocks++;
        
        // 计算进度
        uint8_t progress = (g_file_transfer_ctx->sent_blocks * 100) / g_file_transfer_ctx->total_blocks;
        
        // 触发数据发送事件
        if (g_file_transfer_ctx->callback) {
            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_DATA_SENT, &progress, g_file_transfer_ctx->user_data);
        }
        
        // 短暂延迟，避免发送过快
        usleep(10000); // 10ms
    }
    
    // 发送传输完成通知
    ret = send_file_transfer_complete(ctx, true);
    
    // 清理资源
    cleanup_send_file();
    g_file_transfer_ctx->state = FILE_TRANSFER_COMPLETED;
    
    // 触发完成事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_COMPLETED, NULL, g_file_transfer_ctx->user_data);
    }
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    return ret;
}

/**
 * @brief 取消文件传输
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_cancel(void) {
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 检查当前状态
    if (g_file_transfer_ctx->state == FILE_TRANSFER_IDLE || 
        g_file_transfer_ctx->state == FILE_TRANSFER_COMPLETED || 
        g_file_transfer_ctx->state == FILE_TRANSFER_ERROR) {
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_OK;
    }
    
    // 清理资源
    cleanup_send_file();
    cleanup_recv_file();
    
    // 更新状态
    g_file_transfer_ctx->state = FILE_TRANSFER_CANCELLED;
    
    // 触发取消事件
    if (g_file_transfer_ctx->callback) {
        g_file_transfer_ctx->callback(g_file_transfer_ctx->air8000_ctx, 
                                     FILE_TRANSFER_EVENT_CANCELLED, NULL, 
                                     g_file_transfer_ctx->user_data);
    }
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    return AIR8000_OK;
}

/**
 * @brief 处理文件传输请求
 * @param ctx 设备上下文
 * @param req_frame 请求帧
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_handle_request(air8000_t *ctx, const air8000_frame_t *req_frame) {
    if (!ctx || !req_frame) {
        return AIR8000_ERR_PARAM;
    }
    
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    if (req_frame->cmd == CMD_FILE_TRANSFER_REQUEST) {
        // 处理文件传输请求（CV610请求Air8000发送文件）
        if (req_frame->data_len > 0 && req_frame->data) {
            // 触发请求接收事件
            if (g_file_transfer_ctx->callback) {
                g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_REQUEST_RECEIVED, 
                                           (void *)req_frame->data, g_file_transfer_ctx->user_data);
            }
        }
    } else if (req_frame->cmd == CMD_FILE_TRANSFER_START) {
        // 处理Air8000发送的文件传输开始命令（Air8000→CV610方向）
        handle_file_transfer_start(ctx, req_frame);
    } else if (req_frame->cmd == CMD_FILE_TRANSFER_DATA) {
        // 处理Air8000发送的文件分片数据（Air8000→CV610方向）
        handle_file_transfer_data(ctx, req_frame);
    } else if (req_frame->cmd == CMD_FILE_TRANSFER_ERROR) {
        // 处理Air8000发送的传输错误通知
        if (g_file_transfer_ctx->callback) {
            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_ERROR, NULL, g_file_transfer_ctx->user_data);
        }
    } else if (req_frame->cmd == CMD_FILE_TRANSFER_CANCEL) {
        // 处理Air8000发送的取消传输通知
        if (g_file_transfer_ctx->callback) {
            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_CANCELLED, NULL, g_file_transfer_ctx->user_data);
        }
    }
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    return AIR8000_OK;
}

/**
 * @brief 获取文件传输状态
 * @return 文件传输状态
 */
air8000_file_transfer_state_t air8000_file_transfer_get_state(void) {
    if (!g_file_transfer_ctx) {
        return FILE_TRANSFER_IDLE;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    air8000_file_transfer_state_t state = g_file_transfer_ctx->state;
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    return state;
}

/**
 * @brief 请求Air8000发送文件
 * @param ctx Air8000上下文指针
 * @param filename 文件名
 * @param save_path 保存路径
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_request(air8000_t *ctx, 
                                 const char *filename, 
                                 const char *save_path) {
    if (!ctx || !filename || !save_path) {
        return AIR8000_ERR_PARAM;
    }
    
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    pthread_mutex_lock(&g_file_transfer_ctx->mutex);
    
    // 检查当前状态
    if (g_file_transfer_ctx->state != FILE_TRANSFER_IDLE) {
        pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
        return AIR8000_ERR_BUSY;
    }
    
    // 更新文件信息
    strncpy(g_file_transfer_ctx->filename, filename, sizeof(g_file_transfer_ctx->filename) - 1);
    strncpy(g_file_transfer_ctx->recv_file_path, save_path, sizeof(g_file_transfer_ctx->recv_file_path) - 1);
    g_file_transfer_ctx->direction = FILE_TRANSFER_DIR_AIR8000_TO_CV610;
    g_file_transfer_ctx->state = FILE_TRANSFER_NOTIFIED;
    
    pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
    
    // 构建请求帧
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    
    // 准备请求数据（文件名）
    size_t filename_len = strlen(filename);
    
    // 构建请求
    air8000_build_request(&frame, CMD_FILE_TRANSFER_REQUEST, (uint8_t *)filename, filename_len);
    
    // 发送请求
    int ret = air8000_send_and_wait(ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

/**
 * @brief 处理Air8000发送的文件传输响应
 * @param ctx Air8000上下文指针
 * @param resp_frame 响应帧
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_handle_response(air8000_t *ctx, 
                                         const air8000_frame_t *resp_frame) {
    if (!ctx || !resp_frame) {
        return AIR8000_ERR_PARAM;
    }
    
    if (!g_file_transfer_ctx) {
        return AIR8000_ERR_GENERIC;
    }
    
    // 处理不同类型的响应
    switch (resp_frame->cmd) {
        case CMD_FILE_TRANSFER_START:
            return handle_file_transfer_start(ctx, resp_frame);
        case CMD_FILE_TRANSFER_DATA:
            return handle_file_transfer_data(ctx, resp_frame);
        case CMD_FILE_TRANSFER_ERROR:
            // 处理错误响应
            if (g_file_transfer_ctx->callback) {
                g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_ERROR, NULL, g_file_transfer_ctx->user_data);
            }
            g_file_transfer_ctx->state = FILE_TRANSFER_ERROR;
            break;
        case CMD_FILE_TRANSFER_CANCEL:
            // 处理取消响应
            if (g_file_transfer_ctx->callback) {
                g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_CANCELLED, NULL, g_file_transfer_ctx->user_data);
            }
            g_file_transfer_ctx->state = FILE_TRANSFER_CANCELLED;
            break;
        case CMD_FILE_TRANSFER_STATUS:
            // 处理文件传输状态通知
            if (resp_frame->data_len >= 3) {
                uint8_t status = resp_frame->data[0];
                uint8_t error_code = resp_frame->data[1];
                uint8_t progress = resp_frame->data[2];
                
                log_info("file_transfer", "收到状态通知: status=%d, error=%d, progress=%d%%", 
                        status, error_code, progress);
                
                pthread_mutex_lock(&g_file_transfer_ctx->mutex);
                
                // 更新状态
                switch (status) {
                    case 0: // IDLE
                        g_file_transfer_ctx->state = FILE_TRANSFER_IDLE;
                        break;
                    case 2: // STARTED
                        g_file_transfer_ctx->state = FILE_TRANSFER_STARTED;
                        if (g_file_transfer_ctx->callback) {
                            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_STARTED, NULL, g_file_transfer_ctx->user_data);
                        }
                        break;
                    case 3: // TRANSMITTING
                        g_file_transfer_ctx->state = FILE_TRANSFER_TRANSMITTING;
                        if (g_file_transfer_ctx->callback) {
                            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_DATA_SENT, &progress, g_file_transfer_ctx->user_data);
                        }
                        break;
                    case 4: // COMPLETED
                        g_file_transfer_ctx->state = FILE_TRANSFER_COMPLETED;
                        if (g_file_transfer_ctx->callback) {
                            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_COMPLETED, NULL, g_file_transfer_ctx->user_data);
                        }
                        break;
                    case 5: // ERROR
                        g_file_transfer_ctx->state = FILE_TRANSFER_ERROR;
                        if (g_file_transfer_ctx->callback) {
                            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_ERROR, &error_code, g_file_transfer_ctx->user_data);
                        }
                        break;
                    case 6: // CANCELLED
                        g_file_transfer_ctx->state = FILE_TRANSFER_CANCELLED;
                        if (g_file_transfer_ctx->callback) {
                            g_file_transfer_ctx->callback(ctx, FILE_TRANSFER_EVENT_CANCELLED, NULL, g_file_transfer_ctx->user_data);
                        }
                        break;
                    default:
                        break;
                }
                
                pthread_mutex_unlock(&g_file_transfer_ctx->mutex);
            }
            break;
        default:
            break;
    }
    
    return AIR8000_OK;
}
