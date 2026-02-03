/**
 * @file air8000_image_process.c
 * @brief Air8000 图片处理模块实现
 * @details 接收文件传输完成事件，自动调用image_processor处理图片
 */

#include "air8000_image_process.h"
#include "air8000_file_transfer.h"
#include "image_processor.h"
#include "message_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>

#define RECEIVED_FILE_DIR "/tmp/received_images" /* 接收图片的保存目录 */
#define PROCESSED_FILE_DIR "/tmp/processed_images" /* 处理结果保存目录 */

/**
 * @brief 图像处理上下文结构体
 */
typedef struct {
    air8000_t *air8000_ctx;  /* Air8000上下文 */
    int mq_fd;             /* 消息队列描述符 */
    uint32_t seq_num;        /* 序列号 */
} image_process_context_t;

static image_process_context_t *g_proc_ctx = NULL;

/**
 * @brief 确保目录存在
 */
static int ensure_dir_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        return -1;
    }
    return mkdir(path, 0777);
}

/**
 * @brief 检查是否为图片文件
 */
static bool is_image_file(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    ext++;
    
    if (strcasecmp(ext, "jpg") == 0 || 
        strcasecmp(ext, "jpeg") == 0 || 
        strcasecmp(ext, "png") == 0 || 
        strcasecmp(ext, "bmp") == 0) {
        return true;
    }
    return false;
}

/**
 * @brief 处理接收到的图片文件
 */
static void process_image_file(const char *filename) {
    if (!g_proc_ctx || !is_image_file(filename)) {
        return;
    }
    
    printf("[图片处理] 开始处理文件: %s\n", filename);
    
    /* 确保输出目录存在 */
    ensure_dir_exists(PROCESSED_FILE_DIR);
    
    /* 构建完整路径 */
    char input_path[512];
    snprintf(input_path, sizeof(input_path), "%s/%s", RECEIVED_FILE_DIR, filename);
    
    /* 调用图像处理算法 */
    int result = image_processor_process_image(input_path, PROCESSED_FILE_DIR, 0);
    
    if (result == 0) {
        printf("[图片处理] 处理完成\n");
        
        /* 检查消息队列是否可用 */
        if (g_proc_ctx->mq_fd != -1) {
            /* 发送处理结果到MQTT */
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_IMAGE_PROCESSED;
            msg.seq_num = g_proc_ctx->seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            
            image_process_result_t *result_info = (image_process_result_t *)&msg.payload.data;
            result_info->success = 1;
            result_info->paragraph_count = image_processor_get_paragraph_count();
            
            for (int i = 0; i < result_info->paragraph_count && i < 10; i++) {
                paragraph_t para;
                if (image_processor_get_paragraph(i, &para) == 0) {
                    // 使用memcpy复制结构体内容到数组
                    memcpy(result_info->paragraphs[i], &para, sizeof(paragraph_t));
                }
            }
            
            msg.data_len = sizeof(image_process_result_t);
            
            if (mq_send_msg(g_proc_ctx->mq_fd, &msg, 0) == 0) {
                printf("[图片处理] 处理结果已发送到MQTT\n");
            } else {
                perror("[图片处理] 发送处理结果失败");
            }
        } else {
            printf("[图片处理] 消息队列不可用，跳过发送处理结果\n");
        }
    } else {
        printf("[图片处理] 处理失败\n");
        
        /* 检查消息队列是否可用 */
        if (g_proc_ctx->mq_fd != -1) {
            /* 发送失败通知 */
            message_t msg;
            memset(&msg, 0, sizeof(msg));
            msg.type = MSG_TYPE_IMAGE_PROCESSED;
            msg.seq_num = g_proc_ctx->seq_num++;
            msg.timestamp = (uint32_t)time(NULL);
            
            image_process_result_t *result_info = (image_process_result_t *)&msg.payload.data;
            result_info->success = 0;
            result_info->paragraph_count = 0;
            msg.data_len = sizeof(image_process_result_t);
            
            if (mq_send_msg(g_proc_ctx->mq_fd, &msg, 0) == 0) {
                printf("[图片处理] 处理失败通知已发送到MQTT\n");
            } else {
                perror("[图片处理] 发送处理失败通知失败");
            }
        } else {
            printf("[图片处理] 消息队列不可用，跳过发送处理失败通知\n");
        }
    }
}

/**
 * @brief 文件传输回调函数
 */
static void file_transfer_callback(air8000_t *ctx, air8000_file_transfer_event_t event, void *data, void *user_data) {
    (void)ctx;
    (void)user_data;
    
    switch (event) {
        case FILE_TRANSFER_EVENT_COMPLETED:
            /* 文件传输完成，处理图片 */
            process_image_file((const char *)data);
            break;
        default:
            break;
    }
}

/**
 * @brief 初始化图片处理模块
 */
int air8000_image_process_init(air8000_t *ctx, int mq_fd) {
    if (!ctx) {
        return -1;
    }
    
    if (g_proc_ctx) {
        return -1;
    }
    
    g_proc_ctx = (image_process_context_t *)malloc(sizeof(image_process_context_t));
    if (!g_proc_ctx) {
        return -1;
    }
    
    memset(g_proc_ctx, 0, sizeof(image_process_context_t));
    g_proc_ctx->air8000_ctx = ctx;
    g_proc_ctx->mq_fd = mq_fd;
    g_proc_ctx->seq_num = 0;
    
    /* 确保目录存在 */
    if (ensure_dir_exists(RECEIVED_FILE_DIR) != 0) {
        printf("[图片处理] 警告：无法创建接收目录，将使用当前目录\n");
    }
    if (ensure_dir_exists(PROCESSED_FILE_DIR) != 0) {
        printf("[图片处理] 警告：无法创建处理目录，将使用当前目录\n");
    }
    
    /* 注册文件传输回调，监听传输完成事件 */
    air8000_file_transfer_register_callback(file_transfer_callback, NULL);
    
    printf("[图片处理] 模块初始化完成\n");
    return 0;
}

/**
 * @brief 清理图片处理模块
 */
void air8000_image_process_deinit(void) {
    if (g_proc_ctx) {
        free(g_proc_ctx);
        g_proc_ctx = NULL;
    }
}
