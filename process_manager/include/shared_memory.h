/**
 * @file shared_memory.h
 * @brief 共享内存模块头文件
 * @version 1.0
 * @date 2026-01-22
 */

#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 共享内存名称定义
 */
#define SHARED_MEM_NAME "/air8000_shared_memory"

/**
 * @brief 共享内存大小定义
 */
#define SHARED_MEM_SIZE (16 * 1024)  // 扩展到16KB

/**
 * @brief 共享内存段数量
 */
#define SHARED_MEM_SEGMENTS 4

/**
 * @brief 单个共享内存段大小
 */
#define SHARED_MEM_SEG_SIZE (SHARED_MEM_SIZE / SHARED_MEM_SEGMENTS)

/**
 * @brief 共享内存段状态枚举
 */
typedef enum {
    SHARED_MEM_SEG_FREE = 0,      // 段空闲
    SHARED_MEM_SEG_USED = 1,      // 段已使用
    SHARED_MEM_SEG_LOCKED = 2     // 段被锁定
} shared_mem_seg_state_t;

/**
 * @brief 共享内存段结构体
 */
typedef struct {
    shared_mem_seg_state_t state;  // 段状态
    uint32_t owner_pid;           // 所有者进程ID
    uint32_t data_len;            // 段内数据长度
    uint32_t seq_num;             // 数据序列号
    uint8_t data[SHARED_MEM_SEG_SIZE];  // 段数据缓冲区
} shared_mem_segment_t;

/**
 * @brief 共享内存控制块
 */
typedef struct {
    pthread_mutex_t mutex;                // 互斥锁，用于同步访问
    pthread_cond_t cond;                  // 条件变量，用于通知数据更新
    uint32_t total_segments;              // 总段数
    uint32_t free_segments;               // 空闲段数
    shared_mem_segment_t segments[SHARED_MEM_SEGMENTS];  // 共享内存段数组
} shared_mem_ctrl_t;

/**
 * @brief 共享内存数据结构
 */
typedef struct {
    shared_mem_ctrl_t ctrl;      // 共享内存控制块
} shared_mem_t;

/**
 * @brief 共享内存句柄结构体
 */
typedef struct {
    int shm_fd;               // 共享内存文件描述符
    shared_mem_t *shm_ptr;    // 共享内存映射指针
} shm_handle_t;

/**
 * @brief 创建并初始化共享内存
 * @param handle 共享内存句柄指针
 * @return 成功返回0，失败返回-1
 */
int shm_create(shm_handle_t *handle);

/**
 * @brief 打开已存在的共享内存
 * @param handle 共享内存句柄指针
 * @return 成功返回0，失败返回-1
 */
int shm_open_existing(shm_handle_t *handle);

/**
 * @brief 关闭并销毁共享内存
 * @param handle 共享内存句柄指针
 */
void shm_destroy(shm_handle_t *handle);

/**
 * @brief 写入数据到共享内存
 * @param handle 共享内存句柄指针
 * @param data 要写入的数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int shm_write(shm_handle_t *handle, const uint8_t *data, size_t len);

/**
 * @brief 从共享内存读取数据
 * @param handle 共享内存句柄指针
 * @param data 数据接收缓冲区指针
 * @param len 缓冲区长度
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回实际读取的数据长度，失败返回-1，超时返回0
 */
ssize_t shm_read(shm_handle_t *handle, uint8_t *data, size_t len, int timeout_ms);

/**
 * @brief 获取共享内存当前数据长度
 * @param handle 共享内存句柄指针
 * @return 数据长度
 */
size_t shm_get_data_len(shm_handle_t *handle);

/**
 * @brief 检查共享内存数据是否就绪
 * @param handle 共享内存句柄指针
 * @return 就绪返回true，否则返回false
 */
bool shm_is_ready(shm_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* SHARED_MEMORY_H */
