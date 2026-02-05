#include "fota_file_download.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <openssl/evp.h>

// ==================== 内部函数声明 ====================

/**
 * @brief 更新FOTA状态
 */
static void update_fota_state(fota_context_t *ctx, fota_state_t state, fota_error_t error);

/**
 * @brief 触发FOTA回调
 */
static void trigger_fota_callback(fota_context_t *ctx, fota_state_t state, fota_error_t error);

/**
 * @brief 验证文件校验和
 */
static bool verify_file_checksum(const char *file_path, const char *expected_checksum);

/**
 * @brief 尝试创建目录（带重试）
 */
static bool try_create_directory(const char *dir_path, int max_retries);

/**
 * @brief 尝试打开文件（带重试）
 */
static FILE *try_open_file(const char *file_path, const char *mode, int max_retries);

// ==================== API 函数实现 ====================

fota_context_t *fota_create(const char *file_path, const char *dir_path, fota_callback_t callback, void *user_data) {
    // 分配FOTA上下文
    fota_context_t *ctx = (fota_context_t *)malloc(sizeof(fota_context_t));
    if (!ctx) {
        return NULL;
    }

    // 初始化上下文
    memset(ctx, 0, sizeof(fota_context_t));
    
    // 设置文件路径
    if (file_path) {
        strncpy(ctx->file_path, file_path, sizeof(ctx->file_path) - 1);
    } else {
        strncpy(ctx->file_path, DEFAULT_FOTA_FILE_PATH, sizeof(ctx->file_path) - 1);
    }

    // 设置目录路径
    if (dir_path) {
        strncpy(ctx->dir_path, dir_path, sizeof(ctx->dir_path) - 1);
    } else {
        strncpy(ctx->dir_path, DEFAULT_FOTA_DIR, sizeof(ctx->dir_path) - 1);
    }

    // 设置回调函数
    ctx->callback = callback;
    ctx->user_data = user_data;
    
    // 初始化状态
    ctx->state = FOTA_STATE_IDLE;
    ctx->error = FOTA_ERR_NONE;
    ctx->file_handle = NULL;
    ctx->file_size = 0;
    ctx->received_size = 0;
    ctx->current_chunk = 0;
    ctx->total_chunks = 0;
    ctx->progress = 0;
    ctx->aborted = false;
    memset(ctx->checksum, 0, sizeof(ctx->checksum));

    return ctx;
}

void fota_destroy(fota_context_t *ctx) {
    if (!ctx) {
        return;
    }

    // 关闭文件
    if (ctx->file_handle) {
        fclose(ctx->file_handle);
        ctx->file_handle = NULL;
    }

    // 释放内存
    free(ctx);
}

bool fota_start(fota_context_t *ctx, uint64_t total_size, uint32_t total_chunks) {
    if (!ctx) {
        return false;
    }

    // 检查状态
    if (ctx->state != FOTA_STATE_IDLE) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_OTHER);
        return false;
    }

    // 检查磁盘空间
    if (!fota_check_disk_space(ctx->dir_path, total_size)) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_DISK_SPACE);
        return false;
    }

    // 确保目录存在
    if (!fota_ensure_directory(ctx->dir_path)) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_DIR);
        return false;
    }

    // 尝试打开文件
    ctx->file_handle = try_open_file(ctx->file_path, "wb", FOTA_MAX_RETRY_COUNT);
    if (!ctx->file_handle) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
        return false;
    }

    // 更新状态
    ctx->file_size = total_size;
    ctx->total_chunks = total_chunks;
    ctx->received_size = 0;
    ctx->current_chunk = 0;
    ctx->progress = 0;
    ctx->aborted = false;
    update_fota_state(ctx, FOTA_STATE_RECEIVING, FOTA_ERR_NONE);

    return true;
}

bool fota_process_chunk(fota_context_t *ctx, uint32_t chunk_id, const uint8_t *data, size_t data_len) {
    if (!ctx || !data || data_len == 0) {
        return false;
    }

    // 检查状态
    if (ctx->state != FOTA_STATE_RECEIVING) {
        return false;
    }

    // 检查分片编号
    if (chunk_id != ctx->current_chunk) {
        // 分片乱序，这里可以实现重排序逻辑
        // 暂时返回失败
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_OTHER);
        return false;
    }

    // 写入文件（带重试）
    size_t written = 0;
    int retry_count = 0;
    
    while (retry_count < FOTA_MAX_RETRY_COUNT) {
        written = fwrite(data, 1, data_len, ctx->file_handle);
        if (written == data_len) {
            break;
        }
        
        // 检查错误类型
        if (errno == EIO || errno == ENOSPC) {
            // 设备I/O错误或磁盘空间不足
            retry_count++;
            // 短暂延迟后重试
            usleep(100000); // 100ms
        } else {
            // 其他错误，直接返回失败
            update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
            return false;
        }
    }
    
    if (written != data_len) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
        return false;
    }

    // 刷新文件缓冲区（带重试）
    retry_count = 0;
    while (retry_count < FOTA_MAX_RETRY_COUNT) {
        if (fflush(ctx->file_handle) == 0) {
            break;
        }
        
        // 检查错误类型
        if (errno == EIO) {
            // 设备I/O错误
            retry_count++;
            // 短暂延迟后重试
            usleep(100000); // 100ms
        } else {
            // 其他错误，直接返回失败
            update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
            return false;
        }
    }
    
    if (retry_count >= FOTA_MAX_RETRY_COUNT) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
        return false;
    }

    // 更新状态
    ctx->received_size += data_len;
    ctx->current_chunk++;
    
    // 计算进度
    if (ctx->file_size > 0) {
        ctx->progress = (uint8_t)((ctx->received_size * 100) / ctx->file_size);
    }

    // 检查是否接收完成
    if (ctx->current_chunk >= ctx->total_chunks) {
        update_fota_state(ctx, FOTA_STATE_COMPLETE, FOTA_ERR_NONE);
    }

    return true;
}

bool fota_finish(fota_context_t *ctx, const char *checksum) {
    if (!ctx) {
        return false;
    }

    // 检查状态
    if (ctx->state != FOTA_STATE_COMPLETE) {
        return false;
    }

    // 关闭文件
    if (ctx->file_handle) {
        if (fclose(ctx->file_handle) != 0) {
            update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
            ctx->file_handle = NULL;
            return false;
        }
        ctx->file_handle = NULL;
    }

    // 验证文件大小
    struct stat file_stat;
    if (stat(ctx->file_path, &file_stat) != 0) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
        return false;
    }

    if ((uint64_t)file_stat.st_size != ctx->file_size) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_FILE);
        return false;
    }

    // 验证文件校验和
    if (!verify_file_checksum(ctx->file_path, checksum)) {
        update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_CHECKSUM);
        // 删除校验失败的文件
        unlink(ctx->file_path);
        return false;
    }

    strncpy(ctx->checksum, checksum, sizeof(ctx->checksum) - 1);

    // 更新状态
    update_fota_state(ctx, FOTA_STATE_SAVED, FOTA_ERR_NONE);

    return true;
}

bool fota_abort(fota_context_t *ctx) {
    if (!ctx) {
        return false;
    }

    // 标记为已取消
    ctx->aborted = true;

    // 关闭文件
    if (ctx->file_handle) {
        fclose(ctx->file_handle);
        ctx->file_handle = NULL;
    }

    // 删除部分文件
    unlink(ctx->file_path);

    // 更新状态
    update_fota_state(ctx, FOTA_STATE_FAILED, FOTA_ERR_OTHER);

    return true;
}

fota_state_t fota_get_state(fota_context_t *ctx) {
    if (!ctx) {
        return FOTA_STATE_IDLE;
    }
    return ctx->state;
}

uint8_t fota_get_progress(fota_context_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->progress;
}

fota_error_t fota_get_error(fota_context_t *ctx) {
    if (!ctx) {
        return FOTA_ERR_OTHER;
    }
    return ctx->error;
}

bool fota_check_disk_space(const char *path, uint64_t required_size) {
    struct statvfs stat_buf;
    
    if (statvfs(path, &stat_buf) != 0) {
        return false;
    }
    
    uint64_t available_space = (uint64_t)stat_buf.f_bavail * (uint64_t)stat_buf.f_frsize;
    return available_space >= required_size;
}

bool fota_ensure_directory(const char *dir_path) {
    struct stat dir_stat;
    
    // 检查目录是否存在
    if (stat(dir_path, &dir_stat) == 0) {
        return S_ISDIR(dir_stat.st_mode);
    }
    
    // 尝试创建目录
    return try_create_directory(dir_path, FOTA_MAX_RETRY_COUNT);
}

// ==================== 内部函数实现 ====================

static void update_fota_state(fota_context_t *ctx, fota_state_t state, fota_error_t error) {
    if (!ctx) {
        return;
    }
    
    ctx->state = state;
    ctx->error = error;
    
    // 触发回调
    trigger_fota_callback(ctx, state, error);
}

static void trigger_fota_callback(fota_context_t *ctx, fota_state_t state, fota_error_t error) {
    if (ctx && ctx->callback) {
        ctx->callback(ctx, state, error, ctx->user_data);
    }
}

/**
 * @brief 验证文件校验和 (SHA256)
 */
static bool verify_file_checksum(const char *file_path, const char *expected_checksum) {
    if (!file_path || !expected_checksum) {
        return false;
    }

    FILE *file = fopen(file_path, "rb");
    if (!file) {
        return false;
    }
    
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    const EVP_MD *md = EVP_sha256();
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    EVP_DigestInit_ex(mdctx, md, NULL);

    unsigned char buffer[4096];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        EVP_DigestUpdate(mdctx, buffer, bytes_read);
    }

    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    fclose(file);
    
    // Convert to hex string
    char calculated_checksum[65] = {0};
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(calculated_checksum + i * 2, "%02x", hash[i]);
    }
    
    // Case-insensitive comparison
    return strcasecmp(calculated_checksum, expected_checksum) == 0;
}

static bool try_create_directory(const char *dir_path, int max_retries) {
    for (int i = 0; i < max_retries; i++) {
        if (mkdir(dir_path, 0755) == 0) {
            return true;
        }
        
        // 检查是否是因为目录已存在
        if (errno == EEXIST) {
            struct stat dir_stat;
            if (stat(dir_path, &dir_stat) == 0 && S_ISDIR(dir_stat.st_mode)) {
                return true;
            }
        }
        
        // 检查是否是权限错误
        else if (errno == EACCES) {
            // 尝试修改父目录权限
            char parent_dir[1024];
            strncpy(parent_dir, dir_path, sizeof(parent_dir) - 1);
            char *last_slash = strrchr(parent_dir, '/');
            if (last_slash) {
                *last_slash = '\0';
                // 尝试修改父目录权限
                if (chmod(parent_dir, 0755) == 0) {
                    // 再次尝试创建目录
                    if (mkdir(dir_path, 0755) == 0) {
                        return true;
                    }
                }
            }
        }
        
        // 短暂延迟后重试
        usleep(100000); // 100ms
    }
    
    return false;
}

static FILE *try_open_file(const char *file_path, const char *mode, int max_retries) {
    FILE *file = NULL;
    
    for (int i = 0; i < max_retries; i++) {
        file = fopen(file_path, mode);
        if (file) {
            return file;
        }
        
        // 短暂延迟后重试
        usleep(100000); // 100ms
    }
    
    return NULL;
}



