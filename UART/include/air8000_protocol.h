/**
 * @file air8000_protocol.h
 * @brief Air8000 MCU 通信协议定义头文件
 * @details 定义了 Air8000 设备与上位机之间通信的协议格式、命令码、数据结构和辅助函数
 * 
 * 协议架构设计：
 * 1. **分层结构**：
 *    - 物理层：基于串口通信，波特率 115200
 *    - 数据链路层：帧格式定义、CRC 校验、帧解析
 *    - 应用层：命令体系、数据结构定义
 * 2. **帧格式**：
 *    - 同步字符：0xAA 0x55
 *    - 版本号：当前版本 0x10 (V1.0)
 *    - 帧类型：请求、响应、通知、确认、否认
 *    - 序列号：8位，用于请求-响应匹配
 *    - 命令码：16位，按功能分类
 *    - 数据长度：16位，指示数据字段长度
 *    - 数据：变长，根据命令类型不同
 *    - CRC：2位，MODBUS CRC16 校验
 * 3. **数据结构**：
 *    - 定义了各种命令的请求和响应数据格式
 *    - 支持基本数据类型（整数、浮点数）的网络字节序转换
 *    - 提供文件传输、FOTA升级等高级功能的数据结构
 * 4. **辅助函数**：
 *    - 提供帧构建和解析的便捷函数
 *    - 实现了数据类型转换（主机字节序 <-> 网络字节序）
 *    - 提供CRC计算、序列号生成等基础功能
 */

#ifndef AIR8000_PROTOCOL_H
#define AIR8000_PROTOCOL_H

#include <stdint.h>     /* 标准库头文件，定义了uint8_t等类型 */
#include <stddef.h>     /* 标准库头文件，定义了size_t等类型 */
#include <stdbool.h>    /* 标准库头文件，定义了bool类型 */

#ifdef __cplusplus
extern "C" {
#endif

// ==================== 常量定义 ====================

/**
 * @brief 帧同步字符1
 * @details 用于帧头识别，标识一个新帧的开始
 */
#define AIR8000_SYNC1       0xAA

/**
 * @brief 帧同步字符2
 * @details 用于帧头识别，与SYNC1共同构成帧头标记
 */
#define AIR8000_SYNC2       0x55

/**
 * @brief 协议版本号
 * @details 当前协议版本为 V1.0，使用 0x10 表示
 */
#define AIR8000_VERSION     0x10

/**
 * @brief 帧头大小
 * @details 固定为9字节，包含同步字符、版本、类型、序列号、命令码和数据长度
 */
#define AIR8000_HEADER_SIZE 9

/**
 * @brief CRC校验大小
 * @details 固定为2字节，使用MODBUS CRC16算法
 */
#define AIR8000_CRC_SIZE    2

/**
 * @brief 最小帧大小
 * @details 包含帧头和CRC，没有数据字段的最小帧长度
 */
#define AIR8000_MIN_FRAME   (AIR8000_HEADER_SIZE + AIR8000_CRC_SIZE)

// ==================== 枚举定义 ====================

/**
 * @brief 帧类型枚举
 * @details 定义了不同类型的通信帧，用于区分请求、响应、通知等
 */
typedef enum {
    FRAME_TYPE_REQUEST  = 0x00, ///< 请求帧：上位机发送给设备的命令
    FRAME_TYPE_RESPONSE = 0x01, ///< 响应帧：设备对请求的响应
    FRAME_TYPE_NOTIFY   = 0x02, ///< 通知帧：设备主动发送的状态更新
    FRAME_TYPE_ACK      = 0x03, ///< 确认帧：对请求的简单确认
    FRAME_TYPE_NACK     = 0x04  ///< 否认帧：表示请求失败
} air8000_frame_type_t;

/**
 * @brief 命令码枚举
 * @details 定义了所有支持的命令，按功能分类
 */
typedef enum {
    // 系统命令 (0x00xx)
    CMD_SYS_PING            = 0x0001, ///< PING 测试：用于检测设备连接状态
    CMD_SYS_VERSION         = 0x0002, ///< 获取版本：获取设备固件版本信息
    CMD_SYS_RESET           = 0x0003, ///< 系统复位：重启设备
    CMD_SYS_SLEEP           = 0x0004, ///< 休眠：使设备进入休眠状态
    CMD_SYS_WAKEUP          = 0x0005, ///< 唤醒：唤醒休眠中的设备
    CMD_SYS_HB_WDT_CONFIG   = 0x0006, ///< 心跳看门狗配置：配置看门狗参数
    CMD_SYS_HB_WDT_STATUS   = 0x0007, ///< 心跳看门狗状态查询：获取看门狗当前状态
    CMD_SYS_HB_POWEROFF     = 0x0008, ///< 断电前通知：设备断电前通知上位机
    CMD_SYS_SET_RTC         = 0x0010, ///< 设置 RTC：设置设备实时时钟
    CMD_SYS_GET_RTC         = 0x0011, ///< 获取 RTC：获取设备实时时钟
    CMD_SYS_TEMP_CTRL       = 0x0020, ///< 温度控制：控制设备温度

    // 查询命令 (0x01xx)
    CMD_QUERY_POWER         = 0x0101, ///< 查询电源：获取设备电源状态
    CMD_QUERY_STATUS        = 0x0102, ///< 查询状态：获取设备整体状态
    CMD_QUERY_NETWORK       = 0x0103, ///< 查询网络：获取设备网络状态

    // 电机命令 (0x30xx)
    CMD_MOTOR_ROTATE        = 0x3001, ///< 电机旋转：控制电机旋转到指定绝对位置
    CMD_MOTOR_ENABLE        = 0x3002, ///< 电机使能：使能指定电机
    CMD_MOTOR_DISABLE       = 0x3003, ///< 电机禁用：禁用指定电机
    CMD_MOTOR_STOP          = 0x3004, ///< 电机急停：立即停止指定电机
    CMD_MOTOR_SET_ORIGIN    = 0x3005, ///< 设置原点：将当前位置设为原点
    CMD_MOTOR_GET_POS       = 0x3006, ///< 获取位置：获取电机当前位置
    CMD_MOTOR_SET_VEL       = 0x3007, ///< 设置速度：设置电机速度
    CMD_MOTOR_ROTATE_REL    = 0x3008, ///< 相对旋转：控制电机相对当前位置旋转
    CMD_MOTOR_GET_ALL       = 0x3100, ///< 获取所有电机状态：获取所有电机的当前状态

    // 电机参数命令 (0x31xx)
    CMD_MOTOR_READ_REG      = 0x3101, ///< 读取寄存器：读取电机控制器寄存器
    CMD_MOTOR_WRITE_REG     = 0x3102, ///< 写入寄存器：写入电机控制器寄存器
    CMD_MOTOR_SAVE_FLASH    = 0x3103, ///< 保存到 Flash：将电机参数保存到Flash
    CMD_MOTOR_REFRESH       = 0x3104, ///< 刷新状态：刷新电机状态信息
    CMD_MOTOR_CLEAR_ERROR   = 0x3105, ///< 清除错误：清除电机错误状态

    // 传感器命令 (0x40xx)
    CMD_SENSOR_READ_TEMP    = 0x4001, ///< 读取温度：读取指定温度传感器
    CMD_SENSOR_READ_ALL     = 0x4002, ///< 读取所有传感器：读取所有传感器数据
    CMD_SENSOR_CONFIG       = 0x4010, ///< 传感器配置：配置传感器参数

    // 设备命令 (0x50xx)
    CMD_DEV_HEATER          = 0x5001, ///< 加热器控制：控制加热器开关
    CMD_DEV_FAN             = 0x5002, ///< 风扇控制：控制风扇开关
    CMD_DEV_LED             = 0x5003, ///< LED 控制：控制LED状态
    CMD_DEV_LASER           = 0x5004, ///< 激光控制：控制激光开关
    CMD_DEV_PWM_LIGHT       = 0x5005, ///< PWM 补光灯控制：控制补光灯亮度
    CMD_DEV_MOTOR_POWER     = 0x5006, ///< 电机供电控制：控制电机电源开关
    CMD_DEV_GET_STATE       = 0x5010, ///< 获取设备状态：获取设备当前状态

    // 文件传输命令 (0x60xx) - CV610发送给Air8000的请求
    CMD_FILE_TRANSFER_REQUEST = 0x6020,   ///< CV610请求Air8000发送文件
    CMD_FILE_TRANSFER_ACK     = 0x6021,   ///< CV610确认收到分片
    CMD_FILE_TRANSFER_COMPLETE = 0x6022,  ///< CV610确认文件传输完成
    CMD_FILE_TRANSFER_ERROR   = 0x6023,   ///< CV610通知传输错误
    CMD_FILE_TRANSFER_CANCEL   = 0x6024,   ///< CV610取消文件传输
    CMD_FILE_TRANSFER_DATA     = 0x6025,   ///< Air8000发送文件分片
    CMD_FILE_TRANSFER_STATUS   = 0x6026,   ///< Air8000发送文件传输状态通知
    CMD_FILE_TRANSFER_START    = 0x6027,   ///< Air8000开始文件传输
    
    // FOTA升级命令 (0x601x)
    CMD_OTA_UART_START  = 0x6010,  ///< 开始串口升级，数据=[firmware_size u32 大端序]
    CMD_OTA_UART_DATA   = 0x6011,  ///< 固件数据包，数据=[seq u16 大端序][data...]
    CMD_OTA_UART_FINISH = 0x6012,  ///< 升级完成
    CMD_OTA_UART_ABORT  = 0x6013,  ///< 取消升级
    CMD_OTA_UART_STATUS = 0x6014,  ///< 串口升级状态通知

} air8000_command_t;

/**
 * @brief 电机 ID 枚举
 * @details 定义了系统中可用的电机标识符
 */
typedef enum {
    MOTOR_ID_Y   = 0x01,   ///< Y轴电机
    MOTOR_ID_X   = 0x02,   ///< X轴电机
    MOTOR_ID_Z   = 0x03,   ///< Z轴电机
    MOTOR_ID_ALL = 0xFF    ///< 所有电机
} air8000_motor_id_t;

/**
 * @brief 设备 ID 枚举
 * @details 定义了系统中可控设备的标识符
 */
typedef enum {
    DEVICE_ID_HEATER1   = 0x01,   ///< 加热器1
    DEVICE_ID_HEATER2   = 0x02,   ///< 加热器2
    DEVICE_ID_FAN1      = 0x10,   ///< 风扇1
    DEVICE_ID_LED       = 0x20,   ///< LED指示灯
    DEVICE_ID_LASER     = 0x30,   ///< 激光模块
    DEVICE_ID_PWM_LIGHT = 0x40    ///< PWM补光灯
} air8000_device_id_t;

/**
 * @brief 设备状态枚举
 * @details 定义了设备的不同状态
 */
typedef enum {
    DEVICE_STATE_OFF    = 0x00,   ///< 关闭状态
    DEVICE_STATE_ON     = 0x01,   ///< 开启状态
    DEVICE_STATE_BLINK  = 0x02    ///< 闪烁状态
} air8000_device_state_t;

/**
 * @brief 错误码枚举
 * @details 定义了设备可能返回的错误类型
 */
typedef enum {
    ERROR_UNKNOWN_CMD       = 0x01, ///< 未知命令：设备不支持的命令码
    ERROR_INVALID_PARAM     = 0x02, ///< 无效参数：命令参数错误
    ERROR_DEVICE_BUSY       = 0x03, ///< 设备忙：设备当前无法处理命令
    ERROR_NOT_READY         = 0x04, ///< 未就绪：设备未准备好接收命令
    ERROR_EXEC_FAILED       = 0x05, ///< 执行失败：命令执行失败
    ERROR_TIMEOUT           = 0x06, ///< 超时：命令执行超时
    ERROR_CRC_ERROR         = 0x07, ///< CRC错误：接收到的帧CRC校验失败
    ERROR_VERSION_UNSUPPORTED = 0x08 ///< 版本不支持：协议版本不兼容
} air8000_error_code_t;

/**
 * @brief 电机寄存器地址枚举
 * @details 定义了电机控制器的寄存器地址
 */
typedef enum {
    MOTOR_REG_UV_VALUE   = 0x00, ///< 低压保护值
    MOTOR_REG_KT_VALUE   = 0x01, ///< 扭矩系数
    MOTOR_REG_OT_VALUE   = 0x02, ///< 过温保护值
    MOTOR_REG_OC_VALUE   = 0x03, ///< 过流保护值
    MOTOR_REG_ACC        = 0x04, ///< 加速度
    MOTOR_REG_DEC        = 0x05, ///< 减速度
    MOTOR_REG_MAX_SPD    = 0x06, ///< 最大速度
    MOTOR_REG_MST_ID     = 0x07, ///< 反馈 ID
    MOTOR_REG_ESC_ID     = 0x08, ///< 接收 ID
    MOTOR_REG_TIMEOUT    = 0x09, ///< 超时时间
    MOTOR_REG_CTRL_MODE  = 0x0A, ///< 控制模式
    MOTOR_REG_PMAX       = 0x15, ///< 位置范围
    MOTOR_REG_VMAX       = 0x16, ///< 速度范围
    MOTOR_REG_TMAX       = 0x17, ///< 扭矩范围
    MOTOR_REG_I_BW       = 0x18, ///< 电流带宽
    MOTOR_REG_KP_ASR     = 0x19, ///< 速度环 Kp
    MOTOR_REG_KI_ASR     = 0x1A, ///< 速度环 Ki
    MOTOR_REG_KP_APR     = 0x1B, ///< 位置环 Kp
    MOTOR_REG_KI_APR     = 0x1C, ///< 位置环 Ki
    MOTOR_REG_OV_VALUE   = 0x1D, ///< 过压保护值
    MOTOR_REG_POSITION_M = 0x50, ///< 当前位置 (只读)
    MOTOR_REG_VELOCITY_M = 0x51, ///< 当前速度 (只读)
    MOTOR_REG_TORQUE_M   = 0x52  ///< 当前扭矩 (只读)
} air8000_motor_reg_t;

// ==================== 结构体定义 ====================

/**
 * @brief 通信帧结构
 * @details 定义了通信帧的基本格式，用于数据传输
 */
typedef struct {
    uint8_t version;    ///< 协议版本号，固定为AIR8000_VERSION
    uint8_t type;       ///< 帧类型，使用air8000_frame_type_t枚举值
    uint8_t seq;        ///< 帧序列号，用于匹配请求和响应
    uint16_t cmd;       ///< 命令码，使用air8000_command_t枚举值
    uint16_t data_len;  ///< 数据长度，单位字节
    uint8_t *data;      ///< 数据指针，动态分配，需要手动释放
} air8000_frame_t;

// --- 响应数据结构 ---

/**
 * @brief 版本信息结构体
 * @details 存储设备固件版本信息
 */
typedef struct {
    uint8_t major;      ///< 主版本号
    uint8_t minor;      ///< 次版本号
    uint8_t patch;      ///< 补丁版本号
    char build[32];     ///< 构建信息字符串
} air8000_version_t;

/**
 * @brief 网络状态结构体
 * @details 存储设备网络状态信息
 */
typedef struct {
    uint8_t csq;        ///< 信号强度，范围0-31
    int8_t rssi;        ///< 接收信号强度指示，单位dBm
    int8_t rsrp;        ///< 参考信号接收功率，单位dBm
    uint8_t status;     ///< 网络状态码
    uint8_t operator_id; ///< 运营商ID
    char iccid[21];     ///< ICCID卡号，20位数字
    char ip[16];        ///< IP地址字符串
} air8000_network_status_t;

/**
 * @brief 电源 ADC 结构体
 * @details 存储设备电源电压信息
 */
typedef struct {
    uint16_t v12_mv;    ///< 12V电压，单位毫伏
    uint16_t vbat_mv;   ///< 电池电压，单位毫伏
} air8000_power_adc_t;

/**
 * @brief 传感器数据结构体
 * @details 存储传感器采集的环境数据
 */
typedef struct {
    float temperature;  ///< 温度，单位摄氏度
    uint8_t humidity;   ///< 湿度，范围0-100%
    uint8_t light;      ///< 光照强度，范围0-255
    uint8_t battery;    ///< 电池电量，范围0-100%
} air8000_sensor_data_t;

/**
 * @brief 单个电机状态结构体
 * @details 存储单个电机的状态信息
 */
typedef struct {
    uint8_t motor_id;   ///< 电机ID
    uint8_t action;     ///< 电机动作状态
    uint16_t speed;     ///< 电机速度
} air8000_motor_state_item_t;

/**
 * @brief 所有电机状态结构体
 * @details 存储所有电机的状态信息数组
 */
typedef struct {
    size_t count;                       ///< 电机数量
    air8000_motor_state_item_t *motors; ///< 电机状态数组指针
} air8000_all_motor_status_t;

/**
 * @brief 看门狗配置结构体
 * @details 存储看门狗的配置参数
 */
typedef struct {
    bool enable;            ///< 是否启用看门狗
    uint16_t timeout_sec;   ///< 看门狗超时时间，单位秒
    uint8_t power_off_sec;  ///< 断电前等待时间，单位秒
} air8000_wdt_config_t;

/**
 * @brief 看门狗状态结构体
 * @details 存储看门狗的当前状态
 */
typedef struct {
    bool enable;            ///< 是否启用看门狗
    uint16_t timeout_sec;   ///< 看门狗超时时间，单位秒
    uint8_t power_off_sec;  ///< 断电前等待时间，单位秒
    uint16_t remaining_sec; ///< 剩余超时时间，单位秒
    uint8_t reset_count;    ///< 复位计数
} air8000_wdt_status_t;

// ==================== 文件传输相关数据结构 ====================

/**
 * @brief 文件传输状态枚举
 * @details 描述文件传输的不同状态
 */
typedef enum {
    FILE_TRANSFER_IDLE = 0,         ///< 空闲状态
    FILE_TRANSFER_NOTIFIED,         ///< 已通知，等待确认
    FILE_TRANSFER_STARTED,          ///< 传输开始
    FILE_TRANSFER_TRANSMITTING,     ///< 传输中
    FILE_TRANSFER_COMPLETED,        ///< 传输完成
    FILE_TRANSFER_ERROR,            ///< 传输错误
    FILE_TRANSFER_CANCELLED         ///< 传输取消
} air8000_file_transfer_state_t;

/**
 * @brief 文件传输信息结构体
 * @details 用于文件传输开始时传递文件信息
 */
typedef struct {
    char filename[256];     ///< 文件名
    uint64_t file_size;     ///< 文件大小，单位字节
    uint32_t block_size;    ///< 分片大小，单位字节
    uint32_t crc32;         ///< 文件CRC32校验值
    uint8_t file_type;      ///< 文件类型
} air8000_file_info_t;

/**
 * @brief 文件分片数据结构体
 * @details 用于传输文件的分片数据
 */
typedef struct {
    uint32_t block_index;   ///< 分片索引，从0开始
    uint32_t data_len;      ///< 分片数据长度，最后一个分片可能小于block_size
    uint32_t crc32;         ///< 分片CRC32校验值
    uint8_t data[];         ///< 分片数据，长度为data_len
} air8000_file_block_t;

/**
 * @brief FOTA升级状态枚举
 */
typedef enum {
    FOTA_STATUS_IDLE = 0,           ///< 空闲
    FOTA_STATUS_RECEIVING = 1,      ///< 接收中
    FOTA_STATUS_VERIFYING = 2,      ///< 校验中
    FOTA_STATUS_SUCCESS = 3,        ///< 成功
    FOTA_STATUS_FAILED = 4,         ///< 失败
} air8000_fota_status_t;

/**
 * @brief FOTA升级错误码枚举
 */
typedef enum {
    FOTA_ERROR_NONE = 0,           ///< 无错误
    FOTA_ERROR_INIT_FAILED = 1,    ///< 初始化失败
    FOTA_ERROR_SEQ_ERROR = 2,      ///< 序号错误
    FOTA_ERROR_WRITE_FAILED = 3,   ///< 写入失败
    FOTA_ERROR_VERIFY_FAILED = 4,  ///< 校验失败
    FOTA_ERROR_TIMEOUT = 5,        ///< 超时
    FOTA_ERROR_ABORTED = 6,        ///< 已取消
    FOTA_ERROR_SIZE_MISMATCH = 7,  ///< 大小不匹配
} air8000_fota_error_t;

/**
 * @brief FOTA升级状态信息结构体
 */
typedef struct {
    air8000_fota_status_t status;     ///< 升级状态
    air8000_fota_error_t error;       ///< 错误码
    uint8_t progress;                 ///< 进度百分比
} air8000_fota_status_info_t;

// ==================== 函数声明 ====================

/**
 * @brief CRC16/MODBUS 校验计算
 * @param data 要计算CRC的数据指针
 * @param len 数据长度，单位字节
 * @return 计算得到的CRC16值
 */
uint16_t air8000_crc16_modbus(const uint8_t *data, size_t len);

/**
 * @brief 初始化帧结构
 * @param frame 要初始化的帧指针
 */
void air8000_frame_init(air8000_frame_t *frame);

/**
 * @brief 释放帧内部动态分配的内存
 * @param frame 要清理的帧指针
 * @note 仅释放通过build系列函数构建的帧内部数据
 */
void air8000_frame_cleanup(air8000_frame_t *frame);

/**
 * @brief 编码帧数据
 * @param frame 要编码的帧指针
 * @param buffer 输出缓冲区指针
 * @param max_len 输出缓冲区最大长度
 * @return 成功返回编码后的总长度，失败返回负数
 */
int air8000_frame_encode(const air8000_frame_t *frame, uint8_t *buffer, size_t max_len);

/**
 * @brief 解析帧数据
 * @param buffer 要解析的原始数据指针
 * @param len 原始数据长度
 * @param frame 输出帧指针
 * @return 成功返回解析出的帧长度，失败返回负数
 */
int air8000_frame_parse(const uint8_t *buffer, size_t len, air8000_frame_t *frame);

/**
 * @brief 生成下一个帧序列号
 * @return 下一个序列号，范围0-255，循环递增
 */
uint8_t air8000_next_seq(void);

/**
 * @brief 网络字节序转主机字节序（浮点型）
 * @param val 网络字节序的浮点值（32位）
 * @return 主机字节序的浮点值
 */
float air8000_ntohf(uint32_t val);

/**
 * @brief 主机字节序转网络字节序（浮点型）
 * @param val 主机字节序的浮点值
 * @return 网络字节序的浮点值（32位）
 */
uint32_t air8000_htonf(float val);

// ==================== 帧构建辅助函数 ====================

/**
 * @brief 构建通用请求帧
 * @param frame 帧对象指针
 * @param cmd 命令码
 * @param data 数据内容指针
 * @param len 数据长度，单位字节
 */
void air8000_build_request(air8000_frame_t *frame, uint16_t cmd, const uint8_t *data, size_t len);

// --- 系统命令 ---

/**
 * @brief 构建 PING 请求帧
 * @param frame 帧对象指针
 */
void air8000_build_ping(air8000_frame_t *frame);

/**
 * @brief 构建获取版本请求帧
 * @param frame 帧对象指针
 */
void air8000_build_sys_version(air8000_frame_t *frame);

/**
 * @brief 构建系统复位请求帧
 * @param frame 帧对象指针
 */
void air8000_build_sys_reset(air8000_frame_t *frame);

/**
 * @brief 构建查询电源请求帧
 * @param frame 帧对象指针
 */
void air8000_build_query_power(air8000_frame_t *frame);

/**
 * @brief 构建查询状态请求帧
 * @param frame 帧对象指针
 */
void air8000_build_query_status(air8000_frame_t *frame);

/**
 * @brief 构建查询网络请求帧
 * @param frame 帧对象指针
 */
void air8000_build_query_network(air8000_frame_t *frame);

// --- 电机控制命令 ---

/**
 * @brief 构建电机旋转请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 * @param angle 目标角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 */
void air8000_build_motor_rotate(air8000_frame_t *frame, uint8_t motor_id, float angle, float velocity);

/**
 * @brief 构建电机使能请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_enable(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建电机禁用请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_disable(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建电机急停请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_stop(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建设置原点请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_set_origin(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建获取位置请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_get_pos(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建设置速度请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 * @param velocity 目标速度，单位弧度/秒
 */
void air8000_build_motor_set_vel(air8000_frame_t *frame, uint8_t motor_id, float velocity);

/**
 * @brief 构建相对旋转请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 * @param angle 相对旋转角度，单位弧度
 * @param velocity 旋转速度，单位弧度/秒
 */
void air8000_build_motor_rotate_rel(air8000_frame_t *frame, uint8_t motor_id, float angle, float velocity);

/**
 * @brief 构建获取所有电机状态请求帧
 * @param frame 帧对象指针
 */
void air8000_build_motor_get_all(air8000_frame_t *frame);

/**
 * @brief 构建电机供电控制请求帧
 * @param frame 帧对象指针
 * @param power_on 供电状态，true为开启，false为关闭
 */
void air8000_build_motor_power(air8000_frame_t *frame, bool power_on);

// --- 电机参数命令 ---

/**
 * @brief 构建读取电机寄存器请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 * @param reg_addr 寄存器地址
 */
void air8000_build_motor_read_reg(air8000_frame_t *frame, uint8_t motor_id, uint8_t reg_addr);

/**
 * @brief 构建写入电机寄存器请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 * @param reg_addr 寄存器地址
 * @param value 要写入的值
 */
void air8000_build_motor_write_reg(air8000_frame_t *frame, uint8_t motor_id, uint8_t reg_addr, float value);

/**
 * @brief 构建保存电机参数到Flash请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_save_flash(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建刷新电机状态请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_refresh(air8000_frame_t *frame, uint8_t motor_id);

/**
 * @brief 构建清除电机错误请求帧
 * @param frame 帧对象指针
 * @param motor_id 电机ID
 */
void air8000_build_motor_clear_error(air8000_frame_t *frame, uint8_t motor_id);

// --- 传感器与设备命令 ---

/**
 * @brief 构建读取传感器请求帧
 * @param frame 帧对象指针
 * @param sensor_id 传感器ID
 */
void air8000_build_sensor_read(air8000_frame_t *frame, uint8_t sensor_id);

/**
 * @brief 构建读取所有传感器请求帧
 * @param frame 帧对象指针
 */
void air8000_build_sensor_read_all(air8000_frame_t *frame);

/**
 * @brief 构建设备控制请求帧
 * @param frame 帧对象指针
 * @param cmd 设备控制命令码
 * @param device_id 设备ID
 * @param state 设备状态
 */
void air8000_build_dev_ctrl(air8000_frame_t *frame, uint16_t cmd, uint8_t device_id, uint8_t state);

/**
 * @brief 构建获取设备状态请求帧
 * @param frame 帧对象指针
 * @param device_id 设备ID
 */
void air8000_build_dev_get_state(air8000_frame_t *frame, uint8_t device_id);

// ==================== 响应解析辅助函数 ====================

/**
 * @brief 解析版本信息响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出版本信息结构体指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_version(const uint8_t *data, size_t len, air8000_version_t *out);

/**
 * @brief 解析网络状态响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出网络状态结构体指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_network_status(const uint8_t *data, size_t len, air8000_network_status_t *out);

/**
 * @brief 解析电源ADC响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出电源ADC结构体指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_power_adc(const uint8_t *data, size_t len, air8000_power_adc_t *out);

/**
 * @brief 解析传感器数据响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出传感器数据结构体指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_sensor_data(const uint8_t *data, size_t len, air8000_sensor_data_t *out);

/**
 * @brief 解析所有电机状态响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param out 输出所有电机状态结构体指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_all_motor_status(const uint8_t *data, size_t len, air8000_all_motor_status_t *out);

/**
 * @brief 解析单个电机的浮点型响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机ID指针，可为NULL
 * @param value 输出浮点值指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_motor_float_resp(const uint8_t *data, size_t len, uint8_t *motor_id, float *value);

/**
 * @brief 解析电机寄存器读取响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机ID指针，可为NULL
 * @param reg_id 输出寄存器ID指针，可为NULL
 * @param value 输出寄存器值指针
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_motor_read_reg(const uint8_t *data, size_t len, uint8_t *motor_id, uint8_t *reg_id, float *value);

/**
 * @brief 解析电机状态刷新响应
 * @param data 响应数据指针
 * @param len 数据长度，单位字节
 * @param motor_id 输出电机ID指针，可为NULL
 * @param pos 输出位置值指针，可为NULL
 * @param vel 输出速度值指针，可为NULL
 * @param torque 输出扭矩值指针，可为NULL
 * @param temp_mos 输出MOS温度指针，可为NULL
 * @param temp_rotor 输出转子温度指针，可为NULL
 * @param error 输出错误码指针，可为NULL
 * @param enabled 输出使能状态指针，可为NULL
 * @return 成功返回0，失败返回负数
 */
int air8000_parse_motor_refresh(const uint8_t *data, size_t len, uint8_t *motor_id, float *pos, float *vel, float *torque, uint8_t *temp_mos, uint8_t *temp_rotor, uint8_t *error, bool *enabled);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_PROTOCOL_H
