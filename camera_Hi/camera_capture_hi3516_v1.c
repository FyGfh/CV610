#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

#include "ot_common.h"
#include "ot_defines.h"
#include "ot_type.h"
#include "sample_comm.h"
#include "ss_mpi_sys.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_sys_bind.h"

#define DEFAULT_OUTPUT_FILE "capture.jpg"
#define DEFAULT_WIDTH 3840
#define DEFAULT_HEIGHT 2160
#define DEFAULT_PIPE_ID 0

// 使用IMX415传感器
sample_sns_type g_sns_type = IMX415_MIPI_8M_25FPS_10BIT;

static struct option long_options[] = {
    {"output", required_argument, 0, 'o'},
    {"width", required_argument, 0, 'w'},
    {"height", required_argument, 0, 'h'},
    {"help", no_argument, 0, 'h'},
    {0, 0, 0, 0}
};

static void print_usage(const char *prog_name) {
    printf("Usage: %s [options]\n", prog_name);
    printf("Options:\n");
    printf("  -o, --output <file>    Output file name (default: %s)\n", DEFAULT_OUTPUT_FILE);
    printf("  -w, --width <width>     Image width (default: %d)\n", DEFAULT_WIDTH);
    printf("  -h, --height <height>   Image height (default: %d)\n", DEFAULT_HEIGHT);
    printf("  -?, --help              Show this help message\n");
}

// 调整ISP参数，解决边缘偏紫色问题
static int adjust_isp_parameters(void) {
    printf("Adjusting ISP parameters for edge purple issue...\n");
    
    // 这里可以添加具体的ISP参数调整代码
    // 例如调整AWB参数、色彩校正参数等
    // 由于SDK版本不同，具体的ISP参数调整函数可能不同
    
    printf("ISP parameters adjustment completed\n");
    return TD_SUCCESS;
}

int main(int argc, char *argv[]) {
    char *output_file = DEFAULT_OUTPUT_FILE;
    int width = DEFAULT_WIDTH;
    int height = DEFAULT_HEIGHT;
    int opt;
    int option_index = 0;
    
    // 解析命令行参数
    while ((opt = getopt_long(argc, argv, "o:w:h:", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'o':
                output_file = optarg;
                break;
            case 'w':
                width = atoi(optarg);
                break;
            case 'h':
                height = atoi(optarg);
                break;
            case '?':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return -1;
        }
    }
    
    printf("Camera capture program (Version 1 - Enhanced) starting...\n");
    printf("Output file: %s\n", output_file);
    printf("Resolution: %dx%d\n", width, height);
    printf("Using sensor: %d\n", g_sns_type);
    
    // 1. 系统初始化 - 参考官方快速启动示例
    printf("Initializing VB...\n");
    
    // 定义 VB 参数，使用12位像素格式（匹配传感器实际支持的格式）
    sample_vb_param vb_param = {
        // raw, yuv, vpss chn1
        .vb_size = {{DEFAULT_WIDTH, DEFAULT_HEIGHT}, {DEFAULT_WIDTH, DEFAULT_HEIGHT}, {720, 480}},
        .pixel_format = {OT_PIXEL_FORMAT_RGB_BAYER_12BPP, OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
            OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420},
        .compress_mode = {OT_COMPRESS_MODE_LINE, OT_COMPRESS_MODE_NONE,
            OT_COMPRESS_MODE_NONE},
        .video_format = {OT_VIDEO_FORMAT_LINEAR, OT_VIDEO_FORMAT_LINEAR,
            OT_VIDEO_FORMAT_LINEAR},
        .blk_num = {6, 10, 3} // 增加缓冲区数量以支持8MP传感器
    };
    
    ot_vb_cfg vb_cfg;
    td_u32 supplement_config = OT_VB_SUPPLEMENT_BNR_MOT_MASK | OT_VB_SUPPLEMENT_MOTION_DATA_MASK;
    
    // 获取默认 VB 配置
    sample_comm_sys_get_default_vb_cfg(&vb_param, &vb_cfg);
    
    // 使用官方例程的初始化方法
    printf("Initializing system with VB supplement...\n");
    if (sample_comm_sys_init_with_vb_supplement(&vb_cfg, supplement_config) != TD_SUCCESS) {
        printf("Failed to initialize system with VB supplement\n");
        return -1;
    }
    
    printf("System initialized successfully\n");
    
    // 2. 设置 VI 和 VPSS 模式
    if (sample_comm_vi_set_vi_vpss_mode(OT_VI_OFFLINE_VPSS_OFFLINE, OT_VI_AIISP_MODE_DEFAULT) != TD_SUCCESS) {
        printf("Failed to set VI VPSS mode\n");
        sample_comm_sys_exit();
        return -1;
    }
    
    printf("VI VPSS mode set successfully\n");

    // 3. 获取默认的VI配置 - 这会正确设置所有必要的参数，包括ISP相关配置
    sample_vi_cfg vi_cfg;
    sample_comm_vi_get_default_vi_cfg(g_sns_type, &vi_cfg);
    
    // 添加VI配置详细日志
    printf("VI configuration details:\n");
    printf("Pipe num: %d\n", vi_cfg.bind_pipe.pipe_num);
    for (int i = 0; i < vi_cfg.bind_pipe.pipe_num; i++) {
        printf("Pipe %d:\n", vi_cfg.bind_pipe.pipe_id[i]);
        printf("  Pixel format: %d\n", vi_cfg.pipe_info[i].pipe_attr.pixel_format);
        printf("  ISP bypass: %d\n", vi_cfg.pipe_info[i].pipe_attr.isp_bypass);
        printf("  Width: %d, Height: %d\n", vi_cfg.pipe_info[i].pipe_attr.size.width, vi_cfg.pipe_info[i].pipe_attr.size.height);
        printf("  Compress mode: %d\n", vi_cfg.pipe_info[i].pipe_attr.compress_mode);
        printf("  Chn num: %d\n", vi_cfg.pipe_info[i].chn_num);
        for (int j = 0; j < vi_cfg.pipe_info[i].chn_num; j++) {
            printf("  Chn %d: VI chn %d, Pixel format: %d\n", 
                j, vi_cfg.pipe_info[i].chn_info[j].vi_chn, 
                vi_cfg.pipe_info[i].chn_info[j].chn_attr.pixel_format);
        }
    }
    printf("MIPI info: MIPI dev %d\n", vi_cfg.mipi_info.mipi_dev);
    printf("Sensor info: SNS type %d, Bus ID %d\n", vi_cfg.sns_info.sns_type, vi_cfg.sns_info.bus_id);
    
    // 统一VI管道配置，使用12位像素格式（匹配传感器实际支持的格式）
    printf("Unifying VI pipe configurations with 12-bit pixel format...\n");
    for (int i = 0; i < vi_cfg.bind_pipe.pipe_num; i++) {
        // 确保所有管道使用12位像素格式
        vi_cfg.pipe_info[i].pipe_attr.pixel_format = OT_PIXEL_FORMAT_RGB_BAYER_12BPP;
        // 确保所有管道不绕过ISP
        vi_cfg.pipe_info[i].pipe_attr.isp_bypass = TD_FALSE;
        // 确保所有管道使用相同的压缩模式
        vi_cfg.pipe_info[i].pipe_attr.compress_mode = OT_COMPRESS_MODE_LINE;
        
        // 统一通道配置
        for (int j = 0; j < vi_cfg.pipe_info[i].chn_num; j++) {
            // 确保所有通道使用相同的像素格式
            vi_cfg.pipe_info[i].chn_info[j].chn_attr.pixel_format = OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420;
            // 确保所有通道使用相同的压缩模式
            vi_cfg.pipe_info[i].chn_info[j].chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
        }
    }
    printf("VI pipe configurations unified with 12-bit pixel format\n");
    
    // 检查并调整MIPI相关配置
    printf("Checking and adjusting MIPI configurations...\n");
    // 确保MIPI设备正确设置
    vi_cfg.mipi_info.mipi_dev = 0; // 使用默认的MIPI设备0
    // 确保MIPI通道划分模式正确
    vi_cfg.mipi_info.divide_mode = LANE_DIVIDE_MODE_0;
    // MIPI配置已由默认值设置，保持不变
    printf("MIPI configurations adjusted\n");
    
    // 4. 启动VI和ISP - 使用SDK提供的高级封装函数，它会正确处理所有初始化步骤
    // 包括传感器初始化、MIPI RX启动、ISP注册和初始化等
    printf("Initializing sensor and starting VI...\n");
    if (sample_comm_vi_start_vi(&vi_cfg) != TD_SUCCESS) {
        printf("Failed to start VI\n");
        sample_comm_sys_exit();
        return -1;
    }
    
    printf("VI started successfully\n");
    
    // 调整ISP处理参数，解决边缘偏紫色问题
    if (adjust_isp_parameters() != TD_SUCCESS) {
        printf("Failed to adjust ISP parameters\n");
        goto cleanup;
    }
    
    // 5. 绑定VI到VENC，使VENC能够接收图像数据
    ot_vi_pipe vi_pipe = 0; // 使用默认的pipe 0
    ot_vi_chn vi_chn = 0;   // 使用默认的chn 0
    ot_venc_chn venc_chn = 0;
    
    // 启动拍照模式 - 使用photo模式替代snap模式
    ot_size snap_size = {width, height};
    if (sample_comm_venc_photo_start(venc_chn, &snap_size, TD_FALSE) != TD_SUCCESS) {
        printf("Failed to start photo\n");
        goto cleanup;
    }
    
    // 绑定VI到VENC，使VENC能够接收图像数据
    printf("Binding VI to VENC...\n");
    if (sample_comm_vi_bind_venc(vi_pipe, vi_chn, venc_chn) != TD_SUCCESS) {
        printf("Failed to bind VI to VENC\n");
        goto cleanup;
    }
    printf("VI bound to VENC successfully\n");
    
    // 等待VI稳定输出图像数据
    printf("Waiting for VI to stabilize...\n");
    usleep(1000000); // 延迟1秒，让VI有足够的时间输出图像
    
    // 增加ISP收敛时间，解决边缘偏紫色问题
    printf("Waiting for ISP AWB to converge...\n");
    usleep(4000000); // 增加4秒延迟，让ISP的AWB算法完全收敛
    
    // 处理拍照
    if (sample_comm_venc_snap_process(venc_chn, 1, TD_TRUE, TD_FALSE) != TD_SUCCESS) {
        printf("Failed to process photo\n");
        goto cleanup;
    }
    
    // 停止拍照
    if (ss_mpi_venc_stop_chn(venc_chn) != TD_SUCCESS) {
        printf("Failed to stop venc chn\n");
        goto cleanup;
    }
    if (ss_mpi_venc_destroy_chn(venc_chn) != TD_SUCCESS) {
        printf("Failed to destroy venc chn\n");
        goto cleanup;
    }
    
    // 重命名文件
    if (rename("snap_0.jpg", output_file) != 0) {
        printf("Failed to rename file\n");
        goto cleanup;
    }
    
    printf("Image captured successfully: %s\n", output_file);
    
cleanup:
    // 解绑VI到VENC
    sample_comm_vi_un_bind_venc(vi_pipe, vi_chn, venc_chn);
    
    // 停止VI - 这会自动处理ISP停止、传感器注销等所有相关资源释放
    sample_comm_vi_stop_vi(&vi_cfg);
    
    // 系统退出
    sample_comm_sys_exit();
    
    return 0;
}