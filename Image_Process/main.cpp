/**
 * @file main.cpp
 * @brief 海思CV610平台图像处理程序主入口
 * @author TraeAI
 * @date 2026-01-22
 * 
 * 本文件实现了适用于海思CV610 Linux平台的图像处理程序主入口，
 * 支持命令行参数解析，提供图像处理功能，
 * 可使用标定参数进行图像校正。
 */

#include "image_processor.h"
#include <iostream>
#include <vector>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <sys/stat.h>

using namespace std;

// 日志宏定义
#define LOGI(...) fprintf(stdout, "[INFO] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGW(...) fprintf(stdout, "[WARN] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n")

/**
 * @brief 打印程序帮助信息
 * 
 * @param prog_name 程序名称
 */
static void print_help(const char* prog_name) {
    fprintf(stdout, "\n海思CV610平台图像处理程序\n");
    fprintf(stdout, "====================================\n");
    fprintf(stdout, "用法: %s [选项]\n\n", prog_name);
    fprintf(stdout, "图像处理选项:\n");
    fprintf(stdout, "  -i, --input <path>      输入图像文件或文件夹路径\n");
    fprintf(stdout, "  -o, --output <dir>      输出结果目录\n");
    fprintf(stdout, "  -u, --use-calib         使用标定参数进行图像校正\n");
    fprintf(stdout, "  -m, --max <num>         最大处理图像数量（默认：无限制）\n\n");
    fprintf(stdout, "通用选项:\n");
    fprintf(stdout, "  -h, --help              显示此帮助信息\n");
    fprintf(stdout, "\n示例:\n");
    fprintf(stdout, "  # 处理单个图像文件\n");
    fprintf(stdout, "  %s -i input.jpg -o output -u\n\n", prog_name);
    fprintf(stdout, "  # 处理文件夹中的图像\n");
    fprintf(stdout, "  %s -i input_dir -o output -u -m 10\n\n", prog_name);
}

/**
 * @brief 检查文件或目录是否存在
 * 
 * @param path 文件或目录路径
 * @return bool 是否存在
 */
static bool path_exists(const string& path) {
    struct stat st;
    return (stat(path.c_str(), &st) == 0);
}

/**
 * @brief 主程序入口
 * 
 * @param argc 命令行参数数量
 * @param argv 命令行参数列表
 * @return int 程序退出码
 */
int main(int argc, char* argv[]) {
    // 命令行参数默认值
    string input_path;          // 输入图像文件或文件夹路径
    string output_dir = "./output";  // 输出结果目录
    bool use_calibration = false;  // 是否使用标定参数
    int max_images = 0;         // 最大处理图像数量（0表示无限制）
    
    // 长选项结构体
    struct option long_options[] = {
        {"input", required_argument, NULL, 'i'},
        {"output", required_argument, NULL, 'o'},
        {"use-calib", no_argument, NULL, 'u'},
        {"max", required_argument, NULL, 'm'},
        {"help", no_argument, NULL, 'h'},
        {0, 0, 0, 0}
    };
    
    // 解析命令行参数
    int opt = 0;
    int long_index = 0;
    while ((opt = getopt_long(argc, argv, "i:o:um:h", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'i':
                input_path = optarg;
                break;
            case 'o':
                output_dir = optarg;
                break;
            case 'u':
                use_calibration = true;
                break;
            case 'm':
                max_images = atoi(optarg);
                break;
            case 'h':
                print_help(argv[0]);
                return 0;
            default:
                print_help(argv[0]);
                return 1;
        }
    }
    
    // 图像处理模式
    LOGI("运行在图像处理模式");
    
    // 检查输入路径是否存在
    if (input_path.empty()) {
        LOGE("请使用 -i/--input 指定输入图像文件或文件夹");
        print_help(argv[0]);
        return 1;
    }
    
    if (!path_exists(input_path)) {
        LOGE("输入路径不存在: %s", input_path.c_str());
        return 1;
    }
    
    // 检查输入路径是文件还是文件夹
    struct stat st;
    stat(input_path.c_str(), &st);
    
    if (S_ISREG(st.st_mode)) {
        // 处理单个图像文件
        LOGI("处理单个图像文件: %s", input_path.c_str());
        int ret = image_processor_process_image(input_path, output_dir, use_calibration);
        if (ret != 0) {
            LOGE("图像处理失败: %s", input_path.c_str());
            return 1;
        }
        LOGI("图像处理成功，结果已保存到: %s", output_dir.c_str());
    } else if (S_ISDIR(st.st_mode)) {
        // 处理文件夹中的图像
        LOGI("处理文件夹中的图像: %s", input_path.c_str());
        
        // 设置图像处理配置
        image_processor_config_t config;
        config.input_dir = input_path;
        config.output_dir = output_dir;
        config.use_calibration = use_calibration;
        config.max_images = max_images;
        
        int ret = image_processor_process_folder(config);
        if (ret != 0) {
            LOGE("文件夹处理失败");
            return 1;
        }
        LOGI("文件夹处理成功，结果已保存到: %s", output_dir.c_str());
    } else {
        LOGE("输入路径不是文件也不是文件夹: %s", input_path.c_str());
        return 1;
    }
    
    LOGI("程序执行完成");
    return 0;
}
