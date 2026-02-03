/**
 * @file message_queue.h
 * @brief 消息队列模块头文件
 * @version 1.0
 * @date 2026-01-22
 */

#ifndef MESSAGE_QUEUE_H
#define MESSAGE_QUEUE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/msg.h>
#include <sys/ipc.h>


#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 消息队列名称定义
 */
#define MSG_QUEUE_UART_TO_MQTT "/air8000_uart_to_mqtt"
#define MSG_QUEUE_MQTT_TO_UART "/air8000_mqtt_to_uart"

/**
 * @brief 消息队列最大消息数
 */
#define MSG_QUEUE_MAX_MESSAGES 10

/**
 * @brief 消息队列最大消息大小
 */
#define MSG_QUEUE_MAX_MSG_SIZE 512

/**
 * @brief 消息类型枚举
 */
typedef enum {
    MSG_TYPE_SENSOR_DATA = 1,     // 传感器数据
    MSG_TYPE_MOTOR_CMD,            // 电机控制指令
    MSG_TYPE_DEVICE_CMD,           // 设备控制指令
    MSG_TYPE_HEARTBEAT,            // 心跳消息
    MSG_TYPE_RESPONSE,             // 响应消息
    MSG_TYPE_FILE_DATA,            // 文件数据消息
    MSG_TYPE_FILE_START,           // 文件传输开始通知
    MSG_TYPE_FILE_END,             // 文件传输结束通知
    MSG_TYPE_FILE_ACK,             // 文件分片确认
    MSG_TYPE_FILE_NACK,            // 文件分片否定确认
    MSG_TYPE_FILE_INFO,            // 文件信息（大小、名称等）
    MSG_TYPE_FOTA_DATA,            // FOTA升级数据消息
    MSG_TYPE_FOTA_START,           // 开始FOTA升级指令
    MSG_TYPE_FOTA_END,             // FOTA升级结束通知
    MSG_TYPE_FOTA_COMPLETE,        // FOTA升级完成通知
    MSG_TYPE_FILE_COMPLETE,        // 文件传输完成通知
    MSG_TYPE_IMAGE_PROCESSED       // 图片处理完成通知
} msg_type_t;

/**
 * @brief 图片处理结果结构体
 */
typedef struct {
    uint8_t success;
    uint8_t paragraph_count;
    uint8_t paragraphs[10][64];;
} image_process_result_t;

/**
 * @brief 文件传输元数据结构体
 */
typedef struct {
    uint32_t file_id;          // 文件唯一标识符
    uint32_t total_chunks;     // 总分片数
    uint32_t current_chunk;    // 当前分片号
    uint32_t chunk_offset;     // 分片在文件中的偏移量
    uint32_t chunk_size;       // 当前分片大小
    uint64_t file_size;        // 文件总大小
    char filename[64];         // 文件名
} file_transfer_metadata_t;

/**
 * @brief 消息结构体
 */
typedef struct {
    msg_type_t type;          // 消息类型
    uint32_t seq_num;         // 序列号
    uint32_t timestamp;       // 时间戳
    size_t data_len;          // 数据长度
    union {
        uint8_t data[256];                // 消息数据
        file_transfer_metadata_t file_meta; // 文件传输元数据
        image_process_result_t img_result; // 图片处理结果
    } payload;                // 消息负载，支持多种类型
} message_t;

/**
 * @brief 消息队列配置结构体
 */
typedef struct {
    const char *name;         // 消息队列名称
    int max_messages;         // 最大消息数
    int max_msg_size;         // 最大消息大小
    int flags;                // 打开标志
    mode_t mode;              // 权限模式
    int msg_perm;             // System V IPC 权限
} mq_config_t;

/**
 * @brief System V 消息队列的消息结构体
 */
typedef struct {
    long mtype;               // 消息类型
    message_t msg;            // 自定义消息内容
    unsigned int priority;    // 消息优先级
} sysv_msgbuf_t;

/**
 * @brief 创建消息队列
 * @param name 消息队列名称
 * @param config 消息队列配置指针，NULL表示使用默认配置
 * @return 成功返回消息队列ID，失败返回-1
 */
int mq_create(const char *name, const mq_config_t *config);

/**
 * @brief 打开已存在的消息队列
 * @param name 消息队列名称
 * @param flags 打开标志
 * @return 成功返回消息队列ID，失败返回-1
 */
int mq_open_existing(const char *name, int flags);

/**
 * @brief 关闭消息队列
 * @param mq_fd 消息队列ID
 * @return 成功返回0，失败返回-1
 */
int mq_close_queue(int mq_fd);

/**
 * @brief 删除消息队列
 * @param name 消息队列名称
 * @return 成功返回0，失败返回-1
 */
int mq_delete_queue(const char *name);

/**
 * @brief 发送消息
 * @param mq_fd 消息队列ID
 * @param msg 消息指针
 * @param priority 消息优先级
 * @return 成功返回0，失败返回-1
 */
int mq_send_msg(int mq_fd, const message_t *msg, unsigned int priority);

/**
 * @brief 接收消息
 * @param mq_fd 消息队列ID
 * @param msg 消息接收缓冲区指针
 * @param priority 输出参数，用于返回消息优先级
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回0，失败返回-1，超时返回1
 */
int mq_receive_msg(int mq_fd, message_t *msg, unsigned int *priority, int timeout_ms);

/**
 * @brief 获取消息队列属性
 * @param mq_fd 消息队列ID
 * @param attr 输出参数，用于返回消息队列属性
 * @return 成功返回0，失败返回-1
 */
int mq_get_attr(int mq_fd, struct msqid_ds *attr);

/**
 * @brief 设置消息队列属性
 * @param mq_fd 消息队列ID
 * @param new_attr 新的属性设置
 * @param old_attr 输出参数，用于返回旧的属性
 * @return 成功返回0，失败返回-1
 */
int mq_set_attr(int mq_fd, const struct msqid_ds *new_attr, struct msqid_ds *old_attr);

#ifdef __cplusplus
}
#endif

#endif /* MESSAGE_QUEUE_H */
