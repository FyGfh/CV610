/**
 * @file message_queue.c
 * @brief 消息队列模块实现
 * @version 1.0
 * @date 2026-01-22
 */

#include "message_queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/**
 * @brief 从消息队列名称生成System V IPC键值
 * @param name 消息队列名称
 * @return 成功返回键值，失败返回-1
 */
static key_t mq_name_to_key(const char *name) {
    if (name == NULL) {
        return -1;
    }
    
    // 使用固定键值映射表，避免依赖ftok()函数
    if (strcmp(name, MSG_QUEUE_UART_TO_MQTT) == 0) {
        return 0x12345678; // 固定键值1
    } else if (strcmp(name, MSG_QUEUE_MQTT_TO_UART) == 0) {
        return 0x87654321; // 固定键值2
    }
    
    return -1;
}

/**
 * @brief System V消息队列的消息大小
 */
#define SYSV_MSG_SIZE sizeof(sysv_msgbuf_t)

/**
 * @brief 创建消息队列
 * @param name 消息队列名称
 * @param config 消息队列配置指针，NULL表示使用默认配置
 * @return 成功返回消息队列ID，失败返回-1
 */
int mq_create(const char *name, const mq_config_t *config) {
    if (name == NULL) {
        return -1;
    }

    key_t key = mq_name_to_key(name);
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    int msg_flags = IPC_CREAT | IPC_EXCL | 0666;

    // 使用默认配置或用户提供的配置
    if (config != NULL) {
        if (config->msg_perm != 0) {
            msg_flags = IPC_CREAT | IPC_EXCL | config->msg_perm;
        }
    }

    // 创建消息队列
    int msg_id = msgget(key, msg_flags);
    if (msg_id == -1) {
        // 如果消息队列已存在，尝试打开
        msg_flags = IPC_CREAT | 0666;
        if (config != NULL && config->msg_perm != 0) {
            msg_flags = IPC_CREAT | config->msg_perm;
        }
        
        msg_id = msgget(key, msg_flags);
        if (msg_id == -1) {
            perror("msgget");
            return -1;
        }
    }

    return msg_id;
}

/**
 * @brief 打开已存在的消息队列
 * @param name 消息队列名称
 * @param flags 打开标志（此处未使用，保持API兼容）
 * @return 成功返回消息队列ID，失败返回-1
 */
int mq_open_existing(const char *name, int flags) {
    (void)flags; // 明确表示该参数是故意未使用的
    if (name == NULL) {
        return -1;
    }

    key_t key = mq_name_to_key(name);
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    // 打开已存在的消息队列
    int msg_id = msgget(key, 0666);
    if (msg_id == -1) {
        perror("msgget");
        return -1;
    }

    return msg_id;
}

/**
 * @brief 关闭消息队列
 * @param mq_fd 消息队列ID
 * @return 成功返回0，失败返回-1
 * @note System V消息队列不需要显式关闭，此函数仅保持API兼容
 */
int mq_close_queue(int mq_fd) {
    if (mq_fd == -1) {
        return -1;
    }

    // System V消息队列不需要显式关闭，直接返回成功
    return 0;
}

/**
 * @brief 删除消息队列
 * @param name 消息队列名称
 * @return 成功返回0，失败返回-1
 */
int mq_delete_queue(const char *name) {
    if (name == NULL) {
        return -1;
    }

    key_t key = mq_name_to_key(name);
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    // 获取消息队列ID
    int msg_id = msgget(key, 0);
    if (msg_id == -1) {
        perror("msgget");
        return -1;
    }

    // 删除消息队列
    if (msgctl(msg_id, IPC_RMID, NULL) == -1) {
        perror("msgctl");
        return -1;
    }

    return 0;
}

/**
 * @brief 发送消息
 * @param mq_fd 消息队列ID
 * @param msg 消息指针
 * @param priority 消息优先级
 * @return 成功返回0，失败返回-1
 */
int mq_send_msg(int mq_fd, const message_t *msg, unsigned int priority) {
    if (mq_fd == -1 || msg == NULL) {
        return -1;
    }

    // 检查消息大小
    if (msg->data_len > sizeof(msg->payload.data)) {
        return -1;
    }

    // 创建System V消息缓冲区
    sysv_msgbuf_t msg_buf;
    memset(&msg_buf, 0, sizeof(msg_buf));
    
    // 设置消息类型（固定为1，因为我们不使用消息类型进行过滤）
    msg_buf.mtype = 1;
    
    // 复制消息内容
    msg_buf.msg = *msg;
    if (msg_buf.msg.timestamp == 0) {
        msg_buf.msg.timestamp = (uint32_t)time(NULL);
    }
    
    // 设置优先级
    msg_buf.priority = priority;

    // 发送消息
    if (msgsnd(mq_fd, &msg_buf, SYSV_MSG_SIZE - sizeof(long), 0) == -1) {
        perror("msgsnd");
        return -1;
    }

    return 0;
}

/**
 * @brief 接收消息
 * @param mq_fd 消息队列ID
 * @param msg 消息接收缓冲区指针
 * @param priority 输出参数，用于返回消息优先级
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回0，失败返回-1，超时返回1
 */
int mq_receive_msg(int mq_fd, message_t *msg, unsigned int *priority, int timeout_ms) {
    if (mq_fd == -1 || msg == NULL) {
        return -1;
    }

    ssize_t bytes_read;
    sysv_msgbuf_t msg_buf;
    memset(&msg_buf, 0, sizeof(msg_buf));

    if (timeout_ms == -1) {
        // 无限等待
        bytes_read = msgrcv(mq_fd, &msg_buf, SYSV_MSG_SIZE - sizeof(long), 1, 0);
    } else {
        // 非阻塞尝试接收，没有消息则返回超时
        bytes_read = msgrcv(mq_fd, &msg_buf, SYSV_MSG_SIZE - sizeof(long), 1, IPC_NOWAIT);
        if (bytes_read == -1) {
            if (errno == ENOMSG) {
                // 没有消息，返回超时
                return 1;
            }
            perror("msgrcv");
            return -1;
        }
    }

    if (bytes_read == -1) {
        perror("msgrcv");
        return -1;
    }

    // 复制消息内容
    *msg = msg_buf.msg;
    
    // 返回优先级
    if (priority != NULL) {
        *priority = msg_buf.priority;
    }

    return 0;
}

/**
 * @brief 获取消息队列属性
 * @param mq_fd 消息队列ID
 * @param attr 输出参数，用于返回消息队列属性
 * @return 成功返回0，失败返回-1
 */
int mq_get_attr(int mq_fd, struct msqid_ds *attr) {
    if (mq_fd == -1 || attr == NULL) {
        return -1;
    }

    if (msgctl(mq_fd, IPC_STAT, attr) == -1) {
        perror("msgctl");
        return -1;
    }

    return 0;
}

/**
 * @brief 设置消息队列属性
 * @param mq_fd 消息队列ID
 * @param new_attr 新的属性设置
 * @param old_attr 输出参数，用于返回旧的属性
 * @return 成功返回0，失败返回-1
 */
int mq_set_attr(int mq_fd, const struct msqid_ds *new_attr, struct msqid_ds *old_attr) {
    if (mq_fd == -1 || new_attr == NULL) {
        return -1;
    }

    // 获取当前属性
    struct msqid_ds curr_attr;
    if (msgctl(mq_fd, IPC_STAT, &curr_attr) == -1) {
        perror("msgctl");
        return -1;
    }

    // 返回旧属性
    if (old_attr != NULL) {
        *old_attr = curr_attr;
    }

    // 更新属性
    curr_attr.msg_perm = new_attr->msg_perm;
    
    if (msgctl(mq_fd, IPC_SET, &curr_attr) == -1) {
        perror("msgctl");
        return -1;
    }

    return 0;
}
