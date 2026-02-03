/**
 * @file air8000_serial.c
 * @brief 串口通信实现 (POSIX)
 * @details 基于 POSIX 接口实现的串口通信模块，支持打开、关闭、发送、接收和清空缓冲区等功能
 * 
 * 串口抽象层设计：
 * 1. **跨平台设计**：基于 POSIX 标准接口实现，便于移植到不同系统
 * 2. **模块化设计**：将串口操作封装为独立模块，与上层协议解耦
 * 3. **异步通信支持**：提供超时机制，支持异步通信模式
 * 4. **配置灵活性**：支持不同波特率、数据位、停止位和校验方式的配置
 * 5. **调试友好**：提供数据发送和接收的日志输出
 * 
 * 实现特点：
 * - 使用 poll 机制实现非阻塞读取和超时控制
 * - 支持原始模式通信，适合二进制数据传输
 * - 实现了硬件流控禁用，适合简单通信场景
 * - 支持 DTR 线控制，适应不同硬件需求
 * - 提供完整的错误处理机制
 */

#include "air8000_serial.h"    /* 串口抽象层头文件 */
#include "air8000_log.h"        /* 统一日志头文件 */
#include <stdio.h>             /* 标准输入输出 */
#include <stdlib.h>            /* 标准库函数 */
#include <string.h>            /* 字符串处理函数 */
#include <unistd.h>            /* UNIX 标准函数 */
#include <fcntl.h>             /* 文件控制函数 */
#include <errno.h>             /* 错误处理 */
#include <termios.h>           /* 终端I/O控制 */
#include <sys/ioctl.h>         /* I/O 控制函数 */
#include <sys/types.h>         /* 系统类型定义 */
#include <sys/stat.h>          /* 文件状态函数 */
#include <poll.h>              /* 用于实现超时控制 */
#include <time.h>              /* 时间函数 */
#include <ctype.h>             /* 字符处理函数 */

// 平台特定的 DTR 处理
#ifdef __linux__
#include <linux/serial.h>       /* Linux 特定的串口头文件 */
#endif

/**
 * @brief 打开串口设备
 * @param serial 串口对象指针
 * @param path 串口设备路径，如 "/dev/ttyACM2"
 * @return 成功返回 0，失败返回 -1
 * @details 打开串口设备，配置串口参数，设置为原始模式，无校验，8数据位，1停止位
 */
int air8000_serial_open(air8000_serial_t *serial, const char *path) {
    /* 参数有效性检查 */
    if (!serial || !path) return -1;

    /* 保存设备路径 */
    strncpy(serial->device_path, path, sizeof(serial->device_path) - 1);
    serial->fd = -1;           /* 初始化为无效文件描述符 */

    /* 打开串口：读写模式，不作为控制终端，非阻塞模式(避免打开时阻塞) */
    serial->fd = open(path, O_RDWR | O_NOCTTY | O_NDELAY);
    if (serial->fd < 0) {
        perror("open serial"); /* 打印错误信息 */
        serial->fd = -1;
        return -1;            /* 打开失败，返回 -1 */
    }

    /* 清除 O_NDELAY 标志，使其恢复为阻塞模式 (我们通过 poll 控制超时) */
    fcntl(serial->fd, F_SETFL, 0);

    struct termios tty;        /* 终端属性结构体 */
    /* 获取当前终端属性 */
    if (tcgetattr(serial->fd, &tty) != 0) {
        perror("tcgetattr");  /* 打印错误信息 */
        close(serial->fd);
        serial->fd = -1;
        return -1;            /* 获取失败，返回 -1 */
    }

    /* 配置为原始模式 (Raw mode)，不进行任何字符处理 */
    cfmakeraw(&tty);

    /* 无校验，8数据位，1停止位 */
    tty.c_cflag &= ~PARENB;     /* 清除校验位 */
    tty.c_cflag &= ~CSTOPB;     /* 清除2停止位，使用1停止位 */
    tty.c_cflag &= ~CSIZE;      /* 清除数据位设置 */
    tty.c_cflag |= CS8;         /* 设置8数据位 */
    
    /* 禁用硬件流控 */
    tty.c_cflag &= ~CRTSCTS;

    /* 启用接收，忽略调制解调器控制线 */
    tty.c_cflag |= (CREAD | CLOCAL);

    /* VMIN=0, VTIME=0: 读取立即返回 (Polling 模式) */
    /* 我们将使用 poll() 来处理超时 */
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    /* 设置波特率为 115200 */
    cfsetospeed(&tty, B115200); /* 设置输出波特率 */
    cfsetispeed(&tty, B115200); /* 设置输入波特率 */

    /* 应用终端属性 */
    if (tcsetattr(serial->fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");  /* 打印错误信息 */
        close(serial->fd);
        serial->fd = -1;
        return -1;            /* 设置失败，返回 -1 */
    }

    /* 设置 DTR 为高电平 (Air8000 设备要求) */
    int status;
    if (ioctl(serial->fd, TIOCMGET, &status) == 0) {
        status |= TIOCM_DTR;   /* 设置 DTR 位 */
        ioctl(serial->fd, TIOCMSET, &status);
    }

    /* 清空旧数据，包括输入和输出缓冲区 */
    tcflush(serial->fd, TCIOFLUSH);

    return 0;                 /* 成功打开，返回 0 */
}

/**
 * @brief 关闭串口设备
 * @param serial 串口对象指针
 * @details 关闭串口文件描述符，释放资源
 */
void air8000_serial_close(air8000_serial_t *serial) {
    /* 参数有效性检查 */
    if (serial && serial->fd >= 0) {
        close(serial->fd);     /* 关闭文件描述符 */
        serial->fd = -1;       /* 标记为无效 */
    }
}

/**
 * @brief 向串口发送数据
 * @param serial 串口对象指针
 * @param data 要发送的数据指针
 * @param len 数据长度，单位字节
 * @return 成功返回实际发送的字节数，失败返回 -1
 * @details 向串口发送指定长度的数据，支持调试输出
 */
int air8000_serial_write(air8000_serial_t *serial, const uint8_t *data, size_t len) {
    /* 参数有效性检查 */
    if (!serial || serial->fd < 0) return -1;

    /* 获取当前时间，用于日志 */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    /* 打印发送的数据（调试用） */
    log_info("serial", "[%s] Sending %zu bytes:", time_str, len);
    
    /* 打印十六进制数据 */
    char hex_buf[3 * len + 1];
    hex_buf[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        char byte[4];
        sprintf(byte, "%02X ", data[i]);
        strcat(hex_buf, byte);
    }
    log_info("serial", "[%s] HEX: %s", time_str, hex_buf);
    
    /* 发送数据 */
    ssize_t written = write(serial->fd, data, len);
    if (written < 0) {
        log_error("serial", "[%s] Write failed: %s", time_str, strerror(errno));
        return -1;            /* 发送失败，返回 -1 */
    }

    /* 确保数据已发送到硬件，等待输出缓冲区清空 */
    tcdrain(serial->fd);

    log_info("serial", "[%s] Successfully sent %zd bytes", time_str, written);
    return (int)written;       /* 成功发送，返回实际发送的字节数 */
}

/**
 * @brief 从串口读取数据
 * @param serial 串口对象指针
 * @param buffer 接收缓冲区指针
 * @param len 缓冲区最大长度
 * @param timeout_ms 超时时间，单位毫秒
 * @return 成功返回实际读取的字节数，超时返回 0，失败返回 -1
 * @details 从串口读取数据，支持超时控制，使用 poll 机制实现
 */
int air8000_serial_read(air8000_serial_t *serial, uint8_t *buffer, size_t len, int timeout_ms) {
    /* 参数有效性检查 */
    if (!serial || serial->fd < 0) return -1;

    struct pollfd pfd;         /* poll 文件描述符结构体 */
    pfd.fd = serial->fd;       /* 设置要监听的文件描述符 */
    pfd.events = POLLIN;       /* 监听可读事件 */

    /* 调用 poll 等待数据，超时时间为 timeout_ms */
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        /* 获取当前时间，用于日志 */
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        char time_str[20];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        
        log_error("serial", "[%s] Poll failed: %s", time_str, strerror(errno));
        return -1;            /* poll 失败，返回 -1 */
    } else if (ret == 0) {
        return 0;             /* 超时，返回 0 */
    }

    /* 检查是否有可读数据 */
    if (pfd.revents & POLLIN) {
        /* 读取数据 */
        ssize_t n = read(serial->fd, buffer, len);
        if (n < 0) {
            /* 获取当前时间，用于日志 */
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            log_error("serial", "[%s] Read failed: %s", time_str, strerror(errno));
            return -1;        /* 读取失败，返回 -1 */
        }
        
        /* 打印接收的数据（调试用） */
        if (n > 0) {
            /* 获取当前时间，用于日志 */
            time_t now = time(NULL);
            struct tm *tm_info = localtime(&now);
            char time_str[20];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
            
            log_info("serial", "[%s] Received %zd bytes:", time_str, n);
            
            /* 打印十六进制数据 */
            char hex_buf[3 * n + 1];
            hex_buf[0] = '\0';
            for (size_t i = 0; i < (size_t)n; i++) {
                char byte[4];
                sprintf(byte, "%02X ", buffer[i]);
                strcat(hex_buf, byte);
            }
            log_info("serial", "[%s] HEX: %s", time_str, hex_buf);
            
            fflush(stdout);   /* 立即刷新输出缓冲区 */
        }

        return (int)n;         /* 成功读取，返回实际读取的字节数 */
    }

    return 0;                 /* 没有可读事件，返回 0 */
}

/**
 * @brief 清空串口缓冲区
 * @param serial 串口对象指针
 * @details 清空串口的输入和输出缓冲区
 */
void air8000_serial_flush(air8000_serial_t *serial) {
    /* 参数有效性检查 */
    if (serial && serial->fd >= 0) {
        /* 清空输入和输出缓冲区 */
        tcflush(serial->fd, TCIOFLUSH);
    }
}
