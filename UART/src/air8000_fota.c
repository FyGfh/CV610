/**
 * @file air8000_fota.c
 * @brief Air8000 FOTA升级功能核心实现
 * @details 实现了 Air8000 FOTA升级功能的核心逻辑，包括FOTA升级的初始化、
 *          命令处理、状态管理等功能
 */

#include "air8000_fota.h"
#include "air8000_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>

// ==================== 常量定义 ====================

/**
 * @brief 默认数据包大小
 * @details 每次发送的固件数据包大小，单位字节
 */
#define DEFAULT_PACKET_SIZE  1024

/**
 * @brief 响应超时时间
 * @details 等待Air8000响应的超时时间，单位毫秒
 */
#define RESPONSE_TIMEOUT_MS  5000

/**
 * @brief 最大重试次数
 * @details 发送失败时的最大重试次数
 */
#define MAX_RETRY_COUNT      3

// ==================== 内部函数声明 ====================

/**
 * @brief 发送开始升级命令
 * @param fota_ctx FOTA升级上下文
 * @return 成功返回0，失败返回错误码
 */
static int send_ota_start(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 发送固件数据包
 * @param fota_ctx FOTA升级上下文
 * @return 成功返回0，失败返回错误码
 */
static int send_ota_data(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 发送升级完成命令
 * @param fota_ctx FOTA升级上下文
 * @return 成功返回0，失败返回错误码
 */
static int send_ota_finish(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 发送取消升级命令
 * @param fota_ctx FOTA升级上下文
 * @return 成功返回0，失败返回错误码
 */
static int send_ota_abort(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 更新FOTA状态
 * @param fota_ctx FOTA升级上下文
 * @param status 新的状态
 * @param error 错误码
 * @param progress 进度百分比
 */
static void update_fota_status(air8000_fota_ctx_t *fota_ctx, air8000_fota_status_t status, air8000_fota_error_t error, uint8_t progress);

/**
 * @brief 触发FOTA事件
 * @param fota_ctx FOTA升级上下文
 * @param event 事件类型
 * @param data 事件数据
 */
static void trigger_fota_event(air8000_fota_ctx_t *fota_ctx, air8000_fota_event_t event, void *data);

// ==================== API 函数实现 ====================

air8000_fota_ctx_t *air8000_fota_create(air8000_t *ctx, const char *firmware_path, air8000_fota_cb_t callback, void *user_data) {
    if (!ctx || !firmware_path) {
        log_error("fota", "无效的参数");
        return NULL;
    }

    // 检查固件文件是否存在
    struct stat file_stat;
    if (stat(firmware_path, &file_stat) != 0) {
        log_error("fota", "固件文件不存在: %s", firmware_path);
        return NULL;
    }

    // 分配FOTA升级上下文
    air8000_fota_ctx_t *fota_ctx = (air8000_fota_ctx_t *)malloc(sizeof(air8000_fota_ctx_t));
    if (!fota_ctx) {
        log_error("fota", "分配FOTA上下文失败");
        return NULL;
    }

    // 初始化上下文
    memset(fota_ctx, 0, sizeof(air8000_fota_ctx_t));
    fota_ctx->air8000_ctx = ctx;
    fota_ctx->status = FOTA_STATUS_IDLE;
    fota_ctx->firmware_size = file_stat.st_size;
    fota_ctx->current_seq = 0;
    fota_ctx->callback = callback;
    fota_ctx->user_data = user_data;
    strncpy(fota_ctx->firmware_path, firmware_path, sizeof(fota_ctx->firmware_path) - 1);
    fota_ctx->progress = 0;
    fota_ctx->aborted = false;
    
    // 初始化互斥锁
    if (pthread_mutex_init(&fota_ctx->mutex, NULL) != 0) {
        log_error("fota", "初始化互斥锁失败");
        free(fota_ctx);
        return NULL;
    }

    // 打开固件文件
    fota_ctx->firmware_file = fopen(firmware_path, "rb");
    if (!fota_ctx->firmware_file) {
        log_error("fota", "无法打开固件文件: %s", firmware_path);
        pthread_mutex_destroy(&fota_ctx->mutex);
        free(fota_ctx);
        return NULL;
    }

    log_info("fota", "FOTA上下文创建成功，固件大小: %u字节", fota_ctx->firmware_size);
    return fota_ctx;
}

void air8000_fota_destroy(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return;
    }

    pthread_mutex_lock(&fota_ctx->mutex);

    // 取消升级
    if (fota_ctx->status == FOTA_STATUS_RECEIVING || fota_ctx->status == FOTA_STATUS_VERIFYING) {
        send_ota_abort(fota_ctx);
    }

    // 关闭固件文件
    if (fota_ctx->firmware_file) {
        fclose(fota_ctx->firmware_file);
        fota_ctx->firmware_file = NULL;
    }

    // 销毁互斥锁
    pthread_mutex_unlock(&fota_ctx->mutex);
    pthread_mutex_destroy(&fota_ctx->mutex);

    // 释放内存
    free(fota_ctx);

    log_info("fota", "FOTA上下文已销毁");
}

int air8000_fota_start(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return AIR8000_ERR_PARAM;
    }

    pthread_mutex_lock(&fota_ctx->mutex);

    // 检查当前状态
    if (fota_ctx->status != FOTA_STATUS_IDLE) {
        log_error("fota", "FOTA已在运行中，当前状态: %d", fota_ctx->status);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return AIR8000_ERR_BUSY;
    }

    // 更新状态为接收中
    update_fota_status(fota_ctx, FOTA_STATUS_RECEIVING, FOTA_ERROR_NONE, 0);

    // 触发开始事件
    trigger_fota_event(fota_ctx, FOTA_EVENT_STARTED, NULL);

    // 发送开始升级命令
    int ret = send_ota_start(fota_ctx);
    if (ret != AIR8000_OK) {
        log_error("fota", "发送开始升级命令失败: %d", ret);
        update_fota_status(fota_ctx, FOTA_STATUS_FAILED, FOTA_ERROR_INIT_FAILED, 0);
        trigger_fota_event(fota_ctx, FOTA_EVENT_ERROR, &fota_ctx->error);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return ret;
    }

    // 发送固件数据
    while (fota_ctx->sent_size < fota_ctx->firmware_size && !fota_ctx->aborted) {
        ret = send_ota_data(fota_ctx);
        if (ret != AIR8000_OK) {
            log_error("fota", "发送固件数据失败: %d", ret);
            update_fota_status(fota_ctx, FOTA_STATUS_FAILED, FOTA_ERROR_WRITE_FAILED, fota_ctx->progress);
            trigger_fota_event(fota_ctx, FOTA_EVENT_ERROR, &fota_ctx->error);
            pthread_mutex_unlock(&fota_ctx->mutex);
            return ret;
        }

        // 更新进度
        uint8_t new_progress = (uint8_t)((fota_ctx->sent_size * 100) / fota_ctx->firmware_size);
        if (new_progress != fota_ctx->progress) {
            update_fota_status(fota_ctx, FOTA_STATUS_RECEIVING, FOTA_ERROR_NONE, new_progress);
            trigger_fota_event(fota_ctx, FOTA_EVENT_STATUS_UPDATED, &fota_ctx->progress);
        }
    }

    // 检查是否被取消
    if (fota_ctx->aborted) {
        log_info("fota", "FOTA升级已被取消");
        update_fota_status(fota_ctx, FOTA_STATUS_FAILED, FOTA_ERROR_ABORTED, fota_ctx->progress);
        trigger_fota_event(fota_ctx, FOTA_EVENT_ABORTED, NULL);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return AIR8000_OK;
    }

    // 发送升级完成命令
    ret = send_ota_finish(fota_ctx);
    if (ret != AIR8000_OK) {
        log_error("fota", "发送升级完成命令失败: %d", ret);
        update_fota_status(fota_ctx, FOTA_STATUS_FAILED, FOTA_ERROR_WRITE_FAILED, 100);
        trigger_fota_event(fota_ctx, FOTA_EVENT_ERROR, &fota_ctx->error);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return ret;
    }

    // 更新状态为成功
    update_fota_status(fota_ctx, FOTA_STATUS_SUCCESS, FOTA_ERROR_NONE, 100);
    trigger_fota_event(fota_ctx, FOTA_EVENT_COMPLETED, NULL);

    pthread_mutex_unlock(&fota_ctx->mutex);
    return AIR8000_OK;
}

int air8000_fota_abort(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return AIR8000_ERR_PARAM;
    }

    pthread_mutex_lock(&fota_ctx->mutex);

    // 检查当前状态
    if (fota_ctx->status == FOTA_STATUS_IDLE || fota_ctx->status == FOTA_STATUS_SUCCESS || fota_ctx->status == FOTA_STATUS_FAILED) {
        log_warn("fota", "FOTA未在运行中，当前状态: %d", fota_ctx->status);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return AIR8000_OK;
    }

    // 标记为已取消
    fota_ctx->aborted = true;

    // 发送取消升级命令
    int ret = send_ota_abort(fota_ctx);
    if (ret != AIR8000_OK) {
        log_error("fota", "发送取消升级命令失败: %d", ret);
        pthread_mutex_unlock(&fota_ctx->mutex);
        return ret;
    }

    pthread_mutex_unlock(&fota_ctx->mutex);
    return AIR8000_OK;
}

air8000_fota_status_t air8000_fota_get_status(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return FOTA_STATUS_IDLE;
    }

    pthread_mutex_lock(&fota_ctx->mutex);
    air8000_fota_status_t status = fota_ctx->status;
    pthread_mutex_unlock(&fota_ctx->mutex);

    return status;
}

int air8000_fota_handle_response(air8000_fota_ctx_t *fota_ctx, const air8000_frame_t *resp_frame) {
    if (!fota_ctx || !resp_frame) {
        return AIR8000_ERR_PARAM;
    }

    pthread_mutex_lock(&fota_ctx->mutex);

    // 检查状态
    if (fota_ctx->status == FOTA_STATUS_IDLE) {
        log_warn("fota", "FOTA未在运行中，忽略响应");
        pthread_mutex_unlock(&fota_ctx->mutex);
        return AIR8000_OK;
    }

    // 处理状态通知
    if (resp_frame->cmd == CMD_OTA_UART_STATUS) {
        if (resp_frame->data_len >= sizeof(air8000_fota_status_info_t)) {
            air8000_fota_status_info_t *status_info = (air8000_fota_status_info_t *)resp_frame->data;
            
            // 更新状态
            update_fota_status(fota_ctx, status_info->status, status_info->error, status_info->progress);
            
            // 触发状态更新事件
            trigger_fota_event(fota_ctx, FOTA_EVENT_STATUS_UPDATED, &fota_ctx->progress);
            
            // 检查是否出错
            if (status_info->status == FOTA_STATUS_FAILED) {
                log_error("fota", "Air8000 FOTA升级失败，错误码: %d", status_info->error);
                trigger_fota_event(fota_ctx, FOTA_EVENT_ERROR, &status_info->error);
                fota_ctx->aborted = true;
            }
        }
    }

    pthread_mutex_unlock(&fota_ctx->mutex);
    return AIR8000_OK;
}

void air8000_fota_register_callback(air8000_fota_ctx_t *fota_ctx, air8000_fota_cb_t cb, void *user_data) {
    if (!fota_ctx) {
        return;
    }

    pthread_mutex_lock(&fota_ctx->mutex);
    fota_ctx->callback = cb;
    fota_ctx->user_data = user_data;
    pthread_mutex_unlock(&fota_ctx->mutex);
}

// ==================== 内部函数实现 ====================

static int send_ota_start(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return AIR8000_ERR_PARAM;
    }

    // 构建开始升级命令
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    
    // 准备数据：固件大小（大端序）
    uint32_t firmware_size_be = htonl(fota_ctx->firmware_size);
    
    // 构建请求帧
    air8000_build_request(&frame, CMD_OTA_UART_START, (uint8_t *)&firmware_size_be, sizeof(firmware_size_be));
    
    // 发送请求
    int ret = air8000_send_and_wait(fota_ctx->air8000_ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

static int send_ota_data(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx || !fota_ctx->firmware_file) {
        return AIR8000_ERR_PARAM;
    }

    // 计算剩余数据大小
    uint32_t remaining_size = fota_ctx->firmware_size - fota_ctx->sent_size;
    uint32_t packet_size = remaining_size < DEFAULT_PACKET_SIZE ? remaining_size : DEFAULT_PACKET_SIZE;
    
    // 分配数据包内存
    uint8_t *packet_data = (uint8_t *)malloc(2 + packet_size);
    if (!packet_data) {
        log_error("fota", "分配数据包内存失败");
        return AIR8000_ERR_NOMEM;
    }
    
    // 设置序号（大端序）
    uint16_t seq_be = htons(fota_ctx->current_seq);
    memcpy(packet_data, &seq_be, 2);
    
    // 读取固件数据
    size_t read_len = fread(packet_data + 2, 1, packet_size, fota_ctx->firmware_file);
    if (read_len != packet_size) {
        log_error("fota", "读取固件数据失败: %d, 期望: %u, 实际: %zu", 
                  errno, packet_size, read_len);
        free(packet_data);
        return AIR8000_ERR_IO;
    }
    
    // 构建数据包命令
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    air8000_build_request(&frame, CMD_OTA_UART_DATA, packet_data, 2 + read_len);
    
    // 发送请求，带重试机制
    int ret = AIR8000_OK;
    int retry_count = 0;
    
    while (retry_count < MAX_RETRY_COUNT) {
        ret = air8000_send_and_wait(fota_ctx->air8000_ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
        if (ret == AIR8000_OK) {
            break;
        }
        
        retry_count++;
        log_warn("fota", "发送数据包失败，重试 %d/%d: %d", 
                 retry_count, MAX_RETRY_COUNT, ret);
        
        // 短暂延迟后重试
        usleep(100000); // 100ms
    }
    
    // 清理资源
    air8000_frame_cleanup(&frame);
    free(packet_data);
    
    if (ret == AIR8000_OK) {
        // 更新状态
        fota_ctx->sent_size += read_len;
        fota_ctx->current_seq++;
        
        // 触发数据发送事件
        trigger_fota_event(fota_ctx, FOTA_EVENT_DATA_SENT, &fota_ctx->sent_size);
    }
    
    return ret;
}

static int send_ota_finish(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return AIR8000_ERR_PARAM;
    }

    // 构建完成升级命令
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    air8000_build_request(&frame, CMD_OTA_UART_FINISH, NULL, 0);
    
    // 发送请求
    int ret = air8000_send_and_wait(fota_ctx->air8000_ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

static int send_ota_abort(air8000_fota_ctx_t *fota_ctx) {
    if (!fota_ctx) {
        return AIR8000_ERR_PARAM;
    }

    // 构建取消升级命令
    air8000_frame_t frame;
    air8000_frame_init(&frame);
    air8000_build_request(&frame, CMD_OTA_UART_ABORT, NULL, 0);
    
    // 发送请求
    int ret = air8000_send_and_wait(fota_ctx->air8000_ctx, &frame, NULL, RESPONSE_TIMEOUT_MS);
    
    // 清理帧
    air8000_frame_cleanup(&frame);
    
    return ret;
}

static void update_fota_status(air8000_fota_ctx_t *fota_ctx, air8000_fota_status_t status, air8000_fota_error_t error, uint8_t progress) {
    if (!fota_ctx) {
        return;
    }
    
    fota_ctx->status = status;
    fota_ctx->error = error;
    fota_ctx->progress = progress;
    
    log_info("fota", "FOTA状态更新: 状态=%d, 错误=%d, 进度=%d%%", 
             status, error, progress);
}

static void trigger_fota_event(air8000_fota_ctx_t *fota_ctx, air8000_fota_event_t event, void *data) {
    if (!fota_ctx || !fota_ctx->callback) {
        return;
    }
    
    fota_ctx->callback(fota_ctx->air8000_ctx, event, data, fota_ctx->user_data);
}

