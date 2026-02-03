/**
 * @file air8000_serial.h
 * @brief 串口通信抽象层头文件
 * @details 定义了跨平台的串口通信接口，屏蔽了不同操作系统的串口实现差异
 * 
 * 串口抽象层设计：
 * 1. **封装设计**：
 *    - 封装底层串口操作，提供统一的 API 接口
 *    - 屏蔽不同操作系统的实现差异
 *    - 支持多种串口配置参数
 * 2. **接口设计**：
 *    - 打开/关闭串口
 *    - 发送/接收数据
 *    - 清空缓冲区
 * 3. **实现特点**：
 *    - 支持超时控制
 *    - 提供错误处理机制
 *    - 支持非阻塞操作
 *    - 适配不同硬件需求
 * 4. **使用场景**：
 *    - 与 Air8000 设备进行串口通信
 *    - 适用于需要可靠串口通信的场景
 *    - 支持长时间稳定运行
 */

#ifndef AIR8000_SERIAL_H
#define AIR8000_SERIAL_H

#include <stddef.h>     /* 标准库头文件，定义了size_t等类型 */
#include <stdint.h>     /* 标准库头文件，定义了uint8_t等类型 */
#include <stdbool.h>    /* 标准库头文件，定义了bool类型 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 串口设备对象结构体
 * @details 包含了串口通信所需的基本信息
 */
typedef struct {
    int fd;                     ///< 串口文件描述符
    char device_path[256];      ///< 设备路径，如 "/dev/ttyACM2"
} air8000_serial_t;

/**
 * @brief 打开串口设备
 * @details 初始化串口配置，设置波特率、数据位、停止位等参数
 * @param serial 串口对象指针，用于存储串口状态
 * @param path 设备路径，例如 "/dev/ttyACM2"
 * @return 成功返回 0，失败返回负数错误码
 * @note 调用此函数前，serial 结构体应已分配内存
 */
int air8000_serial_open(air8000_serial_t *serial, const char *path);

/**
 * @brief 关闭串口设备
 * @details 关闭文件描述符，释放串口资源
 * @param serial 串口对象指针
 * @note 调用此函数后，serial 对象可以再次用于打开其他串口
 */
void air8000_serial_close(air8000_serial_t *serial);

/**
 * @brief 向串口发送数据
 * @details 阻塞发送指定长度的数据到串口
 * @param serial 串口对象指针
 * @param data 要发送的数据缓冲区指针
 * @param len 要发送的数据长度，单位字节
 * @return 成功返回实际发送的字节数，失败返回负数错误码
 */
int air8000_serial_write(air8000_serial_t *serial, const uint8_t *data, size_t len);

/**
 * @brief 从串口读取数据
 * @details 根据指定的超时时间从串口读取数据
 * @param serial 串口对象指针
 * @param buffer 用于存储接收数据的缓冲区指针
 * @param len 缓冲区长度，单位字节
 * @param timeout_ms 超时时间，单位毫秒
 *        - > 0: 阻塞等待，直到有数据或超时
 *        - 0: 非阻塞模式，立即返回
 *        - < 0: 无限阻塞等待
 * @return 成功返回实际读取的字节数，失败返回负数错误码，超时返回 0
 */
int air8000_serial_read(air8000_serial_t *serial, uint8_t *buffer, size_t len, int timeout_ms);

/**
 * @brief 清空串口缓冲区
 * @details 清空串口的输入和输出缓冲区
 * @param serial 串口对象指针
 */
void air8000_serial_flush(air8000_serial_t *serial);

#ifdef __cplusplus
}
#endif

#endif // AIR8000_SERIAL_H
