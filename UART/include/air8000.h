/**
 * @file air8000.h
 * @brief Air8000 高级 API 头文件
 * @details 定义了 Air8000 通信库的高级接口，包括上下文管理、单例模式、各种业务命令等
 * 
 * API 设计架构：
 * 1. **上下文管理**：
 *    - 支持多实例模式，每个实例对应一个独立的串口连接
 *    - 支持单例模式，便于全局访问同一设备
 *    - 提供完整的初始化和销毁机制
 * 2. **错误处理**：
 *    - 统一的错误码体系，便于错误识别和处理
 *    - 每个 API 函数返回明确的错误状态
 * 3. **命令分类**：
 *    - 系统命令：PING、版本查询、网络状态等
 *    - 电机命令：旋转、使能、禁用、原点设置等
 *    - 设备命令：LED、风扇、激光等设备控制
 *    - 传感器命令：温度、传感器数据读取
 *    - 看门狗命令：配置、状态查询、心跳等
 * 4. **设计原则**：
 *    - 简单易用：提供高级抽象，隐藏底层复杂性
 *    - 线程安全：支持多线程环境下的安全调用
 *    - 可扩展：便于添加新的命令和功能
 *    - 错误容忍：具有自动重连和超时处理机制
 */

#ifndef AIR8000_H
#define AIR8000_H

#include "air8000_protocol.h"    /* 协议定义和帧处理 */
#include "air8000_serial.h"      /* 串口通信抽象层 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 默认串口路径，与 Rust SDK 保持一致
 * @details 当未指定设备路径时，SDK 将使用此默认路径打开串口
 */
#define DEFAULT_SERIAL_PATH "/dev/ttyACM2"

/**
 * @brief Air8000 上下文句柄类型声明
 * @details 上下文包含了 SDK 运行所需的所有状态信息，是所有 API 调用的基础
 */
typedef struct air8000_s air8000_t;

/**
 * @brief 错误码定义
 * @details 所有 API 函数返回的错误码，用于表示操作结果
 */
typedef enum {
    AIR8000_OK = 0,             ///< 操作成功
    AIR8000_ERR_GENERIC = -1,   ///< 通用错误，未分类的错误
    AIR8000_ERR_TIMEOUT = -2,   ///< 操作超时
    AIR8000_ERR_PARAM = -3,     ///< 参数错误，无效的输入参数
    AIR8000_ERR_NOMEM = -4,     ///< 内存不足，无法分配所需内存
    AIR8000_ERR_IO = -5,        ///< I/O 错误，串口通信失败
    AIR8000_ERR_PROTOCOL = -6,  ///< 协议错误，帧格式或内容无效
    AIR8000_ERR_BUSY = -7,      ///< 系统忙，如序列号冲突
    AIR8000_ERR_SHUTDOWN = -8   ///< 系统正在关闭，无法执行操作
} air8000_error_t;

/**
 * @brief 通知回调函数类型定义
 * @details 用于接收设备主动发送的通知消息
 * @param frame 接收到的通知帧
 * @param user_data 用户自定义数据，在设置回调时传入
 */
typedef void (*air8000_notify_cb_t)(const air8000_frame_t *frame, void *user_data);

/**
 * @brief 初始化 Air8000 上下文
 * @details 创建并初始化一个新的 Air8000 上下文，打开串口并启动 I/O 线程
 * @param device_path 设备路径，如 "/dev/ttyACM2"，NULL 表示使用默认路径
 * @return 成功返回上下文指针，失败返回 NULL
 * @note 每个上下文对应一个独立的串口连接，支持多设备连接
 */
air8000_t* air8000_init(const char *device_path);

/**
 * @brief 销毁 Air8000 上下文
 * @details 关闭串口，停止 I/O 线程，释放所有资源
 * @param ctx 要销毁的上下文指针
 * @note 必须在不再使用上下文时调用，否则会导致内存泄漏
 */
void air8000_deinit(air8000_t *ctx);

/**
 * @brief 获取 Air8000 全局单例实例
 * @details 实现单例模式，确保整个应用中只有一个 Air8000 上下文实例
 * @return 成功返回全局单例指针，失败返回 NULL
 * @note 首次调用时会自动初始化，使用默认串口路径
 */
air8000_t* air8000_get_instance(void);

/**
 * @brief 使用自定义设备路径初始化或重置全局单例
 * @details 如果单例已存在，会先销毁再重新初始化
 * @param device_path 设备路径，NULL 表示使用默认路径
 * @return 成功返回全局单例指针，失败返回 NULL
 */
air8000_t* air8000_init_instance(const char *device_path);

/**
 * @brief 重置 (销毁) 全局单例实例
 * @details 销毁当前全局单例，释放所有资源
 */
void air8000_reset_instance(void);

/**
 * @brief 设置通知回调函数
 * @details 用于接收设备主动发送的通知消息
 * @param ctx 上下文指针
 * @param cb 回调函数指针，NULL 表示取消回调
 * @param user_data 用户自定义数据，将在回调时传递
 */
void air8000_set_notify_callback(air8000_t *ctx, air8000_notify_cb_t cb, void *user_data);

/**
 * @brief 发送帧并等待响应
 * @details 发送请求帧到设备，并等待响应，是所有业务命令的基础
 * @param ctx 上下文指针
 * @param req 请求帧指针，包含要发送的命令和数据
 * @param resp 输出响应帧指针，如果非 NULL，SDK 会为 resp->data 分配内存，调用者需调用 air8000_frame_cleanup 释放
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_send_and_wait(air8000_t *ctx, const air8000_frame_t *req, air8000_frame_t *resp, int timeout_ms);

// ==================== 系统命令 ====================

/**
 * @brief 发送 PING 命令，测试设备连接
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_ping(air8000_t *ctx, int timeout_ms);

/**
 * @brief 获取设备版本信息
 * @param ctx 上下文指针
 * @param ver 输出版本信息结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_get_version(air8000_t *ctx, air8000_version_t *ver, int timeout_ms);

/**
 * @brief 发送系统重置命令
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_sys_reset(air8000_t *ctx, int timeout_ms);

/**
 * @brief 查询网络状态
 * @param ctx 上下文指针
 * @param net 输出网络状态结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_query_network(air8000_t *ctx, air8000_network_status_t *net, int timeout_ms);

/**
 * @brief 查询电源状态
 * @param ctx 上下文指针
 * @param pwr 输出电源状态结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_query_power(air8000_t *ctx, air8000_power_adc_t *pwr, int timeout_ms);

// ==================== 电机命令 ====================

/**
 * @brief 使能电机
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z, 0xFF=所有)
 * @param mode 工作模式 (2=位置速度模式)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_enable(air8000_t *ctx, uint8_t motor_id, int mode, int timeout_ms);

/**
 * @brief 禁用电机
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z, 0xFF=所有)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_disable(air8000_t *ctx, uint8_t motor_id, int timeout_ms);

/**
 * @brief 电机急停
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z, 0xFF=所有)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_stop(air8000_t *ctx, uint8_t motor_id, int timeout_ms);

/**
 * @brief 电机绝对位置旋转
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param angle 目标角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_rotate(air8000_t *ctx, uint8_t motor_id, float angle, float velocity, int timeout_ms);

/**
 * @brief 电机相对位置旋转
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param angle 相对旋转角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_rotate_rel(air8000_t *ctx, uint8_t motor_id, float angle, float velocity, int timeout_ms);

/**
 * @brief 设置电机速度
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param velocity 目标速度，单位弧度/秒
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_set_vel(air8000_t *ctx, uint8_t motor_id, float velocity, int timeout_ms);

/**
 * @brief 设置电机原点
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_set_origin(air8000_t *ctx, uint8_t motor_id, int timeout_ms);

/**
 * @brief 获取电机当前位置
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param pos 输出当前位置，单位弧度
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_get_pos(air8000_t *ctx, uint8_t motor_id, float *pos, int timeout_ms);

/**
 * @brief 获取所有电机状态
 * @param ctx 上下文指针
 * @param status 输出所有电机状态结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 * @note 调用后需调用 air8000_free_motor_status 释放内存
 */
int air8000_motor_get_all(air8000_t *ctx, air8000_all_motor_status_t *status, int timeout_ms);

/**
 * @brief 释放电机状态内存
 * @param status 电机状态结构体指针
 * @note 必须在使用完 air8000_all_motor_status_t 后调用
 */
void air8000_free_motor_status(air8000_all_motor_status_t *status);

/**
 * @brief 读取电机寄存器
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param reg 寄存器地址
 * @param val 输出寄存器值
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_read_reg(air8000_t *ctx, uint8_t motor_id, uint8_t reg, float *val, int timeout_ms);

/**
 * @brief 写入电机寄存器
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param reg 寄存器地址
 * @param val 要写入的寄存器值
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_write_reg(air8000_t *ctx, uint8_t motor_id, uint8_t reg, float val, int timeout_ms);

/**
 * @brief 保存电机参数到 Flash
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_save_flash(air8000_t *ctx, uint8_t motor_id, int timeout_ms);

/**
 * @brief 清除电机错误
 * @param ctx 上下文指针
 * @param motor_id 电机 ID (1=Y, 2=X, 3=Z)
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_clear_error(air8000_t *ctx, uint8_t motor_id, int timeout_ms);

// ==================== 设备命令 ====================

/**
 * @brief 设备控制通用命令
 * @param ctx 上下文指针
 * @param cmd 命令码，如 CMD_DEV_LED, CMD_DEV_FAN 等
 * @param dev_id 设备 ID
 * @param state 设备状态，如 DEVICE_STATE_ON, DEVICE_STATE_OFF 等
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_device_control(air8000_t *ctx, air8000_command_t cmd, uint8_t dev_id, uint8_t state, int timeout_ms);

/**
 * @brief 电机供电控制
 * @param ctx 上下文指针
 * @param on 供电状态，true 为开启，false 为关闭
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_motor_power_control(air8000_t *ctx, bool on, int timeout_ms);

// ==================== 传感器命令 ====================

/**
 * @brief 读取单个温度传感器
 * @param ctx 上下文指针
 * @param sensor_id 传感器 ID
 * @param temp 输出温度值，单位摄氏度
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_sensor_read_temp(air8000_t *ctx, uint8_t sensor_id, float *temp, int timeout_ms);

/**
 * @brief 读取所有传感器数据
 * @param ctx 上下文指针
 * @param data 输出传感器数据结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_sensor_read_all(air8000_t *ctx, air8000_sensor_data_t *data, int timeout_ms);

// ==================== 看门狗命令 ====================

/**
 * @brief 配置看门狗
 * @param ctx 上下文指针
 * @param cfg 看门狗配置结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_wdt_config(air8000_t *ctx, const air8000_wdt_config_t *cfg, int timeout_ms);

/**
 * @brief 查询看门狗状态
 * @param ctx 上下文指针
 * @param status 输出看门狗状态结构体指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 */
int air8000_wdt_status(air8000_t *ctx, air8000_wdt_status_t *status, int timeout_ms);

/**
 * @brief 发送看门狗心跳
 * @param ctx 上下文指针
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回 0，失败返回负数错误码
 * @note 心跳间隔必须小于看门狗超时时间，否则设备会重启
 */
int air8000_wdt_heartbeat(air8000_t *ctx, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_H
