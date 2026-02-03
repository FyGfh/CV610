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

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 段落宽度结构体定义
 * 
 * 用于存储检测到的段落的位置和尺寸信息
 */
typedef struct {
    int start_x;
    int end_x;
    int width_px;
    float width_mm;
} paragraph_t;

/**
 * @brief 图像处理配置结构体
 * 
 * 用于配置图像处理算法的参数
 */
typedef struct {
    char input_dir[512];
    char output_dir[512];
    uint8_t use_calibration;
    int max_images;
} image_processor_config_t;

/**
 * @brief 处理单张图像
 * 
 * @param input_path 输入图像文件路径
 * @param output_dir 输出结果目录
 * @param use_calibration 是否使用标定参数
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_image(const char* input_path, 
                                  const char* output_dir, 
                                  int use_calibration);

/**
 * @brief 处理指定文件夹中的所有图像
 * 
 * @param config 图像处理配置
 * @return int 处理结果，0表示成功，非0表示失败
 */
int image_processor_process_folder(const image_processor_config_t* config);

/**
 * @brief 获取检测到的段落数量
 * 
 * @return int 段落数量
 */
int image_processor_get_paragraph_count(void);

/**
 * @brief 获取指定段落的信息
 * 
 * @param index 段落索引
 * @param paragraph 输出段落信息
 * @return int 0表示成功，非0表示失败
 */
int image_processor_get_paragraph(int index, paragraph_t* paragraph);

#ifdef __cplusplus
}
#endif

#endif // IMAGE_PROCESSOR_H
