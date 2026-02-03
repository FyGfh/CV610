/**
 * @file image_processor.h
 * @brief 海思CV610平台OpenCV图像处理算法头文件
 * @author TraeAI
 * @date 2026-01-22
 * 
 * 本头文件定义了图像处理算法的接口和数据结构，
 * 适用于海思CV610 Linux芯片平台。
 */

#ifndef IMAGE_PROCESSOR_H
#define IMAGE_PROCESSOR_H

#include <string>
#include <vector>

/**
 * @brief 段落宽度结构体定义
 * 
 * 用于存储检测到的段落的位置和尺寸信息
 */
typedef struct {
    int start_x;   ///< 段落起始x坐标
    int end_x;     ///< 段落结束x坐标
    int width_px;  ///< 段落宽度（像素）
    float width_mm; ///< 段落宽度（毫米）
} paragraph_t;

/**
 * @brief 图像处理配置结构体
 * 
 * 用于配置图像处理算法的参数
 */
typedef struct {
    std::string input_dir;     ///< 输入图像目录
    std::string output_dir;    ///< 输出结果目录
    bool use_calibration;      ///< 是否使用标定参数
    int max_images;            ///< 最大处理图像数量（0表示无限制）
} image_processor_config_t;

/**
 * @brief 处理单张图像
 * 
 * @param input_path 输入图像文件路径
 * @param output_dir 输出结果目录
 * @param use_calibration 是否使用标定参数
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_image(const std::string& input_path, 
                                 const std::string& output_dir, 
                                 bool use_calibration = false);

/**
 * @brief 处理指定文件夹中的所有图像
 * 
 * @param config 图像处理配置
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_folder(const image_processor_config_t& config);

/**
 * @brief 相机标定函数
 * 
 * @param calib_images 标定图像文件路径列表
 * @param output_file 输出标定文件路径
 * @return int 标定结果，0表示成功，非0表示失败
 */
int image_processor_calibrate_camera(const std::vector<std::string>& calib_images, 
                                     const std::string& output_file);

/**
 * @brief 从文件夹加载标定图像
 * 
 * @param calib_dir 标定图像文件夹
 * @return std::vector<std::string> 标定图像文件路径列表
 */
std::vector<std::string> image_processor_load_calibration_images(const std::string& calib_dir);

#endif // IMAGE_PROCESSOR_H
