/**
 * @file air8000_file_transfer.h
 * @brief Air8000 文件传输功能 API 头文件
 * @details 定义了 Air8000 文件传输功能的 API 接口，包括文件传输的初始化、
 *          发送、回调注册等功能
 */

#ifndef AIR8000_FILE_TRANSFER_H
#define AIR8000_FILE_TRANSFER_H

#include "air8000.h"
#include "air8000_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 常量定义 ====================

/**
 * @brief 默认分片大小（字节）
 */
#define AIR8000_FILE_DEFAULT_BLOCK_SIZE 1024

/**
 * @brief 最大重试次数
 */
#define AIR8000_FILE_MAX_RETRY_COUNT 5

/**
 * @brief 默认超时时间（毫秒）
 */
#define AIR8000_FILE_DEFAULT_TIMEOUT_MS 1000

// ==================== 事件类型定义 ====================

/**
 * @brief 文件传输事件类型
 * @details 用于回调函数中标识不同的文件传输事件
 */
typedef enum {
    FILE_TRANSFER_EVENT_NOTIFY_ACKED,    ///< 通知被CV610确认
    FILE_TRANSFER_EVENT_STARTED,         ///< 传输开始
    FILE_TRANSFER_EVENT_DATA_SENT,       ///< 分片发送成功
    FILE_TRANSFER_EVENT_COMPLETED,       ///< 传输完成
    FILE_TRANSFER_EVENT_ERROR,           ///< 传输错误
    FILE_TRANSFER_EVENT_CANCELLED,       ///< 传输取消
    FILE_TRANSFER_EVENT_REQUEST_RECEIVED ///< 收到CV610的传输请求
} air8000_file_transfer_event_t;

// ==================== 回调函数类型定义 ====================

/**
 * @brief 文件传输回调函数类型
 * @param ctx Air8000上下文指针
 * @param event 事件类型
 * @param data 事件数据，根据event类型不同而不同
 * @param user_data 用户数据，注册回调时传入
 */
typedef void (*air8000_file_transfer_cb_t)(air8000_t *ctx, 
                                            air8000_file_transfer_event_t event, 
                                            void *data, 
                                            void *user_data);

// ==================== API 函数声明 ====================

/**
 * @brief 初始化文件传输上下文
 * @param ctx Air8000上下文指针
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_init(air8000_t *ctx);

/**
 * @brief 销毁文件传输上下文
 */
void air8000_file_transfer_deinit(void);

/**
 * @brief 注册文件传输回调函数
 * @param cb 回调函数，处理文件传输事件
 * @param user_data 用户数据，回调时传递
 */
void air8000_file_transfer_register_callback(air8000_file_transfer_cb_t cb, 
                                             void *user_data);

/**
 * @brief Air8000主动通知CV610要发送文件
 * @param ctx Air8000上下文指针
 * @param filename 文件名
 * @param file_size 文件大小
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_notify(air8000_t *ctx, 
                                 const char *filename, 
                                 uint64_t file_size);

/**
 * @brief 开始发送文件
 * @param ctx Air8000上下文指针
 * @param filename 文件名
 * @param file_path 文件路径
 * @param block_size 分片大小，0表示使用默认值
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_start(air8000_t *ctx, 
                                const char *filename, 
                                const char *file_path, 
                                uint32_t block_size);

/**
 * @brief 取消文件传输
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_cancel(void);

/**
 * @brief 处理CV610发送的文件传输请求
 * @param ctx Air8000上下文指针
 * @param req_frame 请求帧
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_handle_request(air8000_t *ctx, 
                                         const air8000_frame_t *req_frame);

/**
 * @brief 获取当前文件传输状态
 * @return 文件传输状态
 */
air8000_file_transfer_state_t air8000_file_transfer_get_state(void);

/**
 * @brief 请求Air8000发送文件
 * @param ctx Air8000上下文指针
 * @param filename 文件名
 * @param save_path 保存路径
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_request(air8000_t *ctx, 
                                 const char *filename, 
                                 const char *save_path);

/**
 * @brief 处理Air8000发送的文件传输响应
 * @param ctx Air8000上下文指针
 * @param resp_frame 响应帧
 * @return 成功返回0，失败返回错误码
 */
int air8000_file_transfer_handle_response(air8000_t *ctx, 
                                         const air8000_frame_t *resp_frame);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_FILE_TRANSFER_H
