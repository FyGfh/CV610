/**
 * @file process_manager.h
 * @brief 进程管理模块头文件
 * @version 1.0
 * @date 2026-01-22
 */

#ifndef PROCESS_MANAGER_H
#define PROCESS_MANAGER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 进程状态枚举
 */
typedef enum {
    PROC_STATE_CREATED = 0,   // 已创建
    PROC_STATE_RUNNING,       // 运行中
    PROC_STATE_STOPPED,       // 已停止
    PROC_STATE_EXITED,        // 已退出
    PROC_STATE_ERROR          // 错误状态
} process_state_t;

/**
 * @brief 进程类型枚举
 */
typedef enum {
    PROC_TYPE_UART = 1,        // UART进程
    PROC_TYPE_MQTT             // MQTT进程
} process_type_t;

/**
 * @brief 进程结构体
 */
typedef struct {
    pid_t pid;                 // 进程ID
    process_type_t type;       // 进程类型
    char name[32];             // 进程名称
    process_state_t state;     // 进程状态
    int exit_code;             // 退出码
    char cmd[256];             // 命令行
    char args[512];            // 命令参数
    void *private_data;        // 私有数据指针
} process_t;

/**
 * @brief 进程管理配置结构体
 */
typedef struct {
    const char *name;          // 进程名称
    process_type_t type;       // 进程类型
    const char *cmd;           // 可执行文件路径
    const char *args;          // 命令参数
    bool auto_restart;         // 是否自动重启
    int restart_delay;         // 重启延迟（毫秒）
    void *private_data;        // 私有数据
} process_config_t;

/**
 * @brief 创建进程
 * @param config 进程配置指针
 * @return 成功返回进程结构体指针，失败返回NULL
 */
process_t* process_create(const process_config_t *config);

/**
 * @brief 启动进程
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_start(process_t *process);

/**
 * @brief 停止进程
 * @param process 进程结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回-1
 */
int process_stop(process_t *process, int timeout_ms);

/**
 * @brief 销毁进程
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_destroy(process_t *process);

/**
 * @brief 获取进程状态
 * @param process 进程结构体指针
 * @return 进程状态
 */
process_state_t process_get_state(const process_t *process);

/**
 * @brief 更新进程状态
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_update_state(process_t *process);

/**
 * @brief 检查进程是否正在运行
 * @param process 进程结构体指针
 * @return 运行中返回true，否则返回false
 */
bool process_is_running(const process_t *process);

/**
 * @brief 等待进程退出
 * @param process 进程结构体指针
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回进程退出码，超时返回-1，失败返回-2
 */
int process_wait(process_t *process, int timeout_ms);

/**
 * @brief 重启进程
 * @param process 进程结构体指针
 * @param delay_ms 延迟时间（毫秒）
 * @return 成功返回0，失败返回-1
 */
int process_restart(process_t *process, int delay_ms);

/**
 * @brief 获取进程ID
 * @param process 进程结构体指针
 * @return 进程ID
 */
pid_t process_get_pid(const process_t *process);

#ifdef __cplusplus
}
#endif

#endif /* PROCESS_MANAGER_H */
