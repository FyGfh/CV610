#include "mqtt_file_upload.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

// ==================== 内部函数声明 ====================

/**
 * @brief 生成唯一文件ID
 */
static void generate_file_id(char *buffer, size_t buffer_size);

/**
 * @brief 从文件路径中提取文件名
 */
static void extract_filename(const char *file_path, char *filename, size_t filename_size);

/**
 * @brief 尝试打开文件（带重试）
 */
static FILE *try_open_file(const char *file_path, const char *mode, int max_retries);

// ==================== 内部函数实现 ====================

static void generate_file_id(char *buffer, size_t buffer_size) {
    time_t now = time(NULL);
    snprintf(buffer, buffer_size, "%lld_%d", (long long)now, rand() % 10000);
}

static void extract_filename(const char *file_path, char *filename, size_t filename_size) {
    const char *last_slash = strrchr(file_path, '/');
    if (last_slash) {
        strncpy(filename, last_slash + 1, filename_size - 1);
    } else {
        strncpy(filename, file_path, filename_size - 1);
    }
    filename[filename_size - 1] = '\0';
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

// ==================== API 函数实现 ====================

file_upload_context_t *file_upload_create(const char *file_path, uint32_t chunk_size) {
    if (!file_path) {
        return NULL;
    }

    // 检查文件是否存在
    struct stat file_stat;
    if (stat(file_path, &file_stat) != 0) {
        return NULL;
    }

    // 分配上传上下文
    file_upload_context_t *ctx = (file_upload_context_t *)malloc(sizeof(file_upload_context_t));
    if (!ctx) {
        return NULL;
    }

    // 初始化上下文
    memset(ctx, 0, sizeof(file_upload_context_t));
    strncpy(ctx->file_path, file_path, sizeof(ctx->file_path) - 1);
    generate_file_id(ctx->file_id, sizeof(ctx->file_id));
    extract_filename(file_path, ctx->filename, sizeof(ctx->filename));
    
    ctx->file_size = file_stat.st_size;
    ctx->chunk_size = chunk_size ? chunk_size : 16384; // 默认16KB
    ctx->total_chunks = (ctx->file_size + ctx->chunk_size - 1) / ctx->chunk_size;
    ctx->state = FILE_UPLOAD_STATE_IDLE;
    ctx->error = FILE_UPLOAD_ERR_NONE;
    ctx->progress = 0;
    ctx->aborted = false;

    return ctx;
}

void file_upload_destroy(file_upload_context_t *ctx) {
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

bool file_upload_start(file_upload_context_t *ctx) {
    if (!ctx || ctx->state != FILE_UPLOAD_STATE_IDLE) {
        return false;
    }

    // 打开文件
    ctx->file_handle = try_open_file(ctx->file_path, "rb", FILE_UPLOAD_MAX_RETRY_COUNT);
    if (!ctx->file_handle) {
        ctx->error = FILE_UPLOAD_ERR_FILE;
        ctx->state = FILE_UPLOAD_STATE_FAILED;
        return false;
    }

    // 更新状态
    ctx->state = FILE_UPLOAD_STATE_UPLOADING;
    ctx->current_chunk = 0;
    ctx->uploaded_size = 0;
    ctx->progress = 0;

    return true;
}

bool file_upload_get_next_chunk(file_upload_context_t *ctx, uint8_t **chunk_data, size_t *chunk_data_len, uint32_t *chunk_id) {
    if (!ctx || !chunk_data || !chunk_data_len || !chunk_id) {
        return false;
    }

    if (ctx->state != FILE_UPLOAD_STATE_UPLOADING) {
        return false;
    }

    if (ctx->current_chunk >= ctx->total_chunks) {
        return false;
    }

    // 计算当前分片大小
    size_t current_chunk_size = ctx->chunk_size;
    if (ctx->current_chunk == ctx->total_chunks - 1) {
        current_chunk_size = ctx->file_size - (ctx->total_chunks - 1) * ctx->chunk_size;
    }

    // 分配分片数据内存
    uint8_t *data = (uint8_t *)malloc(current_chunk_size);
    if (!data) {
        ctx->error = FILE_UPLOAD_ERR_NOMEM;
        ctx->state = FILE_UPLOAD_STATE_FAILED;
        return false;
    }

    // 读取分片数据
    size_t read_size = fread(data, 1, current_chunk_size, ctx->file_handle);
    if (read_size != current_chunk_size) {
        free(data);
        ctx->error = FILE_UPLOAD_ERR_FILE;
        ctx->state = FILE_UPLOAD_STATE_FAILED;
        return false;
    }

    // 更新状态
    *chunk_data = data;
    *chunk_data_len = read_size;
    *chunk_id = ctx->current_chunk;

    ctx->current_chunk++;
    ctx->uploaded_size += read_size;
    ctx->progress = (uint8_t)((ctx->uploaded_size * 100) / ctx->file_size);

    return true;
}

bool file_upload_finish(file_upload_context_t *ctx) {
    if (!ctx) {
        return false;
    }

    // 关闭文件
    if (ctx->file_handle) {
        fclose(ctx->file_handle);
        ctx->file_handle = NULL;
    }

    // 更新状态
    ctx->state = FILE_UPLOAD_STATE_COMPLETE;

    return true;
}

bool file_upload_abort(file_upload_context_t *ctx) {
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

    // 更新状态
    ctx->state = FILE_UPLOAD_STATE_FAILED;

    return true;
}

file_upload_state_t file_upload_get_state(file_upload_context_t *ctx) {
    if (!ctx) {
        return FILE_UPLOAD_STATE_IDLE;
    }
    return ctx->state;
}

uint8_t file_upload_get_progress(file_upload_context_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->progress;
}

file_upload_error_t file_upload_get_error(file_upload_context_t *ctx) {
    if (!ctx) {
        return FILE_UPLOAD_ERR_OTHER;
    }
    return ctx->error;
}

uint64_t file_upload_get_file_size(file_upload_context_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->file_size;
}

uint32_t file_upload_get_total_chunks(file_upload_context_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return ctx->total_chunks;
}
