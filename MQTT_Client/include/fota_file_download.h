#ifndef FOTA_FILE_DOWNLOAD_H
#define FOTA_FILE_DOWNLOAD_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

// ==================== 常量定义 ====================

/**
 * @brief 默认FOTA文件保存路径
 */
#define DEFAULT_FOTA_FILE_PATH "/appfs/nfs/AIR8000.bin"

/**
 * @brief 默认FOTA文件目录
 */
#define DEFAULT_FOTA_DIR "/appfs/nfs"

/**
 * @brief FOTA分片大小（16KB）
 */
#define FOTA_CHUNK_SIZE (16 * 1024)

/**
 * @brief FOTA操作最大重试次数
 */
#define FOTA_MAX_RETRY_COUNT 3

/**
 * @brief FOTA操作超时时间（毫秒）
 */
#define FOTA_OPERATION_TIMEOUT_MS 5000

// ==================== 类型定义 ====================

/**
 * @brief FOTA状态枚举
 */
typedef enum {
    FOTA_STATE_IDLE,          // 空闲状态
    FOTA_STATE_RECEIVING,     // 接收中
    FOTA_STATE_COMPLETE,      // 接收完成
    FOTA_STATE_FAILED,        // 失败
    FOTA_STATE_SAVING,        // 保存中
    FOTA_STATE_SAVED          // 已保存
} fota_state_t;

/**
 * @brief FOTA错误枚举
 */
typedef enum {
    FOTA_ERR_NONE,            // 无错误
    FOTA_ERR_NOMEM,           // 内存不足
    FOTA_ERR_FILE,            // 文件操作错误
    FOTA_ERR_DIR,             // 目录操作错误
    FOTA_ERR_DISK_SPACE,      // 磁盘空间不足
    FOTA_ERR_CHECKSUM,        // 校验和错误
    FOTA_ERR_TIMEOUT,         // 超时
    FOTA_ERR_OTHER            // 其他错误
} fota_error_t;

// 前向声明FOTA上下文结构
typedef struct fota_context_t fota_context_t;

/**
 * @brief FOTA回调函数类型
 */
typedef void (*fota_callback_t)(fota_context_t *ctx, fota_state_t state, fota_error_t error, void *user_data);

/**
 * @brief FOTA上下文结构
 */
typedef struct fota_context_t {
    char file_path[1024];      // FOTA文件保存路径
    char dir_path[1024];        // FOTA文件目录路径
    FILE *file_handle;          // 文件句柄
    uint64_t file_size;         // 文件总大小
    uint64_t received_size;     // 已接收大小
    uint32_t current_chunk;     // 当前分片编号
    uint32_t total_chunks;      // 总分片数
    fota_state_t state;         // 当前状态
    fota_error_t error;         // 错误码
    uint8_t progress;           // 进度百分比（0-100）
    bool aborted;               // 是否被取消
    char checksum[65];          // 文件校验和 (SHA256 string)
    fota_callback_t callback;   // 回调函数
    void *user_data;            // 用户数据
} fota_context_t;

// ==================== API 函数声明 ====================

/**
 * @brief 创建FOTA上下文
 * @param file_path FOTA文件保存路径（NULL使用默认路径）
 * @param dir_path FOTA文件目录路径（NULL使用默认路径）
 * @param callback 回调函数（可选）
 * @param user_data 用户数据（可选）
 * @return FOTA上下文指针，失败返回NULL
 */
fota_context_t *fota_create(const char *file_path, const char *dir_path, fota_callback_t callback, void *user_data);

/**
 * @brief 销毁FOTA上下文
 * @param ctx FOTA上下文指针
 */
void fota_destroy(fota_context_t *ctx);

/**
 * @brief 开始FOTA接收
 * @param ctx FOTA上下文指针
 * @param total_size 文件总大小
 * @param total_chunks 总分片数
 * @return 成功返回true，失败返回false
 */
bool fota_start(fota_context_t *ctx, uint64_t total_size, uint32_t total_chunks);

/**
 * @brief 处理FOTA分片数据
 * @param ctx FOTA上下文指针
 * @param chunk_id 分片编号
 * @param data 分片数据
 * @param data_len 分片数据长度
 * @return 成功返回true，失败返回false
 */
bool fota_process_chunk(fota_context_t *ctx, uint32_t chunk_id, const uint8_t *data, size_t data_len);

/**
 * @brief 结束FOTA接收
 * @param ctx FOTA上下文指针
 * @param checksum 文件校验和 (SHA256 string)
 * @return 成功返回true，失败返回false
 */
bool fota_finish(fota_context_t *ctx, const char *checksum);

/**
 * @brief 取消FOTA接收
 * @param ctx FOTA上下文指针
 * @return 成功返回true，失败返回false
 */
bool fota_abort(fota_context_t *ctx);

/**
 * @brief 获取FOTA状态
 * @param ctx FOTA上下文指针
 * @return FOTA状态
 */
fota_state_t fota_get_state(fota_context_t *ctx);

/**
 * @brief 获取FOTA进度
 * @param ctx FOTA上下文指针
 * @return 进度百分比（0-100）
 */
uint8_t fota_get_progress(fota_context_t *ctx);

/**
 * @brief 获取FOTA错误码
 * @param ctx FOTA上下文指针
 * @return FOTA错误码
 */
fota_error_t fota_get_error(fota_context_t *ctx);

/**
 * @brief 检查磁盘空间
 * @param path 检查路径
 * @param required_size 需要的空间（字节）
 * @return 空间足够返回true，不足返回false
 */
bool fota_check_disk_space(const char *path, uint64_t required_size);

/**
 * @brief 确保目录存在
 * @param dir_path 目录路径
 * @return 成功返回true，失败返回false
 */
bool fota_ensure_directory(const char *dir_path);

#endif // FOTA_FILE_DOWNLOAD_H
