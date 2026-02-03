/**
 * @file air8000_log.h
 * @brief Air8000 统一日志格式头文件
 * @details 定义了统一的日志宏，用于项目中所有文件的日志输出
 */

#ifndef AIR8000_LOG_H
#define AIR8000_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 日志宏定义
 * @details 提供统一的日志格式，包含标签和日志级别
 * 
 * @param tag 日志标签，用于标识日志来源
 * @param fmt 格式字符串
 * @param ... 可变参数
 */

/**
 * @brief 普通信息日志
 * @param tag 日志标签
 * @param fmt 格式字符串
 * @param ... 可变参数
 */
#define log_info(tag, fmt, ...) printf("[%s] " fmt "\n", tag, ##__VA_ARGS__)

/**
 * @brief 警告日志
 * @param tag 日志标签
 * @param fmt 格式字符串
 * @param ... 可变参数
 */
#define log_warn(tag, fmt, ...) printf("[%s WARN] " fmt "\n", tag, ##__VA_ARGS__)

/**
 * @brief 错误日志
 * @param tag 日志标签
 * @param fmt 格式字符串
 * @param ... 可变参数
 */
#define log_error(tag, fmt, ...) printf("[%s ERROR] " fmt "\n", tag, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif // AIR8000_LOG_H
