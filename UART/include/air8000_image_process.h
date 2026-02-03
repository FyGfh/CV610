/**
 * @file air8000_image_process.h
 * @brief Air8000 图片处理模块
 * @details 接收文件传输完成事件，自动调用image_processor处理图片
 */

#ifndef AIR8000_IMAGE_PROCESS_H
#define AIR8000_IMAGE_PROCESS_H

#include "air8000.h"
#include <stdbool.h>
#include <stddef.h>
#include <mqueue.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化图片处理模块
 * @param ctx Air8000上下文指针
 * @param mq_fd 消息队列描述符（用于发送处理结果）
 * @return 成功返回0，失败返回错误码
 */
int air8000_image_process_init(air8000_t *ctx, int mq_fd);

/**
 * @brief 清理图片处理模块
 */
void air8000_image_process_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_IMAGE_PROCESS_H
