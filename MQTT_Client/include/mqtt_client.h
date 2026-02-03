/**
 * @file mqtt_client.h
 * @brief MQTT客户端API定义
 * @version 1.0
 * @date 2026-01-17
 * 
 * 本文件定义了MQTT客户端的核心API，基于Mosquitto库实现，
 * 专为海思CV610平台优化，提供完整的MQTT通信能力。
 * 
 * API功能包括：
 * - 客户端生命周期管理（创建/销毁）
 * - 连接管理（连接/断开/重连）
 * - 消息发布与订阅
 * - 状态管理与回调
 * - 日志级别控制
 * 
 * 设计特点：
 * - 线程安全：所有函数都支持多线程调用
 * - 平台适配：针对海思CV610平台进行了优化
 * - 错误处理：详细的错误码和日志
 * - 回调机制：异步处理连接状态和消息
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief MQTT客户端状态枚举
 */
typedef enum {
    MQTT_CLIENT_STATE_DISCONNECTED = 0,  /**< 已断开连接 */
    MQTT_CLIENT_STATE_CONNECTING,        /**< 连接中 */
    MQTT_CLIENT_STATE_CONNECTED,         /**< 已连接 */
    MQTT_CLIENT_STATE_DISCONNECTING      /**< 断开连接中 */
} mqtt_client_state_t;

/**
 * @brief MQTT消息质量等级
 */
typedef enum {
    MQTT_QOS_0 = 0,  /**< 最多一次 */
    MQTT_QOS_1,      /**< 至少一次 */
    MQTT_QOS_2       /**< 正好一次 */
} mqtt_qos_t;

/**
 * @brief MQTT客户端配置结构体
 */
typedef struct {
    const char *host;               /**< MQTT服务器地址 */
    int port;                       /**< MQTT服务器端口，默认1883，TLS默认8883 */
    const char *client_id;           /**< MQTT客户端ID */
    const char *username;           /**< 用户名（可选） */
    const char *password;           /**< 密码（可选） */
    int keep_alive;                 /**< 心跳间隔，单位秒，默认60 */
    bool clean_session;             /**< 清除会话标志，默认true */
    // TLS相关字段已注释掉，不再使用TLS
    // bool use_tls;                   /**< 是否使用TLS加密，默认false */
    // const char *ca_cert_path;       /**< CA证书路径（TLS时必填） */
    // const char *client_cert_path;   /**< 客户端证书路径（双向认证时必填） */
    // const char *client_key_path;    /**< 客户端密钥路径（双向认证时必填） */
    int connect_timeout_ms;         /**< 连接超时时间，单位毫秒，默认5000 */
    int retry_interval_ms;          /**< 重连间隔时间，单位毫秒，默认2000 */
    int max_retry_count;            /**< 最大重连次数，默认-1（无限重试） */
} mqtt_client_config_t;

/**
 * @brief MQTT消息结构体
 */
typedef struct {
    const char *topic;      /**< 消息主题 */
    const void *payload;    /**< 消息内容 */
    size_t payload_len;     /**< 消息内容长度 */
    mqtt_qos_t qos;         /**< 消息质量等级 */
    bool retain;            /**< 保留消息标志 */
} mqtt_message_t;

/**
 * @brief MQTT消息回调函数类型
 * @param topic 消息主题
 * @param payload 消息内容
 * @param payload_len 消息内容长度
 * @param user_data 用户数据
 */
typedef void (*mqtt_message_callback_t)(const char *topic, const void *payload, size_t payload_len, void *user_data);

/**
 * @brief MQTT连接状态变化回调函数类型
 * @param state 新的连接状态
 * @param user_data 用户数据
 */
typedef void (*mqtt_state_callback_t)(mqtt_client_state_t state, void *user_data);

/**
 * @brief MQTT客户端句柄（不透明结构体）
 */
typedef struct mqtt_client_impl *mqtt_client_t;

/**
 * @brief 创建MQTT客户端
 * @param config MQTT客户端配置
 * @return MQTT客户端句柄，失败返回NULL
 */
mqtt_client_t mqtt_client_create(const mqtt_client_config_t *config);

/**
 * @brief 销毁MQTT客户端
 * @param client MQTT客户端句柄
 */
void mqtt_client_destroy(mqtt_client_t client);

/**
 * @brief 连接到MQTT服务器
 * @param client MQTT客户端句柄
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_connect(mqtt_client_t client);

/**
 * @brief 断开与MQTT服务器的连接
 * @param client MQTT客户端句柄
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_disconnect(mqtt_client_t client);

/**
 * @brief 发布MQTT消息
 * @param client MQTT客户端句柄
 * @param message MQTT消息
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_publish(mqtt_client_t client, const mqtt_message_t *message);

/**
 * @brief 订阅MQTT主题
 * @param client MQTT客户端句柄
 * @param topic 要订阅的主题
 * @param qos 订阅的QoS等级
 * @param callback 消息回调函数
 * @param user_data 用户数据
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_subscribe(mqtt_client_t client, const char *topic, mqtt_qos_t qos, 
                         mqtt_message_callback_t callback, void *user_data);

/**
 * @brief 取消订阅MQTT主题
 * @param client MQTT客户端句柄
 * @param topic 要取消订阅的主题
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_unsubscribe(mqtt_client_t client, const char *topic);

/**
 * @brief 获取当前MQTT连接状态
 * @param client MQTT客户端句柄
 * @return 当前连接状态
 */
mqtt_client_state_t mqtt_client_get_state(mqtt_client_t client);

/**
 * @brief 设置MQTT连接状态变化回调
 * @param client MQTT客户端句柄
 * @param callback 状态变化回调函数
 * @param user_data 用户数据
 */
void mqtt_client_set_state_callback(mqtt_client_t client, mqtt_state_callback_t callback, void *user_data);

/**
 * @brief MQTT客户端主循环，需要定期调用以处理网络事件
 * @param client MQTT客户端句柄
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回0，失败返回错误码
 */
int mqtt_client_loop(mqtt_client_t client, int timeout_ms);

/**
 * @brief MQTT日志级别
 */
typedef enum {
    MQTT_LOG_LEVEL_NONE = 0,     /**< 无日志输出 */
    MQTT_LOG_LEVEL_ERROR,        /**< 仅错误日志 */
    MQTT_LOG_LEVEL_WARNING,      /**< 错误和警告日志 */
    MQTT_LOG_LEVEL_INFO,         /**< 错误、警告和信息日志 */
    MQTT_LOG_LEVEL_DEBUG         /**< 所有日志，包括调试信息 */
} mqtt_log_level_t;

/**
 * @brief MQTT错误码
 */
#define MQTT_ERR_SUCCESS            0   /**< 成功 */
#define MQTT_ERR_INVALID_PARAM     -1   /**< 无效参数 */
#define MQTT_ERR_NO_MEMORY         -2   /**< 内存不足 */
#define MQTT_ERR_CONNECT_FAILED    -3   /**< 连接失败 */
#define MQTT_ERR_DISCONNECTED      -4   /**< 未连接 */
#define MQTT_ERR_PUBLISH_FAILED    -5   /**< 发布失败 */
#define MQTT_ERR_SUBSCRIBE_FAILED  -6   /**< 订阅失败 */
#define MQTT_ERR_UNSUBSCRIBE_FAILED -7  /**< 取消订阅失败 */
#define MQTT_ERR_LOOP_FAILED       -8   /**< 主循环失败 */
#define MQTT_ERR_TIMEOUT           -9   /**< 超时 */
// TLS相关错误码已注释掉，不再使用TLS
// #define MQTT_ERR_TLS_FAILED        -10  /**< TLS握手失败 */
#define MQTT_ERR_INTERNAL          -11  /**< 内部错误 */

/**
 * @brief 设置MQTT日志级别
 * @param level 日志级别
 */
void mqtt_set_log_level(mqtt_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
