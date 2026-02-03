/**
 * @file mqtt_client.c
 * @brief MQTT客户端核心实现
 * @version 1.0
 * @date 2026-01-17
 * 
 * 本文件实现了基于Mosquitto库的MQTT客户端核心功能，
 * 专为海思CV610平台优化，提供了完整的MQTT通信能力。
 * 
 * 主要功能包括：
 * - MQTT客户端创建与管理
 * - 连接管理（连接、断开、重连）
 * - 消息发布与订阅
 * - 状态管理与回调
 * - 线程安全操作
 * - 平台特定优化
 * 
 * 设计架构：
 * - 分层设计：API层、实现层、平台适配层
 * - 线程安全：使用互斥锁保护共享资源
 * - 回调机制：支持消息回调和状态回调
 * - 错误处理：详细的错误码和日志
 */

#include "mqtt_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mosquitto.h>
#include <pthread.h>
#include <unistd.h>

// 海思CV610平台特定定义
#define HISEI_CV610_PLATFORM 1

// 平台初始化日志
#if HISEI_CV610_PLATFORM
static const char* PLATFORM_NAME = "HiSilicon CV610";
#else
static const char* PLATFORM_NAME = "Generic ARM";
#endif

/**
 * @brief 平台特定的内存分配函数
 * 针对海思CV610平台优化的内存分配策略
 */
static void* platform_malloc(size_t size)
{
    void* ptr = malloc(size);
    if (ptr != NULL) {
        // 可选：针对海思CV610平台的内存初始化或对齐优化
        memset(ptr, 0, size);
    }
    return ptr;
}

/**
 * @brief 平台特定的内存释放函数
 */
static void platform_free(void* ptr)
{
    if (ptr != NULL) {
        free(ptr);
    }
}

/**
 * @brief 平台特定的字符串复制函数
 */
static char* platform_strdup(const char* str)
{
    if (str == NULL) {
        return NULL;
    }
    
    size_t len = strlen(str) + 1;
    char* dest = (char*)platform_malloc(len);
    if (dest != NULL) {
        strcpy(dest, str);
    }
    return dest;
}

/**
 * @brief 全局日志级别
 */
static mqtt_log_level_t g_log_level = MQTT_LOG_LEVEL_INFO;

/**
 * @brief 日志宏，根据日志级别控制输出
 */
#define MQTT_LOG(level, format, ...) \
    do { \
        if (level <= g_log_level) { \
            printf("MQTT: " format "\n", ##__VA_ARGS__); \
        } \
    } while (0)

/**
 * @brief 消息订阅信息结构体
 */
typedef struct subscription_info {
    char *topic;                        /**< 订阅的主题 */
    mqtt_message_callback_t callback;   /**< 消息回调函数 */
    void *user_data;                    /**< 回调函数用户数据 */
    struct subscription_info *next;     /**< 指向下一个订阅信息的指针 */
} subscription_info_t;

/**
 * @brief MQTT客户端实现结构体
 */
struct mqtt_client_impl {
    struct mosquitto *mosq;           /**< Mosquitto客户端句柄 */
    mqtt_client_config_t config;      /**< 客户端配置 */
    mqtt_client_state_t state;         /**< 当前状态 */
    mqtt_state_callback_t state_cb;    /**< 状态变化回调 */
    void *state_cb_user_data;          /**< 状态变化回调用户数据 */
    subscription_info_t *subscriptions;/**< 订阅信息列表 */
    pthread_mutex_t mutex;             /**< 互斥锁，用于线程安全 */
    int retry_count;                  /**< 当前重连次数 */
    bool auto_reconnect;              /**< 是否自动重连 */
    time_t last_status_print;         /**< 上次打印状态的时间 */
};

/**
 * @brief 静态回调函数：连接状态变化
 */
static void on_connect_callback(struct mosquitto *mosq, void *obj, int reason_code)
{
    (void)mosq;
    mqtt_client_t client = (mqtt_client_t)obj;
    if (client == NULL) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "on_connect_callback called with NULL client");
        return;
    }

    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "on_connect_callback called with reason_code: %d", reason_code);

    pthread_mutex_lock(&client->mutex);
    
    if (reason_code == 0) {
        client->state = MQTT_CLIENT_STATE_CONNECTED;
        client->retry_count = 0;
        MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Successfully connected to broker at %s:%d", 
                 client->config.host, client->config.port);
    } else {
        client->state = MQTT_CLIENT_STATE_DISCONNECTED;
        client->retry_count++;
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to connect to broker, reason: %s (code: %d), retry count: %d", 
                 mosquitto_strerror(reason_code), reason_code, client->retry_count);
    }
    
    // 保存状态回调指针和状态，解锁后再调用，避免死锁
    mqtt_state_callback_t state_cb = client->state_cb;
    void *state_cb_user_data = client->state_cb_user_data;
    mqtt_client_state_t current_state = client->state;
    
    pthread_mutex_unlock(&client->mutex);
    
    // 调用用户注册的状态回调（在锁外调用，避免死锁）
    if (state_cb) {
        MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "Calling user state callback with state: %d", current_state);
        state_cb(current_state, state_cb_user_data);
    } else {
        MQTT_LOG(MQTT_LOG_LEVEL_WARNING, "No state callback registered");
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "on_connect_callback completed");
}

/**
 * @brief 静态回调函数：断开连接
 */
static void on_disconnect_callback(struct mosquitto *mosq, void *obj, int reason_code)
{
    (void)mosq;
    mqtt_client_t client = (mqtt_client_t)obj;
    if (client == NULL) {
        return;
    }

    pthread_mutex_lock(&client->mutex);
    
    client->state = MQTT_CLIENT_STATE_DISCONNECTED;
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Disconnected from broker at %s:%d, reason: %s (code: %d)", 
             client->config.host, client->config.port, 
             reason_code == 0 ? "Normal disconnection" : mosquitto_strerror(reason_code), 
             reason_code);
    
    // 保存状态回调指针和状态，解锁后再调用，避免死锁
    mqtt_state_callback_t state_cb = client->state_cb;
    void *state_cb_user_data = client->state_cb_user_data;
    mqtt_client_state_t current_state = client->state;
    
    pthread_mutex_unlock(&client->mutex);
    
    // 调用用户注册的状态回调（在锁外调用，避免死锁）
    if (state_cb) {
        state_cb(current_state, state_cb_user_data);
    }
}

/**
 * @brief 静态回调函数：消息接收
 */
static void on_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
    (void)mosq;
    mqtt_client_t client = (mqtt_client_t)obj;
    if (client == NULL || message == NULL || message->topic == NULL) {
        return;
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "Received message on topic %s: %.*s", 
           message->topic, (int)message->payloadlen, (char*)message->payload);
    
    // 遍历订阅列表，找到匹配的主题并调用对应的回调函数
    pthread_mutex_lock(&client->mutex);
    
    subscription_info_t *sub = client->subscriptions;
    while (sub != NULL) {
        // 检查主题是否匹配
        if (mosquitto_topic_matches_sub(sub->topic, message->topic, false)) {
            // 调用用户注册的消息回调函数
            if (sub->callback != NULL) {
                sub->callback(message->topic, message->payload, message->payloadlen, sub->user_data);
            }
        }
        sub = sub->next;
    }
    
    pthread_mutex_unlock(&client->mutex);
}

/**
 * @brief 初始化MQTT库
 */
static bool mqtt_lib_init(void)
{
    static bool initialized = false;
    if (!initialized) {
        int rc = mosquitto_lib_init();
        if (rc != MOSQ_ERR_SUCCESS) {
            MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to initialize mosquitto library: %s", mosquitto_strerror(rc));
            return false;
        }
        initialized = true;
    }
    return true;
}

/**
 * @brief 创建MQTT客户端实例
 * 
 * 此函数创建并初始化一个MQTT客户端实例，配置必要的参数和回调函数。
 * 
 * @param config MQTT客户端配置结构体，包含连接参数、认证信息等
 * @return 成功返回MQTT客户端句柄，失败返回NULL
 * 
 * @note 使用流程：
 * 1. 准备mqtt_client_config_t配置结构体
 * 2. 调用此函数创建客户端
 * 3. 设置状态回调（可选）
 * 4. 调用mqtt_client_connect连接服务器
 * 5. 使用完毕后调用mqtt_client_destroy销毁
 * 
 * @example
 * mqtt_client_config_t config = {
 *     .host = "47.107.225.196",
 *     .port = 1883,
 *     .client_id = "device-001",
 *     .username = "",
 *     .password = "",
 *     .keep_alive = 60,
 *     .clean_session = true
 * };
 * mqtt_client_t client = mqtt_client_create(&config);
 */
mqtt_client_t mqtt_client_create(const mqtt_client_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }
    
    // 初始化MQTT库（仅首次调用时生效）
    if (!mqtt_lib_init()) {
        return NULL;
    }
    
    // 创建客户端实例，使用平台特定的内存分配函数
    // platform_malloc会自动初始化内存为0
    mqtt_client_t client = (mqtt_client_t)platform_malloc(sizeof(struct mqtt_client_impl));
    if (client == NULL) {
        return NULL;
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "MQTT client initialized for %s platform", PLATFORM_NAME);
    
    // 复制配置到客户端实例
    memcpy(&client->config, config, sizeof(mqtt_client_config_t));
    
    // 初始化互斥锁，用于线程安全操作
    pthread_mutex_init(&client->mutex, NULL);
    
    // 初始化订阅列表为空
    client->subscriptions = NULL;
    
    // 设置默认值（如果配置中未指定）
    if (client->config.keep_alive == 0) {
        client->config.keep_alive = 60; // 默认心跳间隔60秒
    }
    
    if (client->config.connect_timeout_ms == 0) {
        client->config.connect_timeout_ms = 5000; // 默认连接超时5秒
    }
    
    if (client->config.retry_interval_ms == 0) {
        client->config.retry_interval_ms = 2000; // 默认重连间隔2秒
    }
    
    // 创建底层mosquitto客户端实例
    // 参数说明：
    // 1. client_id: 客户端唯一标识
    // 2. clean_session: 是否清除会话
    // 3. obj: 用户数据，传递给回调函数
    client->mosq = mosquitto_new(config->client_id, config->clean_session, client);
    if (client->mosq == NULL) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to create mosquitto client");
        platform_free(client);
        return NULL;
    }
    
    // 设置回调函数
    mosquitto_connect_callback_set(client->mosq, on_connect_callback);       // 连接回调
    mosquitto_disconnect_callback_set(client->mosq, on_disconnect_callback); // 断开连接回调
    mosquitto_message_callback_set(client->mosq, on_message_callback);       // 消息接收回调
    
    // 设置用户名和密码（如果配置了）
    if (config->username != NULL && config->password != NULL) {
        int rc = mosquitto_username_pw_set(client->mosq, config->username, config->password);
        if (rc != MOSQ_ERR_SUCCESS) {
            MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to set username/password: %s", mosquitto_strerror(rc));
            mosquitto_destroy(client->mosq);
            platform_free(client);
            return NULL;
        }
    }
    
    // TLS相关代码已注释掉，不再使用TLS
    // if (config->use_tls) {
    //     // 初始化TLS配置
    //     int rc = mosquitto_tls_set(client->mosq, config->ca_cert_path, NULL, 
    //                              config->client_cert_path, config->client_key_path, NULL);
    //     if (rc != MOSQ_ERR_SUCCESS) {
    //         MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to set TLS: %s", mosquitto_strerror(rc));
    //         mosquitto_destroy(client->mosq);
    //         platform_free(client);
    //         return NULL;
    //     }
    //     
    //     // 设置TLS选项（跳过证书验证，仅用于测试）
    //     mosquitto_tls_insecure_set(client->mosq, true);
    // }
    
    // 设置自动重连参数
    // 参数说明：
    // 1. min_delay: 最小重连延迟（秒）
    // 2. max_delay: 最大重连延迟（秒）
    // 3. exponential_backoff: 是否使用指数退避
    mosquitto_reconnect_delay_set(client->mosq, 1, client->config.retry_interval_ms / 1000, true);
    
    // 初始化客户端状态为断开连接
    client->state = MQTT_CLIENT_STATE_DISCONNECTED;
    client->auto_reconnect = true; // 启用自动重连
    client->last_status_print = time(NULL); // 初始化上次打印状态的时间
    
    return client;
}

void mqtt_client_destroy(mqtt_client_t client)
{
    if (client == NULL) {
        return;
    }
    
    // 断开连接
    if (client->state == MQTT_CLIENT_STATE_CONNECTED) {
        // 停止内部事件循环线程
        mosquitto_loop_stop(client->mosq, true);
        mosquitto_disconnect(client->mosq);
        // 等待断开连接完成
        mosquitto_loop(client->mosq, 1000, 1);
    }
    
    // 销毁mosquitto客户端
    if (client->mosq != NULL) {
        mosquitto_destroy(client->mosq);
        client->mosq = NULL;
    }
    
    // 释放订阅列表
    pthread_mutex_lock(&client->mutex);
    subscription_info_t *sub = client->subscriptions;
    while (sub != NULL) {
        subscription_info_t *next = sub->next;
        if (sub->topic != NULL) {
            platform_free(sub->topic);
        }
        platform_free(sub);
        sub = next;
    }
    client->subscriptions = NULL;
    pthread_mutex_unlock(&client->mutex);
    
    // 销毁互斥锁
    pthread_mutex_destroy(&client->mutex);
    
    // 释放内存
    platform_free(client);
}

/**
 * @brief 连接到MQTT服务器
 * 
 * 此函数尝试连接到配置中指定的MQTT服务器，并启动内部事件循环线程。
 * 
 * @param client MQTT客户端句柄
 * @return 成功返回MQTT_ERR_SUCCESS(0)，失败返回错误码
 * 
 * @error MQTT_ERR_INVALID_PARAM: 无效的客户端句柄
 * @error MQTT_ERR_CONNECT_FAILED: 连接服务器失败
 * @error MQTT_ERR_INTERNAL: 内部错误（如启动事件循环失败）
 * 
 * @note 连接流程：
 * 1. 检查客户端状态，已连接则直接返回成功
 * 2. 设置状态为CONNECTING并通知回调
 * 3. 尝试连接到MQTT服务器
 * 4. 启动内部事件循环线程
 * 5. 连接结果通过状态回调通知用户
 * 
 * @example
 * int rc = mqtt_client_connect(client);
 * if (rc != MQTT_ERR_SUCCESS) {
 *     printf("连接失败: %d\n", rc);
 *     return rc;
 * }
 * printf("连接中，请等待状态回调...\n");
 */
int mqtt_client_connect(mqtt_client_t client)
{
    if (client == NULL) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "mqtt_client_connect called with NULL client");
        return MQTT_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client->mutex);
    
    // 检查是否已连接
    if (client->state == MQTT_CLIENT_STATE_CONNECTED) {
        MQTT_LOG(MQTT_LOG_LEVEL_INFO, "mqtt_client_connect called, but already connected");
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_SUCCESS;
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Current state before connect: %d", client->state);
    
    // 设置状态为连接中
    client->state = MQTT_CLIENT_STATE_CONNECTING;
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "State set to CONNECTING");
    
    // 先解锁，再调用回调函数，避免死锁
    // 回调函数可能会调用其他需要获取锁的API
    pthread_mutex_unlock(&client->mutex);
    
    // 调用用户注册的状态回调，通知连接开始
    if (client->state_cb) {
        MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "Calling state callback for CONNECTING state");
        client->state_cb(client->state, client->state_cb_user_data);
    } else {
        MQTT_LOG(MQTT_LOG_LEVEL_WARNING, "No state callback registered");
    }
    
    // 添加连接尝试的详细日志
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Attempting to connect to broker at %s:%d, client_id: %s", 
             client->config.host, client->config.port, client->config.client_id);
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Connection parameters: keep_alive=%d, clean_session=%d", 
             client->config.keep_alive, client->config.clean_session);
    
    // 注意：Mosquitto 2.0.18版本不支持mosquitto_connect_timeout_set和mosquitto_loop_set函数
    // 使用同步连接方式，但先启动loop线程，这样连接过程不会阻塞主线程
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Mosquitto 2.0.18 detected, using sync connection with loop thread");
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Connect timeout configured: %d ms", client->config.connect_timeout_ms);
    
    // 先启动内部事件循环线程（在连接之前启动，这样连接过程是异步的）
    // 此线程负责处理网络事件、心跳、重连等
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Calling mosquitto_loop_start before connect...");
    int rc = mosquitto_loop_start(client->mosq);
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "mosquitto_loop_start returned: %d", rc);
    
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to start loop: %s (code: %d)", mosquitto_strerror(rc), rc);
        
        // 恢复状态为断开连接
        pthread_mutex_lock(&client->mutex);
        client->state = MQTT_CLIENT_STATE_DISCONNECTED;
        client->retry_count++;
        MQTT_LOG(MQTT_LOG_LEVEL_INFO, "State set to DISCONNECTED, retry count: %d", client->retry_count);
        
        // 调用状态回调，通知连接失败
        if (client->state_cb) {
            MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "Calling state callback for DISCONNECTED state");
            client->state_cb(client->state, client->state_cb_user_data);
        }
        
        pthread_mutex_unlock(&client->mutex);
        
        return MQTT_ERR_INTERNAL;
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Event loop thread started successfully");
    
    // 连接到MQTT服务器（同步调用，但由于loop线程已启动，不会阻塞太久）
    // 参数说明：
    // 1. host: MQTT服务器地址
    // 2. port: MQTT服务器端口
    // 3. keep_alive: 心跳间隔（秒）
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Calling mosquitto_connect...");
    rc = mosquitto_connect(client->mosq, client->config.host, client->config.port, client->config.keep_alive);
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "mosquitto_connect returned: %d", rc);
    
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Connect failed: %s (code: %d)", mosquitto_strerror(rc), rc);
        
        // 停止loop线程
        mosquitto_loop_stop(client->mosq, true);
        
        // 连接失败，恢复状态为断开连接
        pthread_mutex_lock(&client->mutex);
        client->state = MQTT_CLIENT_STATE_DISCONNECTED;
        client->retry_count++;
        MQTT_LOG(MQTT_LOG_LEVEL_INFO, "State set to DISCONNECTED, retry count: %d", client->retry_count);
        
        // 调用状态回调，通知连接失败
        if (client->state_cb) {
            MQTT_LOG(MQTT_LOG_LEVEL_DEBUG, "Calling state callback for DISCONNECTED state");
            client->state_cb(client->state, client->state_cb_user_data);
        }
        
        pthread_mutex_unlock(&client->mutex);
        
        return MQTT_ERR_CONNECT_FAILED;
    }
    
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "mosquitto_connect succeeded");
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Connection process initiated, waiting for on_connect_callback");
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "mqtt_client_connect function will now return, connection result will be notified via callback");
    return MQTT_ERR_SUCCESS;
}

int mqtt_client_disconnect(mqtt_client_t client)
{
    if (client == NULL) {
        return MQTT_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client->mutex);
    
    if (client->state != MQTT_CLIENT_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_SUCCESS;
    }
    
    client->state = MQTT_CLIENT_STATE_DISCONNECTING;
    
    // 调用用户注册的状态回调
    if (client->state_cb) {
        client->state_cb(client->state, client->state_cb_user_data);
    }
    
    pthread_mutex_unlock(&client->mutex);
    
    // 停止内部事件循环线程
    int rc = mosquitto_loop_stop(client->mosq, true);
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Failed to stop loop: %s", mosquitto_strerror(rc));
    }
    
    // 断开连接
    rc = mosquitto_disconnect(client->mosq);
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Disconnect failed: %s", mosquitto_strerror(rc));
        return MQTT_ERR_INTERNAL;
    }
    
    return MQTT_ERR_SUCCESS;
}

/**
 * @brief 发布MQTT消息
 * 
 * 此函数向指定主题发布MQTT消息，支持不同的QoS级别和保留消息选项。
 * 
 * @param client MQTT客户端句柄
 * @param message MQTT消息结构体，包含主题、负载、QoS等信息
 * @return 成功返回MQTT_ERR_SUCCESS(0)，失败返回错误码
 * 
 * @error MQTT_ERR_INVALID_PARAM: 无效的参数
 * @error MQTT_ERR_DISCONNECTED: 客户端未连接
 * @error MQTT_ERR_PUBLISH_FAILED: 发布失败
 * 
 * @note 消息发布流程：
 * 1. 检查参数和连接状态
 * 2. 调用mosquitto_publish发送消息
 * 3. 返回发布结果
 * 
 * @example
 * mqtt_message_t msg = {
 *     .topic = "device/001/data",
 *     .payload = "{\"temperature\": 25.5}",
 *     .payload_len = strlen("{\"temperature\": 25.5}"),
 *     .qos = MQTT_QOS_1,
 *     .retain = false
 * };
 * int rc = mqtt_client_publish(client, &msg);
 * if (rc == MQTT_ERR_SUCCESS) {
 *     printf("消息发布成功\n");
 * }
 */
int mqtt_client_publish(mqtt_client_t client, const mqtt_message_t *message)
{
    if (client == NULL || message == NULL || message->topic == NULL) {
        return MQTT_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client->mutex);
    
    // 检查客户端是否已连接
    if (client->state != MQTT_CLIENT_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_DISCONNECTED;
    }
    
    pthread_mutex_unlock(&client->mutex);
    
    // 发布消息
    // 参数说明：
    // 1. mosq: mosquitto客户端句柄
    // 2. mid: 消息ID指针，NULL表示不需要
    // 3. topic: 消息主题
    // 4. payloadlen: 消息负载长度
    // 5. payload: 消息负载
    // 6. qos: QoS级别
    // 7. retain: 是否为保留消息
    int rc = mosquitto_publish(client->mosq, NULL, message->topic, 
                              message->payload_len, message->payload, 
                              message->qos, message->retain);
    
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Publish failed: %s", mosquitto_strerror(rc));
        return MQTT_ERR_PUBLISH_FAILED;
    }
    
    return MQTT_ERR_SUCCESS;
}

/**
 * @brief 订阅MQTT主题
 * 
 * 此函数订阅指定的MQTT主题，并设置消息回调函数以处理接收到的消息。
 * 
 * @param client MQTT客户端句柄
 * @param topic 要订阅的主题，可以使用通配符
 * @param qos 订阅的QoS级别（0-2）
 * @param callback 消息回调函数，用于处理接收到的消息
 * @param user_data 用户数据，会传递给回调函数
 * @return 成功返回MQTT_ERR_SUCCESS(0)，失败返回错误码
 * 
 * @error MQTT_ERR_INVALID_PARAM: 无效的参数
 * @error MQTT_ERR_DISCONNECTED: 客户端未连接
 * @error MQTT_ERR_NO_MEMORY: 内存不足
 * @error MQTT_ERR_SUBSCRIBE_FAILED: 订阅失败
 * 
 * @note 订阅流程：
 * 1. 检查参数和连接状态
 * 2. 创建订阅信息结构体
 * 3. 将订阅信息添加到列表
 * 4. 调用mosquitto_subscribe发送订阅请求
 * 5. 处理订阅结果
 * 
 * @example
 * // 订阅命令主题
 * int rc = mqtt_client_subscribe(client, "device/001/command", 
 *                               MQTT_QOS_0, on_message_received, NULL);
 * if (rc != MQTT_ERR_SUCCESS) {
 *     printf("订阅失败: %d\n", rc);
 * }
 */
int mqtt_client_subscribe(mqtt_client_t client, const char *topic, mqtt_qos_t qos, 
                         mqtt_message_callback_t callback, void *user_data)
{
    if (client == NULL || topic == NULL) {
        return MQTT_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client->mutex);
    
    // 检查连接状态
    if (client->state != MQTT_CLIENT_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_DISCONNECTED;
    }
    
    // 创建新的订阅信息结构体
    subscription_info_t *new_sub = (subscription_info_t *)platform_malloc(sizeof(subscription_info_t));
    if (new_sub == NULL) {
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_NO_MEMORY;
    }
    
    // 复制主题字符串
    new_sub->topic = platform_strdup(topic);
    if (new_sub->topic == NULL) {
        platform_free(new_sub);
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_NO_MEMORY;
    }
    
    // 设置回调函数和用户数据
    new_sub->callback = callback;
    new_sub->user_data = user_data;
    new_sub->next = NULL;
    
    // 将新的订阅信息添加到列表末尾
    if (client->subscriptions == NULL) {
        client->subscriptions = new_sub;
    } else {
        subscription_info_t *sub = client->subscriptions;
        while (sub->next != NULL) {
            sub = sub->next;
        }
        sub->next = new_sub;
    }
    
    pthread_mutex_unlock(&client->mutex);
    
    // 订阅主题
    // 参数说明：
    // 1. mosq: mosquitto客户端句柄
    // 2. mid: 消息ID指针，NULL表示不需要
    // 3. sub: 要订阅的主题
    // 4. qos: QoS级别
    int rc = mosquitto_subscribe(client->mosq, NULL, topic, qos);
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Subscribe failed: %s", mosquitto_strerror(rc));
        
        // 订阅失败，从列表中移除该订阅信息
        pthread_mutex_lock(&client->mutex);
        subscription_info_t *sub = client->subscriptions;
        subscription_info_t *prev = NULL;
        while (sub != NULL) {
            if (strcmp(sub->topic, topic) == 0) {
                if (prev == NULL) {
                    client->subscriptions = sub->next;
                } else {
                    prev->next = sub->next;
                }
                platform_free(sub->topic);
                platform_free(sub);
                break;
            }
            prev = sub;
            sub = sub->next;
        }
        pthread_mutex_unlock(&client->mutex);
        
        return MQTT_ERR_SUBSCRIBE_FAILED;
    }
    
    return MQTT_ERR_SUCCESS;
}

int mqtt_client_unsubscribe(mqtt_client_t client, const char *topic)
{
    if (client == NULL || topic == NULL) {
        return MQTT_ERR_INVALID_PARAM;
    }
    
    pthread_mutex_lock(&client->mutex);
    
    if (client->state != MQTT_CLIENT_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->mutex);
        return MQTT_ERR_DISCONNECTED;
    }
    
    pthread_mutex_unlock(&client->mutex);
    
    // 取消订阅
    int rc = mosquitto_unsubscribe(client->mosq, NULL, topic);
    if (rc != MOSQ_ERR_SUCCESS) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "Unsubscribe failed: %s", mosquitto_strerror(rc));
        return MQTT_ERR_UNSUBSCRIBE_FAILED;
    }
    
    // 从订阅列表中移除该主题的订阅信息
    pthread_mutex_lock(&client->mutex);
    
    subscription_info_t *sub = client->subscriptions;
    subscription_info_t *prev = NULL;
    while (sub != NULL) {
        if (strcmp(sub->topic, topic) == 0) {
            if (prev == NULL) {
                client->subscriptions = sub->next;
            } else {
                prev->next = sub->next;
            }
            platform_free(sub->topic);
            platform_free(sub);
            break;
        }
        prev = sub;
        sub = sub->next;
    }
    
    pthread_mutex_unlock(&client->mutex);
    
    return MQTT_ERR_SUCCESS;
}

mqtt_client_state_t mqtt_client_get_state(mqtt_client_t client)
{
    if (client == NULL) {
        return MQTT_CLIENT_STATE_DISCONNECTED;
    }
    
    mqtt_client_state_t state;
    pthread_mutex_lock(&client->mutex);
    state = client->state;
    pthread_mutex_unlock(&client->mutex);
    
    return state;
}

void mqtt_client_set_state_callback(mqtt_client_t client, mqtt_state_callback_t callback, void *user_data)
{
    if (client == NULL) {
        return;
    }
    
    pthread_mutex_lock(&client->mutex);
    client->state_cb = callback;
    client->state_cb_user_data = user_data;
    pthread_mutex_unlock(&client->mutex);
}

int mqtt_client_loop(mqtt_client_t client, int timeout_ms)
{
    (void)timeout_ms;  // 在使用loop_start的情况下不需要timeout参数
    
    if (client == NULL) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "mqtt_client_loop called with NULL client");
        return MQTT_ERR_INVALID_PARAM;
    }
    
    // 检查是否需要打印连接状态（每10秒一次）
    time_t current_time = time(NULL);
    if (current_time - client->last_status_print >= 10) {
        pthread_mutex_lock(&client->mutex);
        
        const char *state_str = "Unknown";
        switch (client->state) {
            case MQTT_CLIENT_STATE_DISCONNECTED:
                state_str = "Disconnected";
                break;
            case MQTT_CLIENT_STATE_CONNECTING:
                state_str = "Connecting";
                break;
            case MQTT_CLIENT_STATE_CONNECTED:
                state_str = "Connected";
                break;
            case MQTT_CLIENT_STATE_DISCONNECTING:
                state_str = "Disconnecting";
                break;
        }
        
        MQTT_LOG(MQTT_LOG_LEVEL_INFO, "MQTT broker connection status: %s", state_str);
        
        client->last_status_print = current_time;
        pthread_mutex_unlock(&client->mutex);
    }
    
    // 检查mosquitto客户端是否有效
    if (client->mosq == NULL) {
        MQTT_LOG(MQTT_LOG_LEVEL_ERROR, "mosquitto client is NULL in mqtt_client_loop");
        return MQTT_ERR_LOOP_FAILED;
    }
    
    // 注意：我们使用mosquitto_loop_start启动了自动事件循环线程
    // 所以这里不需要再调用mosquitto_loop
    // 只需要短暂休眠，让出CPU时间片即可
    usleep(10000);  // 10ms
    
    // 检查连接状态，如果断开则返回错误
    pthread_mutex_lock(&client->mutex);
    mqtt_client_state_t current_state = client->state;
    pthread_mutex_unlock(&client->mutex);
    
    if (current_state == MQTT_CLIENT_STATE_DISCONNECTED) {
        return MQTT_ERR_DISCONNECTED;
    }
    
    return MQTT_ERR_SUCCESS;
}

/**
 * @brief 设置MQTT日志级别
 */
void mqtt_set_log_level(mqtt_log_level_t level)
{
    g_log_level = level;
    MQTT_LOG(MQTT_LOG_LEVEL_INFO, "Log level set to %d", level);
}
