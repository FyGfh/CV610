#include "image_processor.h"
#include "esp_log.h"
#include "esp_err.h"
#include "dirent.h"
#include "sys/stat.h"
#include "stdlib.h"
#include "string.h"
#include "esp_system.h"
#include <vector>
#include <math.h>
// 添加看门狗相关头文件
#include "esp_task_wdt.h"
#include "freertos/task.h"

#include "spiconnect.h" // SPI传输函数

// OpenCV库头文件
//#include <opencv2/opencv.hpp>
#include <opencv2/core.hpp> // 核心功能
#include <opencv2/imgproc.hpp> // 图像处理
#include <opencv2/imgcodecs.hpp> // 图像编码
#include <opencv2/calib3d.hpp> // 相机标定

static const char *TAG = "image_processor";
#define MAX_IMAGE_WIDTH 640
#define MAX_IMAGE_HEIGHT 480
#define SCALE_WIDTH_MM 8.0f  // 最左侧矩形的实际宽度（mm）
#define MIN_PARAGRAPH_WIDTH 5 // 最小段落宽度阈值（像素）

// 通过SPI传输图像文件
esp_err_t image_processor_transfer_image_by_spi(const char *image_path, int spi_host_id)
{
    ESP_LOGI(TAG, "Preparing to transfer image via SPI: %s", image_path);
    // 初始化SPI
    esp_err_t ret = spi_connect_init(spi_host_id);
    // 直接调用SPI传输函数
    return spi_transfer_file(image_path, spi_host_id);
}


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

// 自定义undistort函数：处理畸变图像（支持5参数畸变模型）
static void custom_undistort(const cv::Mat &src, cv::Mat &dst, const cv::Mat &camera_matrix, const cv::Mat &dist_coeffs) {
    if (src.empty() || camera_matrix.empty() || dist_coeffs.empty()) {
        ESP_LOGE(TAG, "自定义undistort参数无效");
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
        ESP_LOGE(TAG, "输出图像创建失败");
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

// 段落宽度结构体定义
typedef struct {
    int start_x;   // 段落起始x坐标
    int end_x;     // 段落结束x坐标
    int width_px;  // 段落宽度（像素）
    float width_mm; // 段落宽度（毫米）
} paragraph_t;

// 检查文件是否为图片文件
static bool is_image_file(const char *filename)
{
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

// 创建目录（如果不存在）
static esp_err_t ensure_directory_exists(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        } else {
            ESP_LOGE(TAG, "路径已存在但不是目录: %s", path);
            return ESP_FAIL;
        }
    }
    
    if (mkdir(path, 0777) != 0) {
        ESP_LOGE(TAG, "创建目录失败: %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 处理指定文件夹中的所有图片
esp_err_t image_processor_process_folder(const image_processor_config_t *config)
{
    DIR *dir;
    struct dirent *entry;
    int processed_count = 0;
    
    ESP_LOGI(TAG, "开始处理文件夹: %s", config->input_dir);
    
    // 确保输出目录存在
    if (ensure_directory_exists(config->output_dir) != ESP_OK) {
        return ESP_FAIL;
    }
    
    // 打开输入目录
    dir = opendir(config->input_dir);
    if (!dir) {
        ESP_LOGE(TAG, "无法打开目录: %s", config->input_dir);
        return ESP_FAIL;
    }
    
    // 遍历目录中的文件
    while ((entry = readdir(dir)) != NULL) {
        // 重置看门狗定时器
        esp_task_wdt_reset();
        // 跳过.和..目录
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // 检查是否为图片文件
        if (is_image_file(entry->d_name)) {
            // 构造完整的文件路径
            char input_path[512];
            snprintf(input_path, sizeof(input_path), "%s/%s", config->input_dir, entry->d_name);
            
            // 处理图片
        if (image_processor_process_image(input_path, config->output_dir, config->use_calibration) == ESP_OK) {
            processed_count++;
            ESP_LOGI(TAG, "已处理图片: %s", entry->d_name);
        }
            
            // 检查是否达到最大处理数量
            if (config->max_images > 0 && processed_count >= config->max_images) {
                break;
            }
            // 每处理一张图片后短暂延迟，给其他任务执行机会
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    
    closedir(dir);
    ESP_LOGI(TAG, "文件夹处理完成，共处理了 %d 张图片", processed_count);
    return ESP_OK;
}

esp_err_t image_processor_process_image(const char *input_path, const char *output_dir, bool use_calibration)
{
    // 提取文件名（不含扩展名）
    const char *filename = strrchr(input_path, '/');
    char base_filename[128];
    char output_path[512];
    
    if (!filename) {
        filename = input_path;
    } else {
        filename++;
    }
    
    // 复制文件名（去掉扩展名）
    strcpy(base_filename, filename);
    char *ext = strrchr(base_filename, '.');
    if (ext) {
        *ext = '\0';
    }
    
    // 读取图像时直接压缩尺寸并转换为灰度图
    cv::Mat gray = cv::imread(input_path, cv::IMREAD_GRAYSCALE);
    if (gray.empty()) {
        ESP_LOGE(TAG, "无法读取图片: %s", input_path);
        return ESP_FAIL;
    }

    // 如果启用标定功能且标定文件存在，则进行畸变校正
    if (use_calibration) {
        // 加载标定参数
        const char *calib_file = "/data/calib_params/camera_calibration.xml";
        cv::FileStorage fs(calib_file, cv::FileStorage::READ);
        if (fs.isOpened()) {
            cv::Mat camera_matrix, dist_coeffs;
            fs["camera_matrix"] >> camera_matrix;
            fs["dist_coeffs"] >> dist_coeffs;
            fs.release();
            
            // 进行图像校正
            cv::Mat undistorted;
            //cv::undistort(gray, undistorted, camera_matrix, dist_coeffs);
            // 使用自定义的畸变校正函数
            custom_undistort(gray, undistorted, camera_matrix, dist_coeffs);
            gray = undistorted.clone();
            
            ESP_LOGI(TAG, "已应用相机标定参数进行图像校正");
        } else {
            ESP_LOGW(TAG, "未找到标定参数文件: %s，使用原始图像", calib_file);
        }
    }
    // if (use_calibration) {
    //     // 预计算的相机内参和畸变系数
    //     // 假设相机内参和畸变系数
    //     double fx = 1000.0, fy = 1000.0, cx = 320.0, cy = 240.0;
    //     double k1 = 0.1, k2 = 0.01, p1 = 0.001, p2 = 0.001, k3 = 0.01;
    //     cv::Mat camera_matrix = (cv::Mat_<double>(3, 3) << fx, 0, cx, 0, fy, cy, 0, 0, 1);
    //     cv::Mat dist_coeffs = (cv::Mat_<double>(1, 5) << k1, k2, p1, p2, k3);
        
    //     // 进行图像校正
    //     cv::Mat undistorted;
    //     // cv::undistort(gray, undistorted, camera_matrix, dist_coeffs);
    //     // gray = undistorted.clone();
        
    //     ESP_LOGI(TAG, "已应用预计算的相机标定参数");
    // }


    // 仅在图像尺寸超过最大值时进行等比例缩放
    if (gray.cols > MAX_IMAGE_WIDTH || gray.rows > MAX_IMAGE_HEIGHT) {
        float scale = std::min((float)MAX_IMAGE_WIDTH / gray.cols, (float)MAX_IMAGE_HEIGHT / gray.rows);
        int new_width = static_cast<int>(gray.cols * scale);
        int new_height = static_cast<int>(gray.rows * scale);
        
        cv::resize(gray, gray, cv::Size(new_width, new_height));
        
        ESP_LOGI(TAG, "已压缩图片尺寸: %dx%d -> %dx%d", 
                gray.cols, gray.rows, new_width, new_height);
    }

    // 重置看门狗
    esp_task_wdt_reset();

    // 高斯模糊
    cv::Mat blur;
    cv::GaussianBlur(gray, blur, cv::Size(5, 5), 0);
    snprintf(output_path, sizeof(output_path), "%s/%s_blur.jpg", output_dir, base_filename);
    if (!cv::imwrite(output_path, blur)) {
        ESP_LOGE(TAG, "保存模糊图失败: %s", output_path);
        return ESP_FAIL;
    }
    gray.release();

    // 重置看门狗
    esp_task_wdt_reset();


    // 普通二值化
    cv::Mat binary;
    cv::threshold(blur, binary, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary.jpg", output_dir, base_filename);
    if (!cv::imwrite(output_path, binary)) {
        ESP_LOGE(TAG, "保存二值图失败: %s", output_path);
        return ESP_FAIL;
    }
    // 重置看门狗
    // esp_task_wdt_reset();
    blur.release();

    // 自适应二值化
    // cv::Mat binary;
    // cv::adaptiveThreshold(blur, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C, cv::THRESH_BINARY, 21, 5);
    // snprintf(output_path, sizeof(output_path), "%s/%s_binary.jpg", output_dir, base_filename);
    // if (!cv::imwrite(output_path, binary)) {
    //     ESP_LOGE(TAG, "保存二值图失败: %s", output_path);
    //     return ESP_FAIL;
    // }
    // blur.release();
    // // 重置看门狗
    esp_task_wdt_reset();


    // 形态学去噪
    cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);
    snprintf(output_path, sizeof(output_path), "%s/%s_binary_denoised.jpg", output_dir, base_filename);
    if (!cv::imwrite(output_path, binary)) {
        ESP_LOGE(TAG, "保存去噪二值图失败: %s", output_path);
        return ESP_FAIL;
    }

    // 重置看门狗
    esp_task_wdt_reset();

    // 在二值图中间取一条水平线，通过像素颜色变化检测段落
    int mid_y = binary.rows / 2;
    ESP_LOGI(TAG, "使用中间线 y = %d 进行测量", mid_y);
    
    // 检测像素颜色变化并测量段落
    std::vector<paragraph_t> paragraphs;
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
            ESP_LOGI(TAG, "段落%d: %d-%d, 宽度: %dpx (%.2fmm)", 
                   i+1, paragraphs[i].start_x, paragraphs[i].end_x,
                   paragraphs[i].width_px, paragraphs[i].width_mm);
        }
    } else {
        ESP_LOGW(TAG, "未检测到任何段落");
    }
    
    // 保存测量数据到文本文件
    snprintf(output_path, sizeof(output_path), "%s/%s_measurements.txt", output_dir, base_filename);
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
        ESP_LOGI(TAG, "已将测量数据保存到: %s", output_path);
    } else {
        ESP_LOGE(TAG, "无法创建测量数据文件: %s", output_path);
    }
    
    binary.release();
    return ESP_OK;
}
