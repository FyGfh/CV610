/**
 * @file process_manager.c
 * @brief 进程管理模块实现
 * @version 1.0
 * @date 2026-01-22
 */

#include "process_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <errno.h>
#include <time.h>

/**
 * @brief 创建进程
 * @param config 进程配置指针
 * @return 成功返回进程结构体指针，失败返回NULL
 */
process_t* process_create(const process_config_t *config) {
    if (config == NULL || config->cmd == NULL) {
        return NULL;
    }

    // 分配进程结构体内存
    process_t *process = (process_t *)malloc(sizeof(process_t));
    if (process == NULL) {
        return NULL;
    }

    // 初始化进程结构体
    memset(process, 0, sizeof(process_t));
    
    // 设置进程类型
    process->type = config->type;
    
    // 设置进程名称
    if (config->name != NULL) {
        strncpy(process->name, config->name, sizeof(process->name) - 1);
    } else {
        // 默认名称
        if (config->type == PROC_TYPE_UART) {
            strcpy(process->name, "uart_process");
        } else if (config->type == PROC_TYPE_MQTT) {
            strcpy(process->name, "mqtt_process");
        }
    }
    
    // 设置命令和参数
    strncpy(process->cmd, config->cmd, sizeof(process->cmd) - 1);
    if (config->args != NULL) {
        strncpy(process->args, config->args, sizeof(process->args) - 1);
    }
    
    // 设置初始状态
    process->state = PROC_STATE_CREATED;
    process->pid = -1;
    process->exit_code = 0;
    
    // 设置私有数据
    process->private_data = config->private_data;
    
    return process;
}

/**
 * @brief 启动进程
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_start(process_t *process) {
    if (process == NULL || process->cmd == NULL) {
        return -1;
    }

    // 检查进程是否已经在运行
    if (process->state == PROC_STATE_RUNNING) {
        return 0;
    }

    // 调用fork创建子进程
    pid_t pid = fork();
    if (pid == -1) {
        perror("fork");
        process->state = PROC_STATE_ERROR;
        return -1;
    }

    if (pid == 0) {
        // 子进程
        
        // 将标准输出和标准错误重定向到父进程的终端
        // 这样子进程的输出就能在父进程的终端中看到
        dup2(STDOUT_FILENO, STDOUT_FILENO); // 标准输出
        dup2(STDERR_FILENO, STDERR_FILENO); // 标准错误
        
        // 解析命令参数
        char *argv[64];
        int argc = 0;
        
        // 第一个参数是命令本身
        argv[argc++] = process->cmd;
        
        // 解析空格分隔的参数
        if (process->args[0] != '\0') {
            char *token = strtok(process->args, " ");
            while (token != NULL && argc < 63) {
                argv[argc++] = token;
                token = strtok(NULL, " ");
            }
        }
        
        // 结束参数列表
        argv[argc] = NULL;
        
        // 执行命令
        execvp(process->cmd, argv);
        
        // 如果execvp返回，说明执行失败
        perror("execvp");
        exit(EXIT_FAILURE);
    } else {
        // 父进程
        process->pid = pid;
        process->state = PROC_STATE_RUNNING;
        return 0;
    }
}

/**
 * @brief 停止进程
 * @param process 进程结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回-1
 */
int process_stop(process_t *process, int timeout_ms) {
    if (process == NULL || process->pid == -1) {
        return -1;
    }

    if (process->state != PROC_STATE_RUNNING) {
        return 0;
    }

    // 发送SIGTERM信号
    if (kill(process->pid, SIGTERM) == -1) {
        perror("kill");
        return -1;
    }

    // 更新进程状态
    process->state = PROC_STATE_STOPPED;
    
    // 等待进程退出
    int exit_code = process_wait(process, timeout_ms);
    if (exit_code == -1) {
        // 超时，发送SIGKILL信号强制终止
        if (kill(process->pid, SIGKILL) == -1) {
            perror("kill");
            return -1;
        }
        
        // 再次等待
        exit_code = process_wait(process, 1000);
        if (exit_code == -1) {
            return -1;
        }
    }
    
    process->exit_code = exit_code;
    process->state = PROC_STATE_EXITED;
    
    return 0;
}

/**
 * @brief 销毁进程
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_destroy(process_t *process) {
    if (process == NULL) {
        return -1;
    }

    // 如果进程正在运行，先停止它
    if (process->state == PROC_STATE_RUNNING) {
        process_stop(process, 5000);
    }

    // 释放内存
    free(process);
    
    return 0;
}

/**
 * @brief 获取进程状态
 * @param process 进程结构体指针
 * @return 进程状态
 */
process_state_t process_get_state(const process_t *process) {
    if (process == NULL) {
        return PROC_STATE_ERROR;
    }

    return process->state;
}

/**
 * @brief 更新进程状态
 * @param process 进程结构体指针
 * @return 成功返回0，失败返回-1
 */
int process_update_state(process_t *process) {
    if (process == NULL || process->pid == -1) {
        return -1;
    }

    if (process->state == PROC_STATE_EXITED || process->state == PROC_STATE_ERROR) {
        return 0;
    }

    // 检查进程是否存在
    if (kill(process->pid, 0) == -1) {
        if (errno == ESRCH) {
            // 进程不存在，更新状态为已退出
            process->state = PROC_STATE_EXITED;
            
            // 尝试获取退出码
            int status;
            if (waitpid(process->pid, &status, WNOHANG) > 0) {
                if (WIFEXITED(status)) {
                    process->exit_code = WEXITSTATUS(status);
                } else if (WIFSIGNALED(status)) {
                    process->exit_code = -WTERMSIG(status);
                }
            }
        }
        return -1;
    }

    // 进程存在，检查是否正在运行
    if (process->state != PROC_STATE_RUNNING) {
        process->state = PROC_STATE_RUNNING;
    }

    return 0;
}

/**
 * @brief 检查进程是否正在运行
 * @param process 进程结构体指针
 * @return 运行中返回true，否则返回false
 */
bool process_is_running(const process_t *process) {
    if (process == NULL) {
        return false;
    }

    return (process->state == PROC_STATE_RUNNING);
}

/**
 * @brief 等待进程退出
 * @param process 进程结构体指针
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回进程退出码，超时返回-1，失败返回-2
 */
int process_wait(process_t *process, int timeout_ms) {
    if (process == NULL || process->pid == -1) {
        return -2;
    }

    int status;
    pid_t result;
    
    if (timeout_ms == -1) {
        // 无限等待
        result = waitpid(process->pid, &status, 0);
    } else {
        // 超时等待
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        
        result = waitpid(process->pid, &status, WNOHANG);
        if (result == 0) {
            // 进程还在运行，使用nanosleep等待
            nanosleep(&ts, NULL);
            result = waitpid(process->pid, &status, WNOHANG);
        }
    }
    
    if (result == -1) {
        perror("waitpid");
        return -2;
    }
    
    if (result == 0) {
        // 超时
        return -1;
    }
    
    // 进程已退出，获取退出码
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        return -WTERMSIG(status);
    } else {
        return -2;
    }
}

/**
 * @brief 重启进程
 * @param process 进程结构体指针
 * @param delay_ms 延迟时间（毫秒）
 * @return 成功返回0，失败返回-1
 */
int process_restart(process_t *process, int delay_ms) {
    if (process == NULL) {
        return -1;
    }

    // 停止进程
    if (process->state == PROC_STATE_RUNNING) {
        if (process_stop(process, 5000) == -1) {
            return -1;
        }
    }
    
    // 延迟
    if (delay_ms > 0) {
        struct timespec ts;
        ts.tv_sec = delay_ms / 1000;
        ts.tv_nsec = (delay_ms % 1000) * 1000000;
        nanosleep(&ts, NULL);
    }
    
    // 启动进程
    return process_start(process);
}

/**
 * @brief 获取进程ID
 * @param process 进程结构体指针
 * @return 进程ID
 */
pid_t process_get_pid(const process_t *process) {
    if (process == NULL) {
        return -1;
    }

    return process->pid;
}
