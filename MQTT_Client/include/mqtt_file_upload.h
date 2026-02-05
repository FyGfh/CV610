#ifndef MQTT_FILE_UPLOAD_H
#define MQTT_FILE_UPLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

// ==================== 常量定义 ====================

/**
 * @brief 文件上传分片大小（16KB）
 */
#define FILE_UPLOAD_CHUNK_SIZE (16 * 1024)

/**
 * @brief 文件上传操作最大重试次数
 */
#define FILE_UPLOAD_MAX_RETRY_COUNT 3

/**
 * @brief 文件上传操作超时时间（毫秒）
 */
#define FILE_UPLOAD_OPERATION_TIMEOUT_MS 5000

// ==================== 类型定义 ====================

/**
 * @brief 文件上传状态枚举
 */
typedef enum {
    FILE_UPLOAD_STATE_IDLE,          // 空闲状态
    FILE_UPLOAD_STATE_UPLOADING,     // 上传中
    FILE_UPLOAD_STATE_COMPLETE,      // 上传完成
    FILE_UPLOAD_STATE_FAILED         // 失败
} file_upload_state_t;

/**
 * @brief 文件上传错误枚举
 */
typedef enum {
    FILE_UPLOAD_ERR_NONE,            // 无错误
    FILE_UPLOAD_ERR_NOMEM,           // 内存不足
    FILE_UPLOAD_ERR_FILE,            // 文件操作错误
    FILE_UPLOAD_ERR_TIMEOUT,         // 超时
    FILE_UPLOAD_ERR_OTHER            // 其他错误
} file_upload_error_t;

/**
 * @brief 文件分片上传上下文结构
 */
typedef struct {
    char file_path[1024];      // 待上传文件路径
    char file_id[64];          // 文件唯一标识
    char filename[256];        // 文件名
    FILE *file_handle;          // 文件句柄
    uint64_t file_size;         // 文件总大小
    uint64_t uploaded_size;     // 已上传大小
    uint32_t current_chunk;     // 当前分片编号
    uint32_t total_chunks;      // 总分片数
    uint32_t chunk_size;        // 分片大小
    file_upload_state_t state;  // 当前状态
    file_upload_error_t error;  // 错误码
    uint8_t progress;           // 进度百分比（0-100）
    bool aborted;               // 是否被取消
} file_upload_context_t;

// ==================== API 函数声明 ====================

/**
 * @brief 创建文件分片上传上下文
 * @param file_path 待上传文件路径
 * @param chunk_size 分片大小（默认16KB）
 * @return 上传上下文指针，失败返回NULL
 */
file_upload_context_t *file_upload_create(const char *file_path, uint32_t chunk_size);

/**
 * @brief 销毁文件分片上传上下文
 * @param ctx 上传上下文指针
 */
void file_upload_destroy(file_upload_context_t *ctx);

/**
 * @brief 开始文件分片上传
 * @param ctx 上传上下文指针
 * @return 成功返回true，失败返回false
 */
bool file_upload_start(file_upload_context_t *ctx);

/**
 * @brief 获取下一个文件分片
 * @param ctx 上传上下文指针
 * @param chunk_data 输出参数，用于存储分片数据
 * @param chunk_data_len 输出参数，用于存储分片数据长度
 * @param chunk_id 输出参数，用于存储分片编号
 * @return 成功返回true，失败返回false
 */
bool file_upload_get_next_chunk(file_upload_context_t *ctx, uint8_t **chunk_data, size_t *chunk_data_len, uint32_t *chunk_id);

/**
 * @brief 完成文件分片上传
 * @param ctx 上传上下文指针
 * @return 成功返回true，失败返回false
 */
bool file_upload_finish(file_upload_context_t *ctx);

/**
 * @brief 取消文件分片上传
 * @param ctx 上传上下文指针
 * @return 成功返回true，失败返回false
 */
bool file_upload_abort(file_upload_context_t *ctx);

/**
 * @brief 获取文件上传状态
 * @param ctx 上传上下文指针
 * @return 上传状态
 */
file_upload_state_t file_upload_get_state(file_upload_context_t *ctx);

/**
 * @brief 获取文件上传进度
 * @param ctx 上传上下文指针
 * @return 进度百分比（0-100）
 */
uint8_t file_upload_get_progress(file_upload_context_t *ctx);

/**
 * @brief 获取文件上传错误码
 * @param ctx 上传上下文指针
 * @return 错误码
 */
file_upload_error_t file_upload_get_error(file_upload_context_t *ctx);

/**
 * @brief 获取文件总大小
 * @param ctx 上传上下文指针
 * @return 文件总大小
 */
uint64_t file_upload_get_file_size(file_upload_context_t *ctx);

/**
 * @brief 获取总分片数
 * @param ctx 上传上下文指针
 * @return 总分片数
 */
uint32_t file_upload_get_total_chunks(file_upload_context_t *ctx);

#endif // MQTT_FILE_UPLOAD_H
