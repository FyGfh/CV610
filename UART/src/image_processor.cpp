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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <cmath>
#include <algorithm>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>


using namespace std;
using namespace cv;

#define LOGI(...) fprintf(stdout, "[INFO] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGW(...) fprintf(stdout, "[WARN] " __VA_ARGS__); fprintf(stdout, "\n")
#define LOGE(...) fprintf(stderr, "[ERROR] " __VA_ARGS__); fprintf(stderr, "\n")

#define MAX_IMAGE_WIDTH 640
#define MAX_IMAGE_HEIGHT 480
#define SCALE_WIDTH_MM 8.0f
#define MIN_PARAGRAPH_WIDTH 5
#define DEFAULT_CALIB_FILE "/data/calib_params/camera_calibration.xml"

static vector<paragraph_t> g_paragraphs;
static float g_pixel_to_mm_ratio = 0.0f;

// 辅助函数：线性插值
static uint8_t bilinear_interpolate(const uint8_t *src, int width, int height, float x, float y) {
    // 边界保护
    if (x < 0) x = 0;
    if (x >= width - 1) x = width - 2;
    if (y < 0) y = 0;
    if (y >= height - 1) y = height - 2;
    
    int x0 = (int)x;
    int y0 = (int)y;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    
    float dx = x - x0;
    float dy = y - y0;
    
    // 四个邻域像素
    uint8_t p00 = src[y0 * width + x0];
    uint8_t p01 = src[y0 * width + x1];
    uint8_t p10 = src[y1 * width + x0];
    uint8_t p11 = src[y1 * width + x1];
    
    // 双线性插值计算
    float value = (1 - dx) * (1 - dy) * p00 +
                  dx * (1 - dy) * p01 +
                  (1 - dx) * dy * p10 +
                  dx * dy * p11;
    
    return (uint8_t)value;
}

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

// 自定义undistort函数：处理畸变图像（支持5参数畸变模型）
static void custom_undistort(const cv::Mat &src, cv::Mat &dst, const cv::Mat &camera_matrix, const cv::Mat &dist_coeffs) {
    if (src.empty() || camera_matrix.empty() || dist_coeffs.empty()) {
        LOGE("自定义undistort参数无效");
        return;
    }
    
    int width = src.cols;
    int height = src.rows;
    
    // 从相机矩阵中提取参数（cv::Mat转普通变量，提高计算效率）
    float fx = camera_matrix.at<double>(0, 0);
    float fy = camera_matrix.at<double>(1, 1);
    float cx = camera_matrix.at<double>(0, 2);
    float cy = camera_matrix.at<double>(1, 2);
    
    // 从畸变系数中提取参数（支持5参数：k1,k2,p1,p2,k3）
    float k1 = dist_coeffs.at<double>(0, 0);
    float k2 = dist_coeffs.at<double>(0, 1);
    float p1 = dist_coeffs.at<double>(0, 2);
    float p2 = dist_coeffs.at<double>(0, 3);
    float k3 = (dist_coeffs.cols > 4) ? dist_coeffs.at<double>(0, 4) : 0.0f;
    
    // 初始化输出图像（和输入图像格式一致：灰度图CV_8UC1）
    dst.create(height, width, CV_8UC1);
    if (dst.empty()) {
        LOGE("输出图像创建失败");
        return;
    }
    
    // 畸变校正核心：反向映射（遍历输出像素，找输入图像对应位置）
    const uint8_t *src_data = src.data;
    uint8_t *dst_data = dst.data;
    
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // 1. 像素坐标 → 归一化相机坐标（去内参）
            float x_norm = (x - cx) / fx;
            float y_norm = (y - cy) / fy;
            
            // 2. 计算畸变因子（径向+切向）
            float r2 = x_norm * x_norm + y_norm * y_norm;  // r? = x? + y?
            float r4 = r2 * r2;
            float r6 = r4 * r2;
            
            // 径向畸变：1 + k1r? + k2r? + k3r?
            float radial = 1.0f + k1 * r2 + k2 * r4 + k3 * r6;
            // 切向畸变
            float tang_x = 2 * p1 * x_norm * y_norm + p2 * (r2 + 2 * x_norm * x_norm);
            float tang_y = p1 * (r2 + 2 * y_norm * y_norm) + 2 * p2 * x_norm * y_norm;
            
            // 3. 畸变后的归一化坐标
            float x_norm_dist = x_norm * radial + tang_x;
            float y_norm_dist = y_norm * radial + tang_y;
            
            // 4. 归一化坐标 → 像素坐标（加内参）
            float x_dist = x_norm_dist * fx + cx;
            float y_dist = y_norm_dist * fy + cy;
            
            // 5. 双线性插值获取像素值（避免锯齿）
            dst_data[y * width + x] = bilinear_interpolate(src_data, width, height, x_dist, y_dist);
        }
    }
}

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

int image_processor_process_image(const char* input_path, const char* output_dir, int use_calibration) {
    const char *filename = strrchr(input_path, '/');
    char base_filename[128] = {0};
    char output_path[512] = {0};
    
    if (!filename) {
        filename = input_path;
    } else {
        filename++;
    }
    
    strcpy(base_filename, filename);
    char *ext = strrchr(base_filename, '.');
    if (ext) {
        *ext = '\0';
    }
    
    LOGI("开始处理图像: %s", input_path);
    
    if (ensure_directory_exists(output_dir) != 0) {
        return -1;
    }
    
    Mat gray = imread(input_path, IMREAD_GRAYSCALE);
    if (gray.empty()) {
        LOGE("无法读取图片: %s", input_path);
        return -1;
    }

    if (use_calibration) {
        const char *calib_file = DEFAULT_CALIB_FILE;
        FileStorage fs(calib_file, FileStorage::READ);
        if (fs.isOpened()) {
            Mat camera_matrix, dist_coeffs;
            fs["camera_matrix"] >> camera_matrix;
            fs["dist_coeffs"] >> dist_coeffs;
            fs.release();
            
            Mat undistorted;
            custom_undistort(gray, undistorted, camera_matrix, dist_coeffs);
            gray = undistorted.clone();
            
            LOGI("已应用相机标定参数进行图像校正");
        } else {
            LOGW("未找到标定参数文件: %s，使用原始图像", calib_file);
        }
    }

    if (gray.cols > MAX_IMAGE_WIDTH || gray.rows > MAX_IMAGE_HEIGHT) {
        float scale = min(static_cast<float>(MAX_IMAGE_WIDTH) / gray.cols, 
                         static_cast<float>(MAX_IMAGE_HEIGHT) / gray.rows);
        int new_width = static_cast<int>(gray.cols * scale);
        int new_height = static_cast<int>(gray.rows * scale);
        
        resize(gray, gray, Size(new_width, new_height));
        
        LOGI("已压缩图片尺寸: %dx%d -> %dx%d", 
            gray.cols, gray.rows, new_width, new_height);
    }

    Mat blur;
    GaussianBlur(gray, blur, Size(5, 5), 0);
    snprintf(output_path, sizeof(output_path), "%s/%s_blur.jpg", output_dir, base_filename);
    if (!imwrite(output_path, blur)) {
        LOGE("保存模糊图失败: %s", output_path);
        return -1;
    }
    gray.release();

    Mat binary;
    threshold(blur, binary, 0, 255, THRESH_BINARY | THRESH_OTSU);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary.jpg", output_dir, base_filename);
    if (!imwrite(output_path, binary)) {
        LOGE("保存二值图失败: %s", output_path);
        return -1;
    }
    blur.release();

    Mat kernel = getStructuringElement(MORPH_RECT, Size(3, 3));
    morphologyEx(binary, binary, MORPH_OPEN, kernel);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary_denoised.jpg", output_dir, base_filename);
    if (!imwrite(output_path, binary)) {
        LOGE("保存去噪二值图失败: %s", output_path);
        return -1;
    }

    int mid_y = binary.rows / 2;
    LOGI("使用中间线 y = %d 进行测量", mid_y);
    
    g_paragraphs.clear();
    bool in_segment = false;
    paragraph_t current_paragraph = {0, 0, 0, 0.0f};
    g_pixel_to_mm_ratio = 0.0f;
    
    for (int x = 0; x < binary.cols; x++) {
        uchar pixel = binary.at<uchar>(mid_y, x);
        
        if (pixel == 0 && !in_segment) {
            in_segment = true;
            current_paragraph.start_x = x;
        } else if (pixel == 255 && in_segment) {
            in_segment = false;
            current_paragraph.end_x = x - 1;
            current_paragraph.width_px = current_paragraph.end_x - current_paragraph.start_x + 1;
            
            if (current_paragraph.width_px >= MIN_PARAGRAPH_WIDTH) {
                g_paragraphs.push_back(current_paragraph);
            }
        }
    }
    
    if (in_segment) {
        current_paragraph.end_x = binary.cols - 1;
        current_paragraph.width_px = current_paragraph.end_x - current_paragraph.start_x + 1;
        
        if (current_paragraph.width_px >= MIN_PARAGRAPH_WIDTH) {
            g_paragraphs.push_back(current_paragraph);
        }
    }
    
    if (!g_paragraphs.empty()) {
        g_pixel_to_mm_ratio = SCALE_WIDTH_MM / g_paragraphs[0].width_px;
        
        for (size_t i = 0; i < g_paragraphs.size(); i++) {
            g_paragraphs[i].width_mm = g_paragraphs[i].width_px * g_pixel_to_mm_ratio;
            LOGI("段落%zu: %d-%d, 宽度: %dpx (%.2fmm)", 
                   i+1, g_paragraphs[i].start_x, g_paragraphs[i].end_x,
                   g_paragraphs[i].width_px, g_paragraphs[i].width_mm);
        }
    } else {
        LOGW("未检测到任何段落");
    }
    
    snprintf(output_path, sizeof(output_path), "%s/%s_measurements.txt", output_dir, base_filename);
    FILE *fp = fopen(output_path, "w");
    if (fp) {
        fprintf(fp, "图像文件名: %s\n", filename);
        fprintf(fp, "图像尺寸: %dx%d\n", binary.cols, binary.rows);
        fprintf(fp, "测量线位置: y = %d\n", mid_y);
        fprintf(fp, "像素到毫米比例: %.4f\n", g_pixel_to_mm_ratio);
        fprintf(fp, "检测到的段落数量: %zu\n", g_paragraphs.size());
        fprintf(fp, "\n详细测量结果:\n");
        fprintf(fp, "------------------------------------\n");
        fprintf(fp, "段落 # | 起始X | 结束X | 宽度(像素) | 宽度(毫米)\n");
        fprintf(fp, "------------------------------------\n");
        
        for (size_t i = 0; i < g_paragraphs.size(); i++) {
            fprintf(fp, "%5zu | %6d | %6d | %11d | %9.2f\n", 
                   i + 1, 
                   g_paragraphs[i].start_x, 
                   g_paragraphs[i].end_x, 
                   g_paragraphs[i].width_px, 
                   g_paragraphs[i].width_mm);
        }
        
        fclose(fp);
        LOGI("已将测量数据保存到: %s", output_path);
    } else {
        LOGE("无法创建测量数据文件: %s", output_path);
    }
    
    binary.release();
    LOGI("图像 %s 处理完成", input_path);
    return 0;
}

int image_processor_process_folder(const image_processor_config_t* config) {
    DIR *dir;
    struct dirent *entry;
    int processed_count = 0;
    
    LOGI("开始处理文件夹: %s", config->input_dir);
    
    if (ensure_directory_exists(config->output_dir) != 0) {
        return -1;
    }
    
    dir = opendir(config->input_dir);
    if (!dir) {
        LOGE("无法打开目录: %s", config->input_dir);
        return -1;
    }
    
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        if (is_image_file(entry->d_name)) {
            char input_path[1024] = {0};
            snprintf(input_path, sizeof(input_path), "%s/%s", config->input_dir, entry->d_name);
            
            if (image_processor_process_image(input_path, config->output_dir, config->use_calibration) == 0) {
                processed_count++;
                LOGI("已处理图片: %s", entry->d_name);
            }
            
            if (config->max_images > 0 && processed_count >= config->max_images) {
                break;
            }
        }
    }
    
    closedir(dir);
    LOGI("文件夹处理完成，共处理了 %d 张图片", processed_count);
    return 0;
}

int image_processor_get_paragraph_count(void) {
    return (int)g_paragraphs.size();
}

int image_processor_get_paragraph(int index, paragraph_t* paragraph) {
    if (index < 0 || index >= (int)g_paragraphs.size() || !paragraph) {
        return -1;
    }
    *paragraph = g_paragraphs[index];
    return 0;
}
