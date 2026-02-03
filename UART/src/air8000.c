/**
 * @file air8000.c
 * @brief Air8000 高级 API 实现 (多线程异步架构)
 * 
 * 该文件是 Air8000 SDK 的核心实现，采用了多线程异步架构，主要功能包括：
 * - 上下文管理（创建、销毁、单例模式）
 * - 请求处理（创建、发送、等待响应、超时处理）
 * - I/O 线程（自动重连、数据收发、帧解析）
 * - 业务命令实现（系统命令、电机控制、设备控制、传感器读取等）
 * - 线程安全设计（互斥锁、条件变量）
 * - 自动重连机制
 * 
 * 代码架构设计：
 * 1. **分层设计**：采用了清晰的分层架构，包括高级API层、协议层和串口抽象层
 * 2. **多线程模型**：
 *    - 主线程：处理API调用，创建请求并等待响应
 *    - I/O线程：负责串口通信、自动重连、帧解析和响应处理
 * 3. **异步通信**：使用非阻塞I/O和事件驱动模型，提高系统响应性
 * 4. **线程安全**：通过互斥锁和条件变量确保多线程环境下的安全访问
 * 5. **请求响应模型**：采用基于序列号的请求-响应机制，支持超时处理
 * 6. **模块化设计**：将不同功能（系统命令、电机控制、设备控制等）分离为独立模块
 */

#include "air8000.h"
#include "air8000_file_transfer.h"
#include "air8000_fota.h"
#include "air8000_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>

/**
 * @brief 接收缓冲区大小
 * @note 足够大以容纳多个完整帧，避免频繁内存分配
 */
#define MAX_RX_BUFFER 4096

/**
 * @brief 发送缓冲区大小
 * @note 足够大以容纳单个完整帧
 */
#define MAX_TX_BUFFER 1024

/**
 * @brief 自动重连间隔（毫秒）
 * @note 当串口连接断开时，每隔此时间尝试重新连接
 */
#define RECONNECT_INTERVAL_MS 1000

/**
 * @brief I/O 线程轮询间隔（毫秒）
 * @note 控制 I/O 线程的休眠时间，平衡响应速度和 CPU 占用
 */
#define IO_THREAD_POLL_MS 10

// ==================== 全局单例变量 ====================

/**
 * @brief Air8000 全局单例实例
 * @note 用于实现单例模式，支持全局访问
 */
static air8000_t *g_air8000_instance = NULL;

/**
 * @brief 单例互斥锁
 * @note 保护 g_air8000_instance 的访问，确保线程安全
 */
static pthread_mutex_t g_instance_mutex = PTHREAD_MUTEX_INITIALIZER;

// ==================== 数据结构定义 ====================

/**
 * @brief 请求状态枚举
 * @details 描述请求在生命周期中的不同状态
 */
typedef enum {
    REQ_STATE_PENDING,   /**< 请求待处理（未发送或已发送等待响应） */
    REQ_STATE_COMPLETED, /**< 请求已完成（成功收到响应） */
    REQ_STATE_TIMEOUT,   /**< 请求超时 */
    REQ_STATE_ERROR      /**< 请求出错 */
} request_state_t;

/**
 * @brief 请求对象结构
 * @details 封装了一次请求-响应的完整上下文信息
 */
typedef struct request_s {
    air8000_frame_t req_frame;      /**< 请求帧数据 */
    air8000_frame_t *resp_frame;    /**< 响应帧指针，用于回填响应数据 */
    
    // 同步控制
    pthread_mutex_t mutex;          /**< 保护请求状态的互斥锁 */
    pthread_cond_t cond;            /**< 用于等待响应的条件变量 */
    request_state_t state;          /**< 请求当前状态 */
    int result_code;                /**< 请求结果码 (0=OK, <0=Error) */
    bool sent;                      /**< 请求是否已发送 */
    
    uint64_t timeout_ms;            /**< 请求超时时间（毫秒） */
    uint64_t start_time;            /**< 请求开始时间（毫秒时间戳） */
    
    struct request_s *next;         /**< 链表下一个请求 */
} request_t;

/**
 * @brief Air8000 上下文对象结构
 * @details 封装了与 Air8000 设备通信的所有状态和资源
 */
struct air8000_s {
    air8000_serial_t serial;        /**< 串口对象 */
    char device_path[256];          /**< 设备路径 */
    
    // 线程相关
    pthread_t io_thread;            /**< I/O 线程 ID */
    bool running;                   /**< I/O 线程运行标志 */
    bool connected;                 /**< 串口连接状态 */
    
    pthread_mutex_t ctx_mutex;      /**< 上下文互斥锁（保护 pending_list 和 connected） */
    
    request_t *pending_list;        /**< 待处理请求链表（发送队列 + 等待队列） */
    request_t *pending_map[256];    /**< 请求映射表（用于 O(1) 查找，索引为 seq） */
    
    // 回调函数
    air8000_notify_cb_t notify_cb;  /**< 通知回调函数 */
    void *notify_user_data;         /**< 回调函数用户数据 */
    
    // 接收缓冲
    uint8_t rx_buffer[MAX_RX_BUFFER]; /**< 接收缓冲区 */
    size_t rx_len;                  /**< 接收缓冲区中有效数据长度 */
};

// ==================== 辅助函数声明 ====================

/**
 * @brief I/O 线程函数
 * @param arg 上下文指针
 * @return NULL
 */
static void *io_thread_func(void *arg);

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 当前时间戳
 */
static uint64_t get_time_ms();

// static void cleanup_request(request_t *req);

// ==================== 初始化与销毁 ====================

/**
 * @brief 初始化 Air8000 上下文
 * @param device_path 设备路径 (如 "/dev/ttyACM2")，NULL 表示使用默认路径
 * @return 成功返回上下文指针，失败返回 NULL
 * @details 该函数完成以下工作：
 * 1. 分配上下文内存
 * 2. 初始化上下文成员
 * 3. 设置设备路径（支持默认路径）
 * 4. 初始化互斥锁
 * 5. 尝试打开串口（非阻塞，允许后台重连）
 * 6. 启动 I/O 线程
 */
air8000_t* air8000_init(const char *device_path) {
    air8000_t *ctx = (air8000_t *)malloc(sizeof(air8000_t));
    if (!ctx) return NULL;

    memset(ctx, 0, sizeof(air8000_t));
    
    // 使用默认串口路径 (如果device_path为NULL)
    const char *path = device_path ? device_path : DEFAULT_SERIAL_PATH;
    snprintf(ctx->device_path, sizeof(ctx->device_path), "%s", path);
    
    // 初始化上下文互斥锁
    pthread_mutex_init(&ctx->ctx_mutex, NULL);
    
    // 初始尝试打开串口 (不强制成功，允许后台重连)
    if (air8000_serial_open(&ctx->serial, path) == 0) {
        ctx->connected = true;
        log_info("air8000", "Initial connection successful to %s", path);
    } else {
        ctx->connected = false;
        log_warn("air8000", "Initial connection failed to %s, will retry in background.", path);
        // 确保serial.fd为-1，避免后续操作使用无效文件描述符
        ctx->serial.fd = -1;
    }
    
    // 启动 I/O 线程
    ctx->running = true;
    if (pthread_create(&ctx->io_thread, NULL, io_thread_func, ctx) != 0) {
        perror("pthread_create");
        air8000_serial_close(&ctx->serial);
        pthread_mutex_destroy(&ctx->ctx_mutex);
        free(ctx);
        return NULL;
    }

    // 初始化文件传输模块
    air8000_file_transfer_init(ctx);
    
    // FOTA升级模块现在按需初始化，不需要在这里调用

    return ctx;
}

/**
 * @brief 销毁 Air8000 上下文
 * @param ctx 上下文指针
 * @details 该函数完成以下工作：
 * 1. 停止 I/O 线程
 * 2. 关闭串口
 * 3. 清理所有待处理请求（唤醒等待线程并设置错误状态）
 * 4. 销毁互斥锁
 * 5. 释放上下文内存
 */
void air8000_deinit(air8000_t *ctx) {
    if (ctx) {
        // 停止 I/O 线程
        ctx->running = false;
        pthread_join(ctx->io_thread, NULL);
        
        // 销毁文件传输模块
        air8000_file_transfer_deinit();
        
        // FOTA升级模块现在按需销毁，不需要在这里调用
        
        // 清理串口资源
        air8000_serial_close(&ctx->serial);
        
        // 清理所有待处理请求
        pthread_mutex_lock(&ctx->ctx_mutex);
        request_t *curr = ctx->pending_list;
        while (curr) {
            request_t *next = curr->next;
            // 唤醒等待者并设置错误状态
            pthread_mutex_lock(&curr->mutex);
            curr->state = REQ_STATE_ERROR;
            curr->result_code = AIR8000_ERR_SHUTDOWN; // 系统正在关闭
            pthread_cond_signal(&curr->cond);
            pthread_mutex_unlock(&curr->mutex);
            curr = next;
        }
        pthread_mutex_unlock(&ctx->ctx_mutex);
        
        // 销毁互斥锁
        pthread_mutex_destroy(&ctx->ctx_mutex);
        // 释放上下文内存
        free(ctx);
    }
}

/**
 * @brief 设置通知回调函数
 * @param ctx 上下文指针
 * @param cb 回调函数指针
 * @param user_data 回调函数用户数据
 * @details 当接收到 NOTIFY 类型的帧时，SDK 会调用该回调函数
 */
void air8000_set_notify_callback(air8000_t *ctx, air8000_notify_cb_t cb, void *user_data) {
    if (ctx) {
        pthread_mutex_lock(&ctx->ctx_mutex);
        ctx->notify_cb = cb;
        ctx->notify_user_data = user_data;
        pthread_mutex_unlock(&ctx->ctx_mutex);
    }
}

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 当前时间戳
 * @details 使用 CLOCK_MONOTONIC 时钟源，确保时间的单调性
 */
static uint64_t get_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ==================== 请求处理逻辑 ====================

/**
 * @brief 创建请求对象
 * @param req 请求帧指针
 * @param resp 响应帧指针（用于回填响应数据，可为NULL）
 * @param timeout_ms 请求超时时间（毫秒）
 * @return 成功返回请求对象指针，失败返回NULL
 * @details 该函数完成以下工作：
 * 1. 分配请求对象内存
 * 2. 深拷贝请求帧数据
 * 3. 初始化请求状态和同步原语
 * 4. 设置超时时间和开始时间
 */
static request_t* create_request(const air8000_frame_t *req, air8000_frame_t *resp, int timeout_ms) {
    request_t *r = (request_t *)malloc(sizeof(request_t));
    if (!r) return NULL;
    
    memset(r, 0, sizeof(request_t));
    
    // 深拷贝请求数据
    r->req_frame = *req;
    r->req_frame.data = NULL; // 先初始化为NULL，避免后续free错误
    if (req->data_len > 0 && req->data) {
        r->req_frame.data = malloc(req->data_len);
        if (r->req_frame.data) {
            memcpy(r->req_frame.data, req->data, req->data_len);
        } else {
            free(r);
            return NULL;
        }
    } else if (req->data_len > 0) {
        // 修复：当data_len > 0但data为NULL时，将data_len设为0，避免创建请求失败
        r->req_frame.data_len = 0;
    }
    
    r->resp_frame = resp;
    r->timeout_ms = timeout_ms;
    r->start_time = get_time_ms();
    r->state = REQ_STATE_PENDING;
    r->sent = false;
    
    // 初始化同步原语
    pthread_mutex_init(&r->mutex, NULL);
    pthread_cond_init(&r->cond, NULL);
    
    return r;
}

/**
 * @brief 销毁请求对象
 * @param req 请求对象指针
 * @details 该函数完成以下工作：
 * 1. 销毁互斥锁和条件变量
 * 2. 释放请求帧数据内存
 * 3. 释放请求对象内存
 */
static void destroy_request(request_t *req) {
    if (req) {
        // 销毁同步原语
        pthread_mutex_destroy(&req->mutex);
        pthread_cond_destroy(&req->cond);
        
        // 释放请求帧数据
        if (req->req_frame.data) {
            free(req->req_frame.data);
        }
        
        // 释放请求对象内存
        free(req);
    }
}

/**
 * @brief 发送请求并等待响应
 * @param ctx 上下文指针
 * @param req 请求帧指针
 * @param resp 响应帧指针（用于回填响应数据，可为NULL）
 * @param timeout_ms 请求超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 该函数完成以下工作：
 * 1. 参数校验
 * 2. 检查序列号冲突
 * 3. 创建请求对象
 * 4. 将请求加入待处理队列和映射表
 * 5. 等待请求完成（阻塞直到收到响应或超时）
 * 6. 从队列中移除请求
 * 7. 销毁请求对象并返回结果
 */
int air8000_send_and_wait(air8000_t *ctx, const air8000_frame_t *req, air8000_frame_t *resp, int timeout_ms) {
    if (!ctx || !req) return AIR8000_ERR_PARAM;
    
    // 检查序列号冲突 (Fix 7)
    pthread_mutex_lock(&ctx->ctx_mutex);
    request_t *p = ctx->pending_list;
    while (p) {
        if (p->req_frame.seq == req->seq) {
            pthread_mutex_unlock(&ctx->ctx_mutex);
            return AIR8000_ERR_BUSY; // 序列号冲突
        }
        p = p->next;
    }
    pthread_mutex_unlock(&ctx->ctx_mutex);

    // 创建请求对象
    request_t *r = create_request(req, resp, timeout_ms);
    if (!r) return AIR8000_ERR_NOMEM;
    
    // 加入队列和映射表
    pthread_mutex_lock(&ctx->ctx_mutex);
    r->next = ctx->pending_list;
    ctx->pending_list = r;
    ctx->pending_map[req->seq] = r; // 添加到映射表（用于O(1)查找）
    pthread_mutex_unlock(&ctx->ctx_mutex);
    
    // 等待结果
    pthread_mutex_lock(&r->mutex);
    while (r->state == REQ_STATE_PENDING) {
        pthread_cond_wait(&r->cond, &r->mutex);
    }
    int result = r->result_code;
    pthread_mutex_unlock(&r->mutex);
    
    // 从队列中移除 (注意：必须在持有 ctx_mutex 下操作链表)
    pthread_mutex_lock(&ctx->ctx_mutex);
    request_t **curr = &ctx->pending_list;
    while (*curr) {
        if (*curr == r) {
            *curr = r->next; // 摘除请求
            break;
        }
        curr = &(*curr)->next;
    }
    pthread_mutex_unlock(&ctx->ctx_mutex);
    
    // 销毁请求对象
    destroy_request(r);
    return result;
}

// ==================== I/O 线程 ====================

/**
 * @brief I/O 线程函数
 * @param arg 上下文指针
 * @return NULL
 * @details 该函数是 SDK 的核心运行线程，完成以下工作：
 * 1. 自动重连管理（当串口断开时定期尝试重新连接）
 * 2. 请求发送（遍历待处理队列，发送未发送的请求）
 * 3. 数据接收（从串口读取数据并缓存）
 * 4. 帧解析（从接收缓冲区解析完整帧）
 * 5. 响应处理（匹配响应与请求，唤醒等待线程）
 * 6. 请求超时检查（定期检查请求是否超时）
 * 
 * 线程运行在一个无限循环中，直到 ctx->running 被设置为 false
 */
static void *io_thread_func(void *arg) {
    air8000_t *ctx = (air8000_t *)arg;
    uint64_t last_reconnect_time = 0;
    uint8_t tx_buf[MAX_TX_BUFFER];
    
    while (ctx->running) {
        uint64_t now = get_time_ms();
        
        // 1. 自动重连逻辑
        if (!ctx->connected) {
            if (now - last_reconnect_time > RECONNECT_INTERVAL_MS) {
                last_reconnect_time = now;
                // 尝试重连
                if (air8000_serial_open(&ctx->serial, ctx->device_path) == 0) {
                    pthread_mutex_lock(&ctx->ctx_mutex);
                    ctx->connected = true;
                    pthread_mutex_unlock(&ctx->ctx_mutex);
                    log_info("air8000", "Reconnected to %s", ctx->device_path);
                }
            }
            
            // 未连接时，检查所有 Pending 请求是否超时
            pthread_mutex_lock(&ctx->ctx_mutex);
            request_t *curr = ctx->pending_list;
            // 修复：重新获取当前时间，避免使用旧的now值导致误判
            uint64_t current_time = get_time_ms();
            while (curr) {
                if (curr->state == REQ_STATE_PENDING && (current_time - curr->start_time > curr->timeout_ms)) {
                    // 从映射表中移除
                    ctx->pending_map[curr->req_frame.seq] = NULL;
                    
                    pthread_mutex_lock(&curr->mutex);
                    curr->state = REQ_STATE_TIMEOUT;
                    curr->result_code = AIR8000_ERR_TIMEOUT; // Timeout
                    pthread_cond_signal(&curr->cond);
                    pthread_mutex_unlock(&curr->mutex);
                }
                curr = curr->next;
            }
            pthread_mutex_unlock(&ctx->ctx_mutex);
            
            usleep(100000); // 100ms sleep
            continue;
        }
        
        // 2. 发送逻辑 (遍历链表，找到未发送的请求)
        pthread_mutex_lock(&ctx->ctx_mutex);
        request_t *curr = ctx->pending_list;
        if (curr) {
            while (curr) {
                if (curr->state == REQ_STATE_PENDING && !curr->sent) {
                    // 尝试发送
                    int encoded_len = air8000_frame_encode(&curr->req_frame, tx_buf, sizeof(tx_buf));
                    if (encoded_len > 0) {
                        int written = air8000_serial_write(&ctx->serial, tx_buf, encoded_len);
                        if (written > 0) {
                            curr->sent = true;
                            // 打印发送数据
                            log_info("air8000", "Sent CMD: 0x%04X, Len: %d", curr->req_frame.cmd, encoded_len);
                        } else {
                            // 发送失败，可能是断开连接，标记连接状态为断开
                            log_error("air8000", "Serial write failed, disconnecting...");
                            ctx->connected = false;
                            air8000_serial_close(&ctx->serial);
                            break; // 跳出当前循环，下一次迭代会处理重连
                        }
                    } else {
                        log_error("air8000", "Frame encode failed - len=%d", encoded_len);
                    }
                }
                curr = curr->next;
            }
        }
        pthread_mutex_unlock(&ctx->ctx_mutex);
        
        // 3. 接收逻辑
        int read_len = air8000_serial_read(&ctx->serial, 
                                          &ctx->rx_buffer[ctx->rx_len], 
                                          MAX_RX_BUFFER - ctx->rx_len, 
                                          IO_THREAD_POLL_MS);
                                          
        if (read_len < 0) {
            // 读取错误 (断开连接)
            log_error("air8000", "Serial read error, disconnecting...");
            pthread_mutex_lock(&ctx->ctx_mutex);
            ctx->connected = false;
            air8000_serial_close(&ctx->serial);
            pthread_mutex_unlock(&ctx->ctx_mutex);
            // 错误后不继续处理
        } else if (read_len > 0) {
            ctx->rx_len += read_len;
            
            // 解析帧
            while (ctx->rx_len > 0) {
                air8000_frame_t frame;
                air8000_frame_init(&frame); // 必须初始化，否则 frame.data 是随机值，导致 air8000_frame_parse 中 free 崩溃
                
                int frame_len = air8000_frame_parse(ctx->rx_buffer, ctx->rx_len, &frame);
                
                if (frame_len > 0) {
                    // 收到完整帧
                    if (frame.type == FRAME_TYPE_NOTIFY) {
                        // 复制回调信息到局部变量，避免持有锁调用回调
                        air8000_notify_cb_t cb = NULL;
                        void *ud = NULL;

                        pthread_mutex_lock(&ctx->ctx_mutex);
                        cb = ctx->notify_cb;
                        ud = ctx->notify_user_data;
                        pthread_mutex_unlock(&ctx->ctx_mutex);

                        if (cb) {
                            cb(&frame, ud);
                        }
                        // 释放NOTIFY帧的数据
                        air8000_frame_cleanup(&frame);
                    } else {
                        // 检查是否是文件传输或FOTA相关请求
                        if (frame.type == FRAME_TYPE_REQUEST) {
                            // 处理文件传输请求
                            air8000_file_transfer_handle_request(ctx, &frame);
                            // FOTA升级请求现在由应用层处理，不需要在这里调用
                        } else {
                            // 打印接收数据
                            log_info("air8000", "Received CMD: 0x%04X, Len: %d", frame.cmd, frame_len);
                            
                            // 翻译后的数据打印
                            if (frame.data_len > 0 && frame.data) {
                                switch (frame.cmd) {
                                    case CMD_SYS_VERSION:
                                        {
                                            air8000_version_t ver;
                                            if (air8000_parse_version(frame.data, frame.data_len, &ver) == 0) {
                                                printf("[PARSED] Version: V%d.%d.%d (%s)\n", ver.major, ver.minor, ver.patch, ver.build);
                                            }
                                        }
                                        break;
                                    case CMD_SENSOR_READ_TEMP:
                                        {
                                            float temp;
                                            if (air8000_parse_motor_float_resp(frame.data, frame.data_len, NULL, &temp) == 0) {
                                                printf("[PARSED] Temperature: %.2f C\n", temp);
                                            }
                                        }
                                        break;
                                    case CMD_SENSOR_READ_ALL:
                                        {
                                            air8000_sensor_data_t sensor_data;
                                            if (air8000_parse_sensor_data(frame.data, frame.data_len, &sensor_data) == 0) {
                                                printf("[PARSED] All Sensors - Temp: %.2f C, Humidity: %d%%, Light: %d, Battery: %d%%\n", 
                                                       sensor_data.temperature, sensor_data.humidity, sensor_data.light, sensor_data.battery);
                                            }
                                        }
                                        break;
                                    case CMD_QUERY_POWER:
                                        {
                                            air8000_power_adc_t power;
                                            if (air8000_parse_power_adc(frame.data, frame.data_len, &power) == 0) {
                                                printf("[PARSED] Power - 12V: %.2f V, Battery: %.2f V\n", 
                                                       power.v12_mv / 1000.0, power.vbat_mv / 1000.0);
                                            }
                                        }
                                        break;
                                    case CMD_QUERY_NETWORK:
                                        {
                                            air8000_network_status_t net;
                                            if (air8000_parse_network_status(frame.data, frame.data_len, &net) == 0) {
                                                printf("[PARSED] Network - CSQ: %d, RSSI: %d, RSRP: %d, Status: %d\n", 
                                                       net.csq, net.rssi, net.rsrp, net.status);
                                            }
                                        }
                                        break;
                                    default:
                                        // 其他命令暂不打印详细解析
                                        break;
                                }
                            }
                            
                            // 查找匹配的请求 (Fix 10: O(1) lookup)
                            request_t *req = ctx->pending_map[frame.seq];

                            if (req && req->state == REQ_STATE_PENDING && req->req_frame.cmd == frame.cmd) {
                         // 匹配成功
                         if (req->resp_frame) {
                             // 手动复制响应帧字段，避免浅拷贝导致的双重释放
                             req->resp_frame->version = frame.version;
                             req->resp_frame->type = frame.type;
                             req->resp_frame->seq = frame.seq;
                             req->resp_frame->cmd = frame.cmd;
                             req->resp_frame->data_len = frame.data_len;
                               
                             // 深拷贝数据
                             if (frame.data_len > 0 && frame.data) {
                                 req->resp_frame->data = malloc(frame.data_len);
                                 if (req->resp_frame->data) {
                                     memcpy(req->resp_frame->data, frame.data, frame.data_len);
                                 } else {
                                     // 修复：内存分配失败时，保持data_len不变，只将data设为NULL
                                     req->resp_frame->data = NULL;
                                 }
                             } else {
                                 req->resp_frame->data = NULL;
                             }
                         }

                         // 从映射表中移除
                         ctx->pending_map[frame.seq] = NULL;

                         pthread_mutex_lock(&req->mutex);
                         req->state = REQ_STATE_COMPLETED;
                         req->result_code = AIR8000_OK;
                         pthread_cond_signal(&req->cond);
                         pthread_mutex_unlock(&req->mutex);
                    }
                        }
                        // 释放非NOTIFY帧的数据
                        air8000_frame_cleanup(&frame);
                    }
                    
                    // 移除缓冲区数据
                    memmove(ctx->rx_buffer, &ctx->rx_buffer[frame_len], ctx->rx_len - frame_len);
                    ctx->rx_len -= frame_len;
                    
                } else if (frame_len == -2) {
                    // 无效头
                    memmove(ctx->rx_buffer, &ctx->rx_buffer[1], ctx->rx_len - 1);
                    ctx->rx_len--;
                } else {
                    break; // 不完整
                }
            }
        } else if (read_len < 0) {
            // 读取错误 (断开连接)
            printf("Air8000: Serial read error, disconnecting...\n");
            pthread_mutex_lock(&ctx->ctx_mutex);
            ctx->connected = false;
            air8000_serial_close(&ctx->serial);
            pthread_mutex_unlock(&ctx->ctx_mutex);
        }
        
        // 4. 超时检查
        pthread_mutex_lock(&ctx->ctx_mutex);
        curr = ctx->pending_list;
        // 修复：重新获取当前时间，保持与未连接状态下的检查逻辑一致
        uint64_t current_time = get_time_ms();
        while (curr) {
            if (curr->state == REQ_STATE_PENDING && (current_time - curr->start_time > curr->timeout_ms)) {
                // 从映射表中移除
                ctx->pending_map[curr->req_frame.seq] = NULL;
                
                pthread_mutex_lock(&curr->mutex);
                curr->state = REQ_STATE_TIMEOUT;
                curr->result_code = AIR8000_ERR_TIMEOUT; // Timeout
                pthread_cond_signal(&curr->cond);
                pthread_mutex_unlock(&curr->mutex);
            }
            curr = curr->next;
        }
        pthread_mutex_unlock(&ctx->ctx_mutex);
    }
    
    return NULL;
}

// ==================== 辅助函数 ====================

/**
 * @brief 简化的命令发送函数
 * @param ctx 上下文指针
 * @param cmd 命令码
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 该函数用于发送简单命令（无数据或只需确认响应），完成以下工作：
 * 1. 初始化请求帧和响应帧
 * 2. 设置请求类型、序列号和命令码
 * 3. 发送请求并等待响应
 * 4. 检查响应类型是否为ACK或RESPONSE
 * 5. 释放响应帧资源
 */
static int simple_command(air8000_t *ctx, uint16_t cmd, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = cmd;
    
    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

// ==================== 业务命令实现 ====================

/**
 * @brief PING 测试命令
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 用于测试与设备的连接是否正常
 */
int air8000_ping(air8000_t *ctx, int timeout_ms) {
    return simple_command(ctx, CMD_SYS_PING, timeout_ms);
}

/**
 * @brief 获取设备版本信息
 * @param ctx 上下文指针
 * @param ver 版本信息结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取设备的固件版本信息，包括主版本、次版本、补丁版本和构建号
 */
int air8000_get_version(air8000_t *ctx, air8000_version_t *ver, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_SYS_VERSION;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 3) {
            if (air8000_parse_version(resp.data, resp.data_len, ver) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

/**
 * @brief 系统复位命令
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 用于重启设备系统
 */
int air8000_sys_reset(air8000_t *ctx, int timeout_ms) {
    return simple_command(ctx, CMD_SYS_RESET, timeout_ms);
}

/**
 * @brief 查询网络状态
 * @param ctx 上下文指针
 * @param net 网络状态结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取设备的网络状态信息，包括信号强度、RSSI、RSRP、运营商ID、ICCID和IP地址等
 */
int air8000_query_network(air8000_t *ctx, air8000_network_status_t *net, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_QUERY_NETWORK;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 5) {
            if (air8000_parse_network_status(resp.data, resp.data_len, net) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

/**
 * @brief 查询电源状态
 * @param ctx 上下文指针
 * @param pwr 电源ADC结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取设备的电源状态信息，包括12V电压和电池电压
 */
int air8000_query_power(air8000_t *ctx, air8000_power_adc_t *pwr, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_QUERY_POWER;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 4) {
            if (air8000_parse_power_adc(resp.data, resp.data_len, pwr) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

// ==================== 电机命令 ====================

/**
 * @brief 使能电机
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param mode 控制模式
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 使能指定电机，进入指定控制模式
 */
int air8000_motor_enable(air8000_t *ctx, uint8_t motor_id, int mode, int timeout_ms) {
    uint8_t data[2] = {motor_id, (uint8_t)mode};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_ENABLE;
    req.data = data;
    req.data_len = 2;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

/**
 * @brief 禁用电机
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 禁用指定电机，使其进入自由状态
 */
int air8000_motor_disable(air8000_t *ctx, uint8_t motor_id, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_DISABLE;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

/**
 * @brief 停止电机
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 立即停止指定电机运动
 */
int air8000_motor_stop(air8000_t *ctx, uint8_t motor_id, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    air8000_frame_init(&resp); // 初始化响应帧，确保resp.data为NULL
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_STOP;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    air8000_frame_cleanup(&resp); // 确保无论结果如何都释放资源
    return ret;
}

/**
 * @brief 电机绝对旋转
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param angle 目标角度（弧度）
 * @param velocity 旋转速度（弧度/秒）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 使电机旋转到指定绝对角度
 */
int air8000_motor_rotate(air8000_t *ctx, uint8_t motor_id, float angle, float velocity, int timeout_ms) {
    uint8_t data[9];
    data[0] = motor_id;
    uint32_t a = air8000_htonf(angle);
    uint32_t v = air8000_htonf(velocity);
    memcpy(&data[1], &a, 4);
    memcpy(&data[5], &v, 4);

    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_ROTATE;
    req.data = data;
    req.data_len = 9;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 电机相对旋转
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param angle 旋转角度（弧度）
 * @param velocity 旋转速度（弧度/秒）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 使电机相对于当前位置旋转指定角度
 */
int air8000_motor_rotate_rel(air8000_t *ctx, uint8_t motor_id, float angle, float velocity, int timeout_ms) {
    uint8_t data[9];
    data[0] = motor_id;
    uint32_t a = air8000_htonf(angle);
    uint32_t v = air8000_htonf(velocity);
    memcpy(&data[1], &a, 4);
    memcpy(&data[5], &v, 4);

    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_ROTATE_REL;
    req.data = data;
    req.data_len = 9;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 设置电机速度
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param velocity 目标速度（弧度/秒）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 设置电机的目标速度
 */
int air8000_motor_set_vel(air8000_t *ctx, uint8_t motor_id, float velocity, int timeout_ms) {
    uint8_t data[5];
    data[0] = motor_id;
    uint32_t v = air8000_htonf(velocity);
    memcpy(&data[1], &v, 4);

    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_SET_VEL;
    req.data = data;
    req.data_len = 5;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 设置电机原点
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 将当前位置设为电机原点（0度）
 */
int air8000_motor_set_origin(air8000_t *ctx, uint8_t motor_id, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_SET_ORIGIN;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 获取电机位置
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param pos 位置值指针（弧度）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取电机当前位置
 */
int air8000_motor_get_pos(air8000_t *ctx, uint8_t motor_id, float *pos, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_GET_POS;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 5) {
            if (air8000_parse_motor_float_resp(resp.data, resp.data_len, NULL, pos) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

/**
 * @brief 获取所有电机状态
 * @param ctx 上下文指针
 * @param status 电机状态结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取所有电机的状态信息
 */
int air8000_motor_get_all(air8000_t *ctx, air8000_all_motor_status_t *status, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_GET_ALL;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len > 0) {
            if (air8000_parse_all_motor_status(resp.data, resp.data_len, status) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

/**
 * @brief 释放电机状态内存
 * @param status 电机状态结构体指针
 * @details 释放air8000_motor_get_all分配的内存
 */
void air8000_free_motor_status(air8000_all_motor_status_t *status) {
    if (status && status->motors) {
        free(status->motors);
        status->motors = NULL;
        status->count = 0;
    }
}

/**
 * @brief 读取电机寄存器
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param reg 寄存器地址
 * @param val 寄存器值指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 读取电机控制器的寄存器值
 */
int air8000_motor_read_reg(air8000_t *ctx, uint8_t motor_id, uint8_t reg, float *val, int timeout_ms) {
    uint8_t data[2] = {motor_id, reg};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_READ_REG;
    req.data = data;
    req.data_len = 2;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 6) {
            if (air8000_parse_motor_read_reg(resp.data, resp.data_len, NULL, NULL, val) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

/**
 * @brief 写入电机寄存器
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param reg 寄存器地址
 * @param val 寄存器值
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 写入电机控制器的寄存器值
 */
int air8000_motor_write_reg(air8000_t *ctx, uint8_t motor_id, uint8_t reg, float val, int timeout_ms) {
    uint8_t data[6];
    data[0] = motor_id;
    data[1] = reg;
    uint32_t v = air8000_htonf(val);
    memcpy(&data[2], &v, 4);

    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_WRITE_REG;
    req.data = data;
    req.data_len = 6;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 保存电机参数到Flash
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 将电机参数保存到Flash，断电后不丢失
 */
int air8000_motor_save_flash(air8000_t *ctx, uint8_t motor_id, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_SAVE_FLASH;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 清除电机错误
 * @param ctx 上下文指针
 * @param motor_id 电机ID (1=Y轴, 2=X轴, 3=Z轴, 0xFF=所有)
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 清除电机的错误状态
 */
int air8000_motor_clear_error(air8000_t *ctx, uint8_t motor_id, int timeout_ms) {
    uint8_t data[1] = {motor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_MOTOR_CLEAR_ERROR;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

// ==================== 设备控制命令 ====================

/**
 * @brief 设备控制命令
 * @param ctx 上下文指针
 * @param cmd 命令码（如CMD_DEV_LED, CMD_DEV_FAN等）
 * @param dev_id 设备ID
 * @param state 设备状态（0=关, 1=开, 2=闪烁）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 用于控制各种设备的开关状态
 */
int air8000_device_control(air8000_t *ctx, air8000_command_t cmd, uint8_t dev_id, uint8_t state, int timeout_ms) {
    uint8_t data[2] = {dev_id, state};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = cmd;
    req.data = data;
    req.data_len = 2;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 电机电源控制
 * @param ctx 上下文指针
 * @param on 电源状态（true=开, false=关）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 控制电机的电源供应
 */
int air8000_motor_power_control(air8000_t *ctx, bool on, int timeout_ms) {
    uint8_t data[1] = {on ? 1 : 0};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_DEV_MOTOR_POWER;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

// ==================== 传感器命令 ====================

/**
 * @brief 读取温度传感器
 * @param ctx 上下文指针
 * @param sensor_id 传感器ID
 * @param temp 温度值指针（摄氏度）
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 读取指定温度传感器的值
 */
int air8000_sensor_read_temp(air8000_t *ctx, uint8_t sensor_id, float *temp, int timeout_ms) {
    uint8_t data[1] = {sensor_id};
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_SENSOR_READ_TEMP;
    req.data = data;
    req.data_len = 1;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 5) {
            if (air8000_parse_motor_float_resp(resp.data, resp.data_len, NULL, temp) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

/**
 * @brief 读取所有传感器数据
 * @param ctx 上下文指针
 * @param data 传感器数据结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 读取所有传感器的数据，包括温度、湿度、光照和电池电量
 */
int air8000_sensor_read_all(air8000_t *ctx, air8000_sensor_data_t *data, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_SENSOR_READ_ALL;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 5) {
            if (air8000_parse_sensor_data(resp.data, resp.data_len, data) == 0) {
                air8000_frame_cleanup(&resp);
                return 0;
            }
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

// ==================== 看门狗命令 ====================

/**
 * @brief 配置看门狗
 * @param ctx 上下文指针
 * @param cfg 看门狗配置结构体
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 配置看门狗的启用状态、超时时间和断电时间
 */
int air8000_wdt_config(air8000_t *ctx, const air8000_wdt_config_t *cfg, int timeout_ms) {
    uint8_t data[4];
    data[0] = cfg->enable ? 1 : 0;
    data[1] = (cfg->timeout_sec >> 8) & 0xFF;
    data[2] = cfg->timeout_sec & 0xFF;
    data[3] = cfg->power_off_sec;

    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_SYS_HB_WDT_CONFIG;
    req.data = data;
    req.data_len = 4;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        bool success = (resp.type == FRAME_TYPE_ACK || resp.type == FRAME_TYPE_RESPONSE);
        air8000_frame_cleanup(&resp);
        return success ? 0 : -1;
    }
    return ret;
}

/**
 * @brief 获取看门狗状态
 * @param ctx 上下文指针
 * @param status 看门狗状态结构体指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 获取看门狗的当前状态，包括启用状态、超时时间、剩余时间和复位次数
 */
int air8000_wdt_status(air8000_t *ctx, air8000_wdt_status_t *status, int timeout_ms) {
    air8000_frame_t req, resp;
    air8000_frame_init(&req);
    req.type = FRAME_TYPE_REQUEST;
    req.seq = air8000_next_seq();
    req.cmd = CMD_SYS_HB_WDT_STATUS;

    int ret = air8000_send_and_wait(ctx, &req, &resp, timeout_ms);
    if (ret == 0) {
        if (resp.type == FRAME_TYPE_RESPONSE && resp.data_len >= 7) {
            if (status) {
                status->enable = resp.data[0] != 0;
                status->timeout_sec = ((uint16_t)resp.data[1] << 8) | resp.data[2];
                status->power_off_sec = resp.data[3];
                status->remaining_sec = ((uint16_t)resp.data[4] << 8) | resp.data[5];
                status->reset_count = resp.data[6];
            }
            air8000_frame_cleanup(&resp);
            return 0;
        }
        air8000_frame_cleanup(&resp);
        return -1;
    }
    return ret;
}

/**
 * @brief 发送看门狗心跳
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间（毫秒）
 * @return 成功返回0，失败返回错误码
 * @details 发送心跳包，重置看门狗计时器
 */
int air8000_wdt_heartbeat(air8000_t *ctx, int timeout_ms) {
    return air8000_ping(ctx, timeout_ms);
}

// ==================== 单例模式实现 ====================

/**
 * @brief 获取 Air8000 全局单例实例
 * @return 全局单例指针，失败返回 NULL
 * @details 实现了线程安全的单例模式，采用双重检查锁定模式：
 * 1. 快速检查实例是否已存在（无锁）
 * 2. 如果不存在，初始化实例
 * 3. 首次调用时会自动初始化，使用默认串口路径
 */
air8000_t* air8000_get_instance(void) {
    air8000_t *instance = NULL;
    
    // 先读取锁，快速检查实例是否已存在
    pthread_mutex_lock(&g_instance_mutex);
    instance = g_air8000_instance;
    pthread_mutex_unlock(&g_instance_mutex);
    
    if (instance != NULL) {
        return instance;
    }
    
    // 实例不存在，需要初始化
    return air8000_init_instance(NULL);
}

/**
 * @brief 使用自定义设备路径初始化或重置全局单例
 * @param device_path 设备路径，NULL 表示使用默认路径
 * @return 全局单例指针，失败返回 NULL
 * @details 该函数完成以下工作：
 * 1. 锁定单例互斥锁
 * 2. 如果实例已存在，先销毁
 * 3. 使用指定设备路径初始化新实例
 * 4. 更新全局单例指针
 * 5. 解锁互斥锁并返回实例
 */
air8000_t* air8000_init_instance(const char *device_path) {
    air8000_t *instance = NULL;
    
    pthread_mutex_lock(&g_instance_mutex);
    
    // 如果实例已存在，先销毁
    if (g_air8000_instance != NULL) {
        air8000_deinit(g_air8000_instance);
        g_air8000_instance = NULL;
    }
    
    // 初始化新实例
    instance = air8000_init(device_path);
    if (instance != NULL) {
        g_air8000_instance = instance;
    }
    
    pthread_mutex_unlock(&g_instance_mutex);
    
    return instance;
}

/**
 * @brief 重置 (销毁) 全局单例实例
 * @details 该函数完成以下工作：
 * 1. 锁定单例互斥锁
 * 2. 如果实例已存在，销毁实例
 * 3. 重置全局单例指针为 NULL
 * 4. 解锁互斥锁
 */
void air8000_reset_instance(void) {
    pthread_mutex_lock(&g_instance_mutex);
    
    if (g_air8000_instance != NULL) {
        air8000_deinit(g_air8000_instance);
        g_air8000_instance = NULL;
    }
    
    pthread_mutex_unlock(&g_instance_mutex);
}
