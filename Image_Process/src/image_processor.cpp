/**
 * @file image_processor.cpp
 * @brief 海思CV610平台OpenCV图像处理算法实现
 * @author TraeAI
 * @date 2026-01-22
 * 
 * 本文件实现了适用于海思CV610 Linux平台的OpenCV图像处理算法，
 * 包括高斯模糊、二值化、形态学去噪、段落检测和测量等功能。
 */

#include "image_processor.h"
#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <cmath>
#include <algorithm>

// OpenCV库头文件
#include <opencv2/core.hpp>       // 核心功能
#include <opencv2/imgproc.hpp>    // 图像处理
#include <opencv2/imgcodecs.hpp>  // 图像编码
#include <opencv2/calib3d.hpp>    // 相机标定

using namespace std;
using namespace cv;

// 日志宏定义
#define LOGI(...) fprintf(stdout, "[INFO] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGW(...) fprintf(stdout, "[WARN] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n")

// 常量定义
#define MAX_IMAGE_WIDTH 640         // 最大图像宽度
#define MAX_IMAGE_HEIGHT 480        // 最大图像高度
#define SCALE_WIDTH_MM 8.0f         // 最左侧矩形的实际宽度（mm）
#define MIN_PARAGRAPH_WIDTH 5       // 最小段落宽度阈值（像素）
#define DEFAULT_CALIB_FILE "/data/calib_params/camera_calibration.xml"  // 默认标定文件路径



/**
 * @brief 检查文件是否为图片文件
 * 
 * @param filename 文件名
 * @return bool 是否为图片文件
 */
static bool is_image_file(const char *filename) {
    const char *extensions[] = {"jpg", "jpeg", "png", "bmp", "gif", NULL};
    const char *ext = strrchr(filename, '.');
    if (!ext) return false;
    ext++;
    
    for (int i = 0; extensions[i]; i++) {
        if (strcasecmp(ext, extensions[i]) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 创建目录（如果不存在）
 * 
 * @param path 目录路径
 * @return int 创建结果，0表示成功，非0表示失败
 */
static int ensure_directory_exists(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        } else {
            LOGE("路径已存在但不是目录: %s", path);
            return -1;
        }
    }
    
    if (mkdir(path, 0777) != 0) {
        LOGE("创建目录失败: %s", path);
        return -1;
    }
    return 0;
}

/**
 * @brief 处理单张图像
 * 
 * 实现完整的图像处理流程：读取图像、畸变校正、高斯模糊、二值化、
 * 形态学去噪、段落检测和测量，并保存处理结果。
 * 
 * @param input_path 输入图像文件路径
 * @param output_dir 输出结果目录
 * @param use_calibration 是否使用标定参数
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_image(const string& input_path, const string& output_dir, bool use_calibration) {
    // 提取文件名（不含扩展名）
    const char *filename = strrchr(input_path.c_str(), '/');
    char base_filename[128] = {0};
    char output_path[512] = {0};
    
    if (!filename) {
        filename = input_path.c_str();
    } else {
        filename++;
    }
    
    // 复制文件名（去掉扩展名）
    strcpy(base_filename, filename);
    char *ext = strrchr(base_filename, '.');
    if (ext) {
        *ext = '\0';
    }
    
    LOGI("开始处理图像: %s", input_path.c_str());
    
    // 确保输出目录存在
    if (ensure_directory_exists(output_dir.c_str()) != 0) {
        return -1;
    }
    
    // 读取图像时直接转换为灰度图
    Mat gray = imread(input_path, IMREAD_GRAYSCALE);
    if (gray.empty()) {
        LOGE("无法读取图片: %s", input_path.c_str());
        return -1;
    }

    // 如果启用标定功能，进行图像校正
    if (use_calibration) {
        // 加载标定参数
        const char *calib_file = DEFAULT_CALIB_FILE;
        FileStorage fs(calib_file, FileStorage::READ);
        if (fs.isOpened()) {
            Mat camera_matrix, dist_coeffs;
            fs["camera_matrix"] >> camera_matrix;
            fs["dist_coeffs"] >> dist_coeffs;
            fs.release();
            
            // 进行图像校正
            Mat undistorted;
            cv::undistort(gray, undistorted, camera_matrix, dist_coeffs);
            gray = undistorted.clone();
            
            LOGI("已应用相机标定参数进行图像校正");
        } else {
            LOGW("未找到标定参数文件: %s，使用原始图像", calib_file);
        }
    }

    // 仅在图像尺寸超过最大值时进行等比例缩放
    if (gray.cols > MAX_IMAGE_WIDTH || gray.rows > MAX_IMAGE_HEIGHT) {
        float scale = min(static_cast<float>(MAX_IMAGE_WIDTH) / gray.cols, 
                         static_cast<float>(MAX_IMAGE_HEIGHT) / gray.rows);
        int new_width = static_cast<int>(gray.cols * scale);
        int new_height = static_cast<int>(gray.rows * scale);
        
        resize(gray, gray, Size(new_width, new_height));
        
        LOGI("已压缩图片尺寸: %dx%d -> %dx%d", 
            gray.cols, gray.rows, new_width, new_height);
    }

    // 高斯模糊：去除图像噪声
    Mat blur;
    GaussianBlur(gray, blur, Size(5, 5), 0);
    snprintf(output_path, sizeof(output_path), "%s/%s_blur.jpg", output_dir.c_str(), base_filename);
    if (!imwrite(output_path, blur)) {
        LOGE("保存模糊图失败: %s", output_path);
        return -1;
    }
    gray.release();

    // 普通二值化：使用OTSU自动阈值
    Mat binary;
    threshold(blur, binary, 0, 255, THRESH_BINARY | THRESH_OTSU);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary.jpg", output_dir.c_str(), base_filename);
    if (!imwrite(output_path, binary)) {
        LOGE("保存二值图失败: %s", output_path);
        return -1;
    }
    blur.release();

    // 形态学去噪：使用开运算去除小的噪点
    Mat kernel = getStructuringElement(MORPH_RECT, Size(3, 3));
    morphologyEx(binary, binary, MORPH_OPEN, kernel);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary_denoised.jpg", output_dir.c_str(), base_filename);
    if (!imwrite(output_path, binary)) {
        LOGE("保存去噪二值图失败: %s", output_path);
        return -1;
    }

    // 在二值图中间取一条水平线，通过像素颜色变化检测段落
    int mid_y = binary.rows / 2;
    LOGI("使用中间线 y = %d 进行测量", mid_y);
    
    // 检测像素颜色变化并测量段落
    vector<paragraph_t> paragraphs;
    bool in_segment = false;
    paragraph_t current_paragraph = {0};
    float pixel_to_mm_ratio = 0.0f;
    
    for (int x = 0; x < binary.cols; x++) {
        uchar pixel = binary.at<uchar>(mid_y, x);
        
        // 像素值0表示背景，像素值255表示前景
        if (pixel == 0 && !in_segment) {
            // 开始段落
            in_segment = true;
            current_paragraph.start_x = x;
        } else if (pixel == 255 && in_segment) {
            // 结束段落
            in_segment = false;
            current_paragraph.end_x = x - 1;
            current_paragraph.width_px = current_paragraph.end_x - current_paragraph.start_x + 1;
            
            // 筛选宽度合适的段落
            if (current_paragraph.width_px >= MIN_PARAGRAPH_WIDTH) {
                paragraphs.push_back(current_paragraph);
            }
        }
    }
    
    // 处理最后一个段落（如果图像边缘是段落）
    if (in_segment) {
        current_paragraph.end_x = binary.cols - 1;
        current_paragraph.width_px = current_paragraph.end_x - current_paragraph.start_x + 1;
        
        if (current_paragraph.width_px >= MIN_PARAGRAPH_WIDTH) {
            paragraphs.push_back(current_paragraph);
        }
    }
    
    // 计算像素到毫米的转换比例，使用第一个段落作为标准
    if (!paragraphs.empty()) {
        pixel_to_mm_ratio = SCALE_WIDTH_MM / paragraphs[0].width_px;
        
        // 计算每个段落的毫米尺寸
        for (size_t i = 0; i < paragraphs.size(); i++) {
            paragraphs[i].width_mm = paragraphs[i].width_px * pixel_to_mm_ratio;
            LOGI("段落%d: %d-%d, 宽度: %dpx (%.2fmm)", 
                   i+1, paragraphs[i].start_x, paragraphs[i].end_x,
                   paragraphs[i].width_px, paragraphs[i].width_mm);
        }
    } else {
        LOGW("未检测到任何段落");
    }
    
    // 保存测量数据到文本文件
    snprintf(output_path, sizeof(output_path), "%s/%s_measurements.txt", output_dir.c_str(), base_filename);
    FILE *fp = fopen(output_path, "w");
    if (fp) {
        fprintf(fp, "图像文件名: %s\n", filename);
        fprintf(fp, "图像尺寸: %dx%d\n", binary.cols, binary.rows);
        fprintf(fp, "测量线位置: y = %d\n", mid_y);
        fprintf(fp, "像素到毫米比例: %.4f\n", pixel_to_mm_ratio);
        fprintf(fp, "检测到的段落数量: %zu\n", paragraphs.size());
        fprintf(fp, "\n详细测量结果:\n");
        fprintf(fp, "------------------------------------\n");
        fprintf(fp, "段落 # | 起始X | 结束X | 宽度(像素) | 宽度(毫米)\n");
        fprintf(fp, "------------------------------------\n");
        
        for (size_t i = 0; i < paragraphs.size(); i++) {
            fprintf(fp, "%5zu | %6d | %6d | %11d | %9.2f\n", 
                   i + 1, 
                   paragraphs[i].start_x, 
                   paragraphs[i].end_x, 
                   paragraphs[i].width_px, 
                   paragraphs[i].width_mm);
        }
        
        fclose(fp);
        LOGI("已将测量数据保存到: %s", output_path);
    } else {
        LOGE("无法创建测量数据文件: %s", output_path);
    }
    
    binary.release();
    LOGI("图像 %s 处理完成", input_path.c_str());
    return 0;
}

/**
 * @brief 处理指定文件夹中的所有图像
 * 
 * @param config 图像处理配置
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_folder(const image_processor_config_t& config) {
    DIR *dir;
    struct dirent *entry;
    int processed_count = 0;
    
    LOGI("开始处理文件夹: %s", config.input_dir.c_str());
    
    // 确保输出目录存在
    if (ensure_directory_exists(config.output_dir.c_str()) != 0) {
        return -1;
    }
    
    // 打开输入目录
    dir = opendir(config.input_dir.c_str());
    if (!dir) {
        LOGE("无法打开目录: %s", config.input_dir.c_str());
        return -1;
    }
    
    // 遍历目录中的文件
    while ((entry = readdir(dir)) != NULL) {
        // 跳过.和..目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 检查是否为图片文件
        if (is_image_file(entry->d_name)) {
            // 构造完整的文件路径
            char input_path[512] = {0};
            snprintf(input_path, sizeof(input_path), "%s/%s", config.input_dir.c_str(), entry->d_name);
            
            // 处理图片
            if (image_processor_process_image(input_path, config.output_dir, config.use_calibration) == 0) {
                processed_count++;
                LOGI("已处理图片: %s", entry->d_name);
            }
            
            // 检查是否达到最大处理数量
            if (config.max_images > 0 && processed_count >= config.max_images) {
                break;
            }
        }
    }
    
    closedir(dir);
    LOGI("文件夹处理完成，共处理了 %d 张图片", processed_count);
    return 0;
}

/**
 * @brief 从文件夹加载标定图像
 * 
 * @param calib_dir 标定图像文件夹
 * @return vector<string> 标定图像文件路径列表
 */
vector<string> image_processor_load_calibration_images(const string& calib_dir) {
    vector<string> calib_images;
    DIR *dir;
    struct dirent *entry;
    
    // 打开标定图像目录
    dir = opendir(calib_dir.c_str());
    if (!dir) {
        LOGE("无法打开标定图像目录: %s", calib_dir.c_str());
        return calib_images;
    }
    
    // 遍历目录中的文件
    while ((entry = readdir(dir)) != NULL) {
        // 跳过.和..目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 检查是否为图片文件
        if (is_image_file(entry->d_name)) {
            // 构造完整的文件路径
            char image_path[512] = {0};
            snprintf(image_path, sizeof(image_path), "%s/%s", calib_dir.c_str(), entry->d_name);
            calib_images.push_back(string(image_path));
        }
    }
    
    closedir(dir);
    LOGI("从目录 %s 加载了 %zu 张标定图像", calib_dir.c_str(), calib_images.size());
    return calib_images;
}
