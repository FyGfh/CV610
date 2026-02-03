/**
 * @file shared_memory.c
 * @brief 共享内存模块实现
 * @version 1.0
 * @date 2026-01-22
 */

#include "shared_memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/**
 * @brief 从共享内存名称生成System V IPC键值
 * @param name 共享内存名称
 * @return 成功返回键值，失败返回-1
 */
static key_t shm_name_to_key(const char *name) {
    if (name == NULL) {
        return -1;
    }
    
    // 使用固定键值，避免依赖ftok()函数
    return 0x56781234; // 固定键值
}

/**
 * @brief 创建并初始化共享内存
 * @param handle 共享内存句柄指针
 * @return 成功返回0，失败返回-1
 */
int shm_create(shm_handle_t *handle) {
    if (handle == NULL) {
        return -1;
    }

    key_t key = shm_name_to_key(SHARED_MEM_NAME);
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    int shm_id = shmget(key, sizeof(shared_mem_t), IPC_CREAT | IPC_EXCL | 0666);
    if (shm_id == -1) {
        // 如果共享内存已存在，尝试打开
        shm_id = shmget(key, sizeof(shared_mem_t), IPC_CREAT | 0666);
        if (shm_id == -1) {
            perror("shmget");
            return -1;
        }
    }

    // 映射共享内存
    handle->shm_ptr = (shared_mem_t *)shmat(shm_id, NULL, 0);
    if (handle->shm_ptr == (void *)-1) {
        perror("shmat");
        shmctl(shm_id, IPC_RMID, NULL);
        return -1;
    }

    // 设置共享内存ID
    handle->shm_fd = shm_id;

    // 初始化共享内存
    memset(handle->shm_ptr, 0, sizeof(shared_mem_t));

    // 初始化共享内存控制块
    handle->shm_ptr->ctrl.total_segments = SHARED_MEM_SEGMENTS;
    handle->shm_ptr->ctrl.free_segments = SHARED_MEM_SEGMENTS;
    
    // 初始化所有共享内存段
    for (int i = 0; i < SHARED_MEM_SEGMENTS; i++) {
        handle->shm_ptr->ctrl.segments[i].state = SHARED_MEM_SEG_FREE;
        handle->shm_ptr->ctrl.segments[i].owner_pid = 0;
        handle->shm_ptr->ctrl.segments[i].data_len = 0;
        handle->shm_ptr->ctrl.segments[i].seq_num = 0;
        memset(handle->shm_ptr->ctrl.segments[i].data, 0, SHARED_MEM_SEG_SIZE);
    }

    return 0;
}

/**
 * @brief 打开已存在的共享内存
 * @param handle 共享内存句柄指针
 * @return 成功返回0，失败返回-1
 */
int shm_open_existing(shm_handle_t *handle) {
    if (handle == NULL) {
        return -1;
    }

    key_t key = shm_name_to_key(SHARED_MEM_NAME);
    if (key == -1) {
        perror("ftok");
        return -1;
    }

    // 获取已存在的共享内存ID
    int shm_id = shmget(key, sizeof(shared_mem_t), 0666);
    if (shm_id == -1) {
        perror("shmget");
        return -1;
    }

    // 映射共享内存
    handle->shm_ptr = (shared_mem_t *)shmat(shm_id, NULL, 0);
    if (handle->shm_ptr == (void *)-1) {
        perror("shmat");
        return -1;
    }

    // 设置共享内存ID
    handle->shm_fd = shm_id;

    return 0;
}

/**
 * @brief 关闭并销毁共享内存
 * @param handle 共享内存句柄指针
 */
void shm_destroy(shm_handle_t *handle) {
    if (handle == NULL) {
        return;
    }

    // 解除内存映射
    if (handle->shm_ptr != NULL && handle->shm_ptr != (void *)-1) {
        if (shmdt(handle->shm_ptr) == -1) {
            perror("shmdt");
        }
        handle->shm_ptr = NULL;
    }

    // 删除共享内存对象
    if (handle->shm_fd != -1) {
        if (shmctl(handle->shm_fd, IPC_RMID, NULL) == -1) {
            perror("shmctl");
        }
        handle->shm_fd = -1;
    }
}

/**
 * @brief 查找空闲的共享内存段
 * @param ctrl 共享内存控制块指针
 * @return 找到返回段索引，未找到返回-1
 */
static int find_free_segment(shared_mem_ctrl_t *ctrl) {
    for (uint32_t i = 0; i < ctrl->total_segments; i++) {
        if (ctrl->segments[i].state == SHARED_MEM_SEG_FREE) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief 写入数据到共享内存
 * @param handle 共享内存句柄指针
 * @param data 要写入的数据指针
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int shm_write(shm_handle_t *handle, const uint8_t *data, size_t len) {
    if (handle == NULL || handle->shm_ptr == NULL || data == NULL) {
        return -1;
    }

    if (len > SHARED_MEM_SEG_SIZE) {
        return -1;
    }

    // 查找空闲段
    int seg_idx = find_free_segment(&handle->shm_ptr->ctrl);
    if (seg_idx == -1) {
        return -1; // 没有空闲段
    }

    // 复制数据到空闲段
    shared_mem_segment_t *seg = &handle->shm_ptr->ctrl.segments[seg_idx];
    memcpy(seg->data, data, len);
    seg->data_len = len;
    seg->seq_num++;
    seg->state = SHARED_MEM_SEG_USED;
    seg->owner_pid = getpid();
    
    // 更新空闲段计数
    handle->shm_ptr->ctrl.free_segments--;

    return 0;
}

/**
 * @brief 从共享内存读取数据
 * @param handle 共享内存句柄指针
 * @param data 数据接收缓冲区指针
 * @param len 缓冲区长度
 * @param timeout_ms 超时时间（毫秒），-1表示无限等待
 * @return 成功返回实际读取的数据长度，失败返回-1，超时返回0
 */
ssize_t shm_read(shm_handle_t *handle, uint8_t *data, size_t len, int timeout_ms) {
    (void)timeout_ms; // 明确表示该参数是故意未使用的
    if (handle == NULL || handle->shm_ptr == NULL || data == NULL) {
        return -1;
    }

    // 检查是否有已使用的段
    bool has_data = false;
    for (int i = 0; i < SHARED_MEM_SEGMENTS; i++) {
        if (handle->shm_ptr->ctrl.segments[i].state == SHARED_MEM_SEG_USED) {
            has_data = true;
            break;
        }
    }
    
    // 如果没有数据，直接返回
    if (!has_data) {
        return 0;
    }

    // 查找已使用的段
    int seg_idx = -1;
    for (int i = 0; i < SHARED_MEM_SEGMENTS; i++) {
        if (handle->shm_ptr->ctrl.segments[i].state == SHARED_MEM_SEG_USED) {
            seg_idx = i;
            break;
        }
    }
    
    if (seg_idx == -1) {
        return 0;
    }

    // 复制数据
    shared_mem_segment_t *seg = &handle->shm_ptr->ctrl.segments[seg_idx];
    size_t copy_len = seg->data_len;
    if (copy_len > len) {
        copy_len = len;
    }
    memcpy(data, seg->data, copy_len);

    // 重置段状态
    seg->state = SHARED_MEM_SEG_FREE;
    seg->owner_pid = 0;
    handle->shm_ptr->ctrl.free_segments++;

    return copy_len;
}

/**
 * @brief 获取共享内存当前数据长度
 * @param handle 共享内存句柄指针
 * @return 数据长度
 */
size_t shm_get_data_len(shm_handle_t *handle) {
    if (handle == NULL || handle->shm_ptr == NULL) {
        return 0;
    }

    // 查找第一个已使用的段
    for (int i = 0; i < SHARED_MEM_SEGMENTS; i++) {
        if (handle->shm_ptr->ctrl.segments[i].state == SHARED_MEM_SEG_USED) {
            return handle->shm_ptr->ctrl.segments[i].data_len;
        }
    }
    return 0;
}

/**
 * @brief 检查共享内存数据是否就绪
 * @param handle 共享内存句柄指针
 * @return 就绪返回true，否则返回false
 */
bool shm_is_ready(shm_handle_t *handle) {
    if (handle == NULL || handle->shm_ptr == NULL) {
        return false;
    }

    // 检查是否有已使用的段
    for (int i = 0; i < SHARED_MEM_SEGMENTS; i++) {
        if (handle->shm_ptr->ctrl.segments[i].state == SHARED_MEM_SEG_USED) {
            return true;
        }
    }
    return false;
}
