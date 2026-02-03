/**
 * @file air8000_fota.h
 * @brief Air8000 FOTA升级功能 API 头文件
 * @details 定义了 Air8000 FOTA升级功能的 API 接口，包括FOTA升级的初始化、
 *          命令处理、回调注册等功能
 */

#ifndef AIR8000_FOTA_H
#define AIR8000_FOTA_H

#include "air8000.h"
#include "air8000_protocol.h"

#include <stdio.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 事件类型定义 ====================

/**
 * @brief FOTA升级事件类型
 * @details 用于回调函数中标识不同的FOTA升级事件
 */
typedef enum {
    FOTA_EVENT_STARTED,         ///< 升级开始
    FOTA_EVENT_DATA_SENT,       ///< 数据发送成功
    FOTA_EVENT_COMPLETED,       ///< 升级完成
    FOTA_EVENT_ERROR,           ///< 升级错误
    FOTA_EVENT_ABORTED,         ///< 升级取消
    FOTA_EVENT_STATUS_UPDATED   ///< 状态更新
} air8000_fota_event_t;

// ==================== 回调函数类型定义 ====================

/**
 * @brief FOTA升级回调函数类型
 * @param ctx Air8000上下文指针
 * @param event 事件类型
 * @param data 事件数据，根据event类型不同而不同
 * @param user_data 用户数据，注册回调时传入
 */
typedef void (*air8000_fota_cb_t)(air8000_t *ctx, 
                                  air8000_fota_event_t event, 
                                  void *data, 
                                  void *user_data);

// ==================== FOTA上下文结构体定义 ====================

/**
 * @brief FOTA升级上下文结构体
 * @details 用于管理FOTA升级过程中的状态和资源
 */
typedef struct air8000_fota_ctx {
    air8000_t *air8000_ctx;           ///< Air8000上下文指针
    air8000_fota_status_t status;       ///< FOTA升级状态
    air8000_fota_error_t error;         ///< FOTA升级错误码
    uint32_t firmware_size;             ///< 固件总大小
    uint32_t sent_size;                 ///< 已发送大小
    uint16_t current_seq;               ///< 当前发送的序号
    FILE *firmware_file;                ///< 固件文件指针
    air8000_fota_cb_t callback;         ///< 回调函数
    void *user_data;                    ///< 用户数据
    pthread_mutex_t mutex;              ///< 互斥锁
    char firmware_path[512];           ///< 固件文件路径
    uint8_t progress;                   ///< 进度百分比
    int timeout_fd;                    ///< 超时定时器文件描述符
    bool aborted;                      ///< 是否已取消
} air8000_fota_ctx_t;

// ==================== API 函数声明 ====================

/**
 * @brief 创建FOTA升级上下文
 * @param ctx Air8000上下文指针
 * @param firmware_path 固件文件路径
 * @param callback 回调函数
 * @param user_data 用户数据
 * @return 成功返回FOTA上下文指针，失败返回NULL
 */
air8000_fota_ctx_t *air8000_fota_create(air8000_t *ctx, const char *firmware_path, air8000_fota_cb_t callback, void *user_data);

/**
 * @brief 销毁FOTA升级上下文
 * @param fota_ctx FOTA上下文指针
 */
void air8000_fota_destroy(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 开始FOTA升级
 * @param fota_ctx FOTA上下文指针
 * @return 成功返回0，失败返回错误码
 */
int air8000_fota_start(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 取消FOTA升级
 * @param fota_ctx FOTA上下文指针
 * @return 成功返回0，失败返回错误码
 */
int air8000_fota_abort(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 获取当前FOTA升级状态
 * @param fota_ctx FOTA上下文指针
 * @return FOTA升级状态
 */
air8000_fota_status_t air8000_fota_get_status(air8000_fota_ctx_t *fota_ctx);

/**
 * @brief 处理FOTA响应
 * @param fota_ctx FOTA上下文指针
 * @param resp_frame 响应帧
 * @return 成功返回0，失败返回错误码
 */
int air8000_fota_handle_response(air8000_fota_ctx_t *fota_ctx, const air8000_frame_t *resp_frame);

/**
 * @brief 注册FOTA升级回调函数
 * @param fota_ctx FOTA上下文指针
 * @param cb 回调函数，处理FOTA升级事件
 * @param user_data 用户数据，回调时传递
 */
void air8000_fota_register_callback(air8000_fota_ctx_t *fota_ctx, air8000_fota_cb_t cb, void *user_data);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_FOTA_H
