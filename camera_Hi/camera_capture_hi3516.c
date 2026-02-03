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
    
    printf("Camera capture program starting...\n");
    printf("Output file: %s\n", output_file);
    printf("Resolution: %dx%d\n", width, height);
    printf("Using sensor: %d\n", g_sns_type);
    
    // 1. 传感器初始化
    sample_sns_info sns_info;
    sample_mipi_info mipi_info;
    sample_comm_vi_get_default_sns_info(g_sns_type, &sns_info);
    sample_comm_vi_get_default_mipi_info(g_sns_type, &mipi_info);
    
    printf("Initializing sensor...\n");
    if (sample_comm_vi_start_sensor(&sns_info, &mipi_info) != TD_SUCCESS) {
        printf("Failed to start sensor\n");
        return -1;
    }
    
    // 2. 系统初始化
    sample_comm_cfg comm_cfg;
    sample_comm_sys_get_default_cfg(1, &comm_cfg);
    
    // 设置传感器相关配置
    comm_cfg.mode_type = OT_VI_ONLINE_VPSS_OFFLINE;
    comm_cfg.aiisp_mode = OT_VI_AIISP_MODE_DEFAULT;
    comm_cfg.nr_pos = OT_3DNR_POS_VI;
    comm_cfg.supplement_cfg = OT_VB_SUPPLEMENT_BNR_MOT_MASK;
    comm_cfg.vi_pipe = 0;
    comm_cfg.vi_chn = 0;
    
    // 设置输入大小
    comm_cfg.in_size.width = DEFAULT_WIDTH;
    comm_cfg.in_size.height = DEFAULT_HEIGHT;
    
    // 初始化 VB 配置 - 采用官方例程的方式
    printf("Initializing VB...\n");
    
    // 定义 VB 参数，参考官方例程
    sample_vb_param vb_param = {
        // raw, yuv, vpss chn1
        .vb_size = {{DEFAULT_WIDTH, DEFAULT_HEIGHT}, {DEFAULT_WIDTH, DEFAULT_HEIGHT}, {720, 480}},
        .pixel_format = {OT_PIXEL_FORMAT_RGB_BAYER_12BPP, OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420,
            OT_PIXEL_FORMAT_YVU_SEMIPLANAR_420},
        .compress_mode = {OT_COMPRESS_MODE_LINE, OT_COMPRESS_MODE_NONE,
            OT_COMPRESS_MODE_NONE},
        .video_format = {OT_VIDEO_FORMAT_LINEAR, OT_VIDEO_FORMAT_LINEAR,
            OT_VIDEO_FORMAT_LINEAR},
        .blk_num = {4, 7, 3}
    };
    
    ot_vb_cfg vb_cfg;
    td_u32 supplement_config = OT_VB_SUPPLEMENT_BNR_MOT_MASK | OT_VB_SUPPLEMENT_MOTION_DATA_MASK;
    
    // 获取默认 VB 配置
    sample_comm_sys_get_default_vb_cfg(&vb_param, &vb_cfg);
    
    // 使用官方例程的初始化方法
    printf("Initializing system with VB supplement...\n");
    if (sample_comm_sys_init_with_vb_supplement(&vb_cfg, supplement_config) != TD_SUCCESS) {
        printf("Failed to initialize system with VB supplement\n");
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    printf("System initialized successfully\n");
    
    // 设置 VI 和 VPSS 模式
    if (sample_comm_vi_set_vi_vpss_mode(comm_cfg.mode_type, comm_cfg.aiisp_mode) != TD_SUCCESS) {
        printf("Failed to set VI VPSS mode\n");
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    printf("VI VPSS mode set successfully\n");

    // 3. 注册传感器到ISP
    printf("Registering sensor to ISP...\n");
    if (sample_comm_isp_sensor_regiter_callback(DEFAULT_PIPE_ID, g_sns_type) != TD_SUCCESS) {
        printf("Failed to register sensor to ISP\n");
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 注册3A库
    printf("Registering 3A libraries...\n");
    if (sample_comm_isp_ae_lib_callback(DEFAULT_PIPE_ID) != TD_SUCCESS) {
        printf("Failed to register AE library\n");
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    if (sample_comm_isp_awb_lib_callback(DEFAULT_PIPE_ID) != TD_SUCCESS) {
        printf("Failed to register AWB library\n");
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 4. 绑定传感器到ISP
    printf("Binding sensor to ISP...\n");
    if (sample_comm_isp_bind_sns(DEFAULT_PIPE_ID, g_sns_type, sns_info.bus_id) != TD_SUCCESS) {
        printf("Failed to bind sensor to ISP\n");
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }

    // 5. 初始化和启动 ISP
    // 获取ISP公共属性
    ot_isp_pub_attr isp_pub_attr;
    sample_comm_isp_get_pub_attr_by_sns(g_sns_type, &isp_pub_attr);
    
    // 初始化ISP内存
    printf("Initializing ISP memory...\n");
    if (ss_mpi_isp_mem_init(DEFAULT_PIPE_ID) != TD_SUCCESS) {
        printf("Failed to initialize ISP memory\n");
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 设置ISP公共属性
    printf("Setting ISP public attributes...\n");
    if (ss_mpi_isp_set_pub_attr(DEFAULT_PIPE_ID, &isp_pub_attr) != TD_SUCCESS) {
        printf("Failed to set ISP public attributes\n");
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 初始化ISP
    printf("Initializing ISP...\n");
    if (ss_mpi_isp_init(DEFAULT_PIPE_ID) != TD_SUCCESS) {
        printf("Failed to initialize ISP\n");
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 启动ISP
    printf("Starting ISP...\n");
    if (sample_comm_isp_run(DEFAULT_PIPE_ID) != TD_SUCCESS) {
        printf("Failed to start ISP\n");
        ss_mpi_isp_exit(DEFAULT_PIPE_ID);
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 4. 启动 VI（使用传感器特定配置）
    sample_vi_cfg vi_cfg;
    sample_comm_vi_get_default_vi_cfg(g_sns_type, &vi_cfg);
    
    // 设置绑定信息
    vi_cfg.bind_pipe.pipe_num = 1;
    vi_cfg.bind_pipe.pipe_id[0] = DEFAULT_PIPE_ID;
    
    // 设置管道信息
    vi_cfg.pipe_info[0].pipe_need_start = TD_TRUE;
    vi_cfg.pipe_info[0].isp_need_run = TD_TRUE;
    vi_cfg.pipe_info[0].is_master_pipe = TD_TRUE;
    
    // 设置通道信息
    vi_cfg.pipe_info[0].chn_num = 1;
    vi_cfg.pipe_info[0].chn_info[0].vi_chn = 0;
    
    printf("Starting VI...\n");
    if (sample_comm_vi_start_vi(&vi_cfg) != TD_SUCCESS) {
        printf("Failed to start VI\n");
        ss_mpi_isp_exit(DEFAULT_PIPE_ID);
        sample_comm_isp_stop(DEFAULT_PIPE_ID);
        sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
        sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
        sample_comm_sys_exit();
        sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
        return -1;
    }
    
    // 6. 捕获图像
    printf("Capturing image...\n");
    
    // 这里使用VENC模块来保存JPEG图像
    ot_venc_chn venc_chn = 0;
    sample_comm_venc_chn_param venc_param;
    
    // 设置编码参数
    venc_param.frame_rate = 30;
    venc_param.gop = 1;
    venc_param.venc_size.width = width;
    venc_param.venc_size.height = height;
    venc_param.size = sample_comm_sys_get_pic_enum(&venc_param.venc_size);
    venc_param.profile = 0;
    venc_param.is_rcn_ref_share_buf = TD_FALSE;
    venc_param.type = OT_PT_JPEG;
    venc_param.rc_mode = SAMPLE_RC_FIXQP;
    
    // 创建并启动编码器
    if (sample_comm_venc_create(venc_chn, &venc_param) != TD_SUCCESS) {
        printf("Failed to create encoder\n");
        goto cleanup;
    }
    
    if (sample_comm_venc_start(venc_chn, &venc_param) != TD_SUCCESS) {
        printf("Failed to start encoder\n");
        goto cleanup;
    }
    
    // 启动抓拍
    ot_size snap_size = {width, height};
    if (sample_comm_venc_snap_start(venc_chn, &snap_size, TD_FALSE) != TD_SUCCESS) {
        printf("Failed to start snap\n");
        goto cleanup;
    }
    
    // 处理抓拍
    if (sample_comm_venc_snap_process(venc_chn, 1, TD_TRUE, TD_FALSE) != TD_SUCCESS) {
        printf("Failed to process snap\n");
        goto cleanup;
    }
    
    // 保存JPEG图像
    if (sample_comm_venc_save_jpeg(venc_chn, 0) != TD_SUCCESS) {
        printf("Failed to save JPEG\n");
        goto cleanup;
    }
    
    // 停止抓拍
    if (sample_comm_venc_snap_stop(venc_chn) != TD_SUCCESS) {
        printf("Failed to stop snap\n");
        goto cleanup;
    }
    
    // 重命名文件
    if (rename("snap_0.jpg", output_file) != 0) {
        printf("Failed to rename file\n");
        goto cleanup;
    }
    
    printf("Image captured successfully: %s\n", output_file);
    
cleanup:
    // 停止编码器
    sample_comm_venc_stop(venc_chn);
    
    // 停止VI
    sample_comm_vi_stop_vi(&vi_cfg);
    
    // 停止ISP
    ss_mpi_isp_exit(DEFAULT_PIPE_ID);
    sample_comm_isp_stop(DEFAULT_PIPE_ID);
    
    // 注销3A库
    sample_comm_isp_awb_lib_uncallback(DEFAULT_PIPE_ID);
    sample_comm_isp_ae_lib_uncallback(DEFAULT_PIPE_ID);
    
    // 注销传感器
    sample_comm_isp_sensor_unregiter_callback(DEFAULT_PIPE_ID);
    
    // 停止MIPI RX和传感器
    sample_comm_vi_stop_mipi_rx(&sns_info, &mipi_info);
    
    // 系统退出
    sample_comm_sys_exit();
    
    return 0;
}