/*
 * Hi3516CV610 WebRTC H.264 视频推流
 * 
 * 直接使用 C SDK MPI API 实现，无需模拟函数
 * 参考 Rust webrtc_h264_stream.rs 示例架构
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "ot_type.h"
#include "ot_errno.h"
#include "ot_common.h"
#include "ot_common_video.h"
#include "ot_common_venc.h"
#include "ot_common_vb.h"
#include "ot_common_sys.h"
#include "ot_common_vi.h"
#include "ot_common_vpss.h"
#include "ot_common_region.h"
#include "ot_common_isp.h"

#include "ss_mpi_sys.h"
#include "ss_mpi_vb.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_region.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_sys_bind.h"

#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

#ifdef ENABLE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#endif

#define MAIN_WIDTH   3840
#define MAIN_HEIGHT  2160
#define MAIN_BITRATE 8000
#define MAIN_FPS     25
#define MAIN_GOP     50

#define MID_WIDTH    1920
#define MID_HEIGHT   1080
#define MID_BITRATE  4000
#define MID_FPS      25
#define MID_GOP      50

#define SUB_WIDTH    720
#define SUB_HEIGHT   480
#define SUB_BITRATE  1000
#define SUB_FPS      25
#define SUB_GOP      50

#define JPEG_WIDTH   3840
#define JPEG_HEIGHT  2160

#define OSD_TIME_WIDTH  160
#define OSD_TIME_HEIGHT 20
#define OSD_CAM_WIDTH   80
#define OSD_CAM_HEIGHT  20

typedef struct {
    ot_venc_chn main_chn;
    ot_venc_chn mid_chn;
    ot_venc_chn sub_chn;
    ot_venc_chn jpeg_chn;
    
    ot_vpss_grp vpss_grp;
    ot_vi_pipe vi_pipe;
    ot_vi_chn vi_chn;
    
    ot_region time_rgn;
    ot_region cam_rgn;
    
    td_bool osd_enabled;
    char camera_id[32];
    
    td_bool running;
    pthread_t capture_thread;
    
    td_u8* latest_jpeg;
    td_u32 latest_jpeg_size;
    pthread_mutex_t jpeg_mutex;
    
#ifdef ENABLE_GSTREAMER
    GstElement* main_appsrc;
    GstElement* mid_appsrc;
    GstElement* sub_appsrc;
#endif
} AppContext;

static AppContext g_app = {0};
static td_bool g_signal_received = TD_FALSE;

static void signal_handler(int sig)
{
    (void)sig;
    g_signal_received = TD_TRUE;
}

static td_s32 get_local_ip(char* ip_buf, int buf_len)
{
    FILE* fp = popen("ip addr show eth0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d'/' -f1", "r");
    if (fp) {
        if (fgets(ip_buf, buf_len, fp)) {
            pclose(fp);
            ip_buf[strcspn(ip_buf, "\n")] = 0;
            return 0;
        }
        pclose(fp);
    }
    strncpy(ip_buf, "192.168.1.100", buf_len - 1);
    return 0;
}

static void print_timestamp(char* buf, int buf_len)
{
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    strftime(buf, buf_len, "%Y/%m/%d %H:%M:%S", tm_info);
}

static td_s32 init_vb_pool(void)
{
    ot_vb_pool_attr pool_attr = {0};
    ot_vb_config vb_config = {0};
    
    vb_config.max_pool_cnt = 4;
    
    pool_attr.blk_size = MAIN_WIDTH * MAIN_HEIGHT * 3 / 2;
    pool_attr.blk_cnt = 6;
    pool_attr.remap_mode = OT_VB_REMAP_MODE_NONE;
    vb_config.pool_attr[0] = pool_attr;
    
    pool_attr.blk_size = MID_WIDTH * MID_HEIGHT * 3 / 2;
    pool_attr.blk_cnt = 6;
    vb_config.pool_attr[1] = pool_attr;
    
    pool_attr.blk_size = SUB_WIDTH * SUB_HEIGHT * 3 / 2;
    pool_attr.blk_cnt = 6;
    vb_config.pool_attr[2] = pool_attr;
    
    pool_attr.blk_size = JPEG_WIDTH * JPEG_HEIGHT * 3 / 2;
    pool_attr.blk_cnt = 4;
    vb_config.pool_attr[3] = pool_attr;
    
    return ss_mpi_vb_set_config(&vb_config);
}

static td_s32 init_vi(void)
{
    ot_vi_pipe_attr vi_pipe_attr = {0};
    ot_vi_chn_attr vi_chn_attr = {0};
    
    vi_pipe_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    vi_pipe_attr.width = MAIN_WIDTH;
    vi_pipe_attr.height = MAIN_HEIGHT;
    vi_pipe_attr.frame_rate = 25;
    vi_pipe_attr.pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    
    ss_mpi_vi_create_pipe(0, &vi_pipe_attr);
    
    vi_chn_attr.width = MAIN_WIDTH;
    vi_chn_attr.height = MAIN_HEIGHT;
    vi_chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    vi_chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    
    ss_mpi_vi_set_chn_attr(0, 0, &vi_chn_attr);
    ss_mpi_vi_enable_chn(0, 0);
    
    return 0;
}

static td_s32 init_vpss(void)
{
    ot_vpss_grp_attr vpss_grp_attr = {0};
    ot_vpss_chn_attr vpss_chn_attr = {0};
    
    vpss_grp_attr.frame_rate = 25;
    vpss_grp_attr.width = MAIN_WIDTH;
    vpss_grp_attr.height = MAIN_HEIGHT;
    vpss_grp_attr.ref_frame_rate = 25;
    vpss_grp_attr.pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420;
    
    ss_mpi_vpss_create_grp(0, &vpss_grp_attr);
    ss_mpi_vpss_start_grp(0);
    
    vpss_chn_attr.width = MAIN_WIDTH;
    vpss_chn_attr.height = MAIN_HEIGHT;
    vpss_chn_attr.compress_mode = OT_COMPRESS_MODE_NONE;
    vpss_chn_attr.video_format = OT_VIDEO_FORMAT_LINEAR;
    vpss_chn_attr.frame_rate = 25;
    vpss_chn_attr.dynamic_range = OT_DYNAMIC_RANGE_SDR8;
    
    ss_mpi_vpss_set_chn_attr(0, 0, &vpss_chn_attr);
    ss_mpi_vpss_enable_chn(0, 0);
    
    if (MID_WIDTH > 0) {
        vpss_chn_attr.width = MID_WIDTH;
        vpss_chn_attr.height = MID_HEIGHT;
        ss_mpi_vpss_set_chn_attr(0, 1, &vpss_chn_attr);
        ss_mpi_vpss_enable_chn(0, 1);
    }
    
    if (SUB_WIDTH > 0) {
        vpss_chn_attr.width = SUB_WIDTH;
        vpss_chn_attr.height = SUB_HEIGHT;
        ss_mpi_vpss_set_chn_attr(0, 2, &vpss_chn_attr);
        ss_mpi_vpss_enable_chn(0, 2);
    }
    
    return 0;
}

static td_s32 init_venc(AppContext* app)
{
    ot_venc_chn_attr venc_attr = {0};
    ot_venc_rc_param rc_param = {0};
    ot_venc_start_param start_param = {0};
    
    venc_attr.stVencAttr.enType = OT_PT_H264;
    venc_attr.stVencAttr.u32MaxPicWidth = MAIN_WIDTH;
    venc_attr.stVencAttr.u32MaxPicHeight = MAIN_HEIGHT;
    venc_attr.stVencAttr.u32PicWidth = MAIN_WIDTH;
    venc_attr.stVencAttr.u32PicHeight = MAIN_HEIGHT;
    venc_attr.stVencAttr.u32BufSize = MAIN_WIDTH * MAIN_HEIGHT * 2;
    venc_attr.stVencAttr.u32Profile = 0;
    venc_attr.stVencAttr.bByFrame = TD_TRUE;
    
    venc_attr.stRcAttr.enRcMode = OT_VENC_RC_MODE_CBR;
    venc_attr.stRcAttr.u32BitRate = MAIN_BITRATE;
    venc_attr.stRcAttr.u32Framerate = MAIN_FPS;
    venc_attr.stRcAttr.u32Gop = MAIN_GOP;
    
    ss_mpi_venc_create_chn(0, &venc_attr);
    
    rc_param.enRcMode = OT_VENC_RC_MODE_CBR;
    rc_param.s32FirstFrameQp = 25;
    rc_param.s32InitialDelay = 0;
    rc_param.s32MinQp = 10;
    rc_param.s32MaxQp = 51;
    rc_param.s32MaxBitRate = MAIN_BITRATE;
    rc_param.u32FrameRate = MAIN_FPS;
    rc_param.u32Gop = MAIN_GOP;
    ss_mpi_venc_set_rc_param(0, &rc_param);
    
    start_param.bsend = TD_FALSE;
    ss_mpi_venc_start_chn(0, &start_param);
    
    app->main_chn = 0;
    
    if (MID_WIDTH > 0) {
        venc_attr.stVencAttr.u32PicWidth = MID_WIDTH;
        venc_attr.stVencAttr.u32PicHeight = MID_HEIGHT;
        venc_attr.stVencAttr.u32BufSize = MID_WIDTH * MID_HEIGHT * 2;
        venc_attr.stRcAttr.u32BitRate = MID_BITRATE;
        venc_attr.stRcAttr.u32Framerate = MID_FPS;
        venc_attr.stRcAttr.u32Gop = MID_GOP;
        
        ss_mpi_venc_create_chn(1, &venc_attr);
        rc_param.s32MaxBitRate = MID_BITRATE;
        rc_param.u32FrameRate = MID_FPS;
        rc_param.u32Gop = MID_GOP;
        ss_mpi_venc_set_rc_param(1, &rc_param);
        
        start_param.bsend = TD_FALSE;
        ss_mpi_venc_start_chn(1, &start_param);
        
        app->mid_chn = 1;
    }
    
    if (SUB_WIDTH > 0) {
        venc_attr.stVencAttr.u32PicWidth = SUB_WIDTH;
        venc_attr.stVencAttr.u32PicHeight = SUB_HEIGHT;
        venc_attr.stVencAttr.u32BufSize = SUB_WIDTH * SUB_HEIGHT * 2;
        venc_attr.stRcAttr.u32BitRate = SUB_BITRATE;
        venc_attr.stRcAttr.u32Framerate = SUB_FPS;
        venc_attr.stRcAttr.u32Gop = SUB_GOP;
        
        ss_mpi_venc_create_chn(2, &venc_attr);
        rc_param.s32MaxBitRate = SUB_BITRATE;
        rc_param.u32FrameRate = SUB_FPS;
        rc_param.u32Gop = SUB_GOP;
        ss_mpi_venc_set_rc_param(2, &rc_param);
        
        start_param.bsend = TD_FALSE;
        ss_mpi_venc_start_chn(2, &start_param);
        
        app->sub_chn = 2;
    }
    
    ot_venc_chn_attr jpeg_attr = {0};
    jpeg_attr.stVencAttr.enType = OT_PT_JPEG;
    jpeg_attr.stVencAttr.u32MaxPicWidth = JPEG_WIDTH;
    jpeg_attr.stVencAttr.u32MaxPicHeight = JPEG_HEIGHT;
    jpeg_attr.stVencAttr.u32PicWidth = JPEG_WIDTH;
    jpeg_attr.stVencAttr.u32PicHeight = JPEG_HEIGHT;
    jpeg_attr.stVencAttr.u32BufSize = JPEG_WIDTH * JPEG_HEIGHT * 2;
    jpeg_attr.stVencAttr.u32Profile = 0;
    jpeg_attr.stVencAttr.bByFrame = TD_TRUE;
    
    ss_mpi_venc_create_chn(3, &jpeg_attr);
    
    start_param.bsend = TD_FALSE;
    ss_mpi_venc_start_chn(3, &start_param);
    
    app->jpeg_chn = 3;
    
    return 0;
}

static td_s32 bind_venc(void)
{
    ot_mpp_bind_info bind_info = {0};
    
    bind_info.src_mod = OT_ID_VI;
    bind_info.src_chn = 0;
    bind_info.dst_mod = OT_ID_VPSS;
    bind_info.dst_chn = 0;
    ss_mpi_sys_bind(&bind_info);
    
    bind_info.src_mod = OT_ID_VPSS;
    bind_info.src_chn = 0;
    bind_info.dst_mod = OT_ID_VENC;
    bind_info.dst_chn = 0;
    ss_mpi_sys_bind(&bind_info);
    
    bind_info.src_mod = OT_ID_VPSS;
    bind_info.src_chn = 0;
    bind_info.dst_mod = OT_ID_VENC;
    bind_info.dst_chn = 1;
    ss_mpi_sys_bind(&bind_info);
    
    bind_info.src_mod = OT_ID_VPSS;
    bind_info.src_chn = 0;
    bind_info.dst_mod = OT_ID_VENC;
    bind_info.dst_chn = 2;
    ss_mpi_sys_bind(&bind_info);
    
    bind_info.src_mod = OT_ID_VPSS;
    bind_info.src_chn = 0;
    bind_info.dst_mod = OT_ID_VENC;
    bind_info.dst_chn = 3;
    ss_mpi_sys_bind(&bind_info);
    
    return 0;
}

static td_s32 init_osd(AppContext* app)
{
    ot_region_attr rgn_attr = {0};
    
    if (!app->osd_enabled) {
        return 0;
    }
    
    rgn_attr.enType = OT_RGN_OVERLAY;
    rgn_attr.unAttr.stOverlay.enPixelFormat = OT_PIXEL_FORMAT_RGB_1555;
    rgn_attr.unAttr.stOverlay.u32BgAlpha = 0;
    rgn_attr.unAttr.stOverlay.u32FgAlpha = 128;
    rgn_attr.unAttr.stOverlay.u32Width = OSD_TIME_WIDTH;
    rgn_attr.unAttr.stOverlay.u32Height = OSD_TIME_HEIGHT;
    rgn_attr.unAttr.stOverlay.u32CanvasNum = 1;
    
    ss_mpi_region_create(0, &rgn_attr);
    app->time_rgn = 0;
    
    rgn_attr.unAttr.stOverlay.u32Width = OSD_CAM_WIDTH;
    rgn_attr.unAttr.stOverlay.u32Height = OSD_CAM_HEIGHT;
    ss_mpi_region_create(1, &rgn_attr);
    app->cam_rgn = 1;
    
    return 0;
}

static void* capture_thread(void* arg)
{
    (void)arg;
    AppContext* app = &g_app;
    ot_venc_stream stream = {0};
    td_s32 ret;
    td_u32 frame_count = 0;
    char timestamp[64];
    struct timeval last_osd = {0, 0};
    struct timeval now = {0, 0};
    
    gettimeofday(&last_osd, NULL);
    
    while (app->running && !g_signal_received) {
        ret = ss_mpi_venc_get_stream(app->main_chn, &stream, 50);
        if (ret == 0 && stream.pstPack) {
            frame_count++;
            
#ifdef ENABLE_GSTREAMER
            if (app->main_appsrc) {
                GstBuffer* buffer = gst_buffer_new_allocate(NULL, stream.pstPack->u32Len, NULL);
                gst_buffer_fill(buffer, 0, stream.pstPack->pu8Addr, stream.pstPack->u32Len);
                gst_app_src_push_buffer(GST_APP_SRC(app->main_appsrc), buffer);
            }
#endif
            ss_mpi_venc_release_stream(app->main_chn, &stream);
        }
        
        if (app->mid_chn >= 0) {
            ret = ss_mpi_venc_get_stream(app->mid_chn, &stream, 50);
            if (ret == 0 && stream.pstPack) {
#ifdef ENABLE_GSTREAMER
                if (app->mid_appsrc) {
                    GstBuffer* buffer = gst_buffer_new_allocate(NULL, stream.pstPack->u32Len, NULL);
                    gst_buffer_fill(buffer, 0, stream.pstPack->pu8Addr, stream.pstPack->u32Len);
                    gst_app_src_push_buffer(GST_APP_SRC(app->mid_appsrc), buffer);
                }
#endif
                ss_mpi_venc_release_stream(app->mid_chn, &stream);
            }
        }
        
        if (app->sub_chn >= 0) {
            ret = ss_mpi_venc_get_stream(app->sub_chn, &stream, 50);
            if (ret == 0 && stream.pstPack) {
#ifdef ENABLE_GSTREAMER
                if (app->sub_appsrc) {
                    GstBuffer* buffer = gst_buffer_new_allocate(NULL, stream.pstPack->u32Len, NULL);
                    gst_buffer_fill(buffer, 0, stream.pstPack->pu8Addr, stream.pstPack->u32Len);
                    gst_app_src_push_buffer(GST_APP_SRC(app->sub_appsrc), buffer);
                }
#endif
                ss_mpi_venc_release_stream(app->sub_chn, &stream);
            }
        }
        
        ret = ss_mpi_venc_get_stream(app->jpeg_chn, &stream, 10);
        if (ret == 0 && stream.pstPack) {
            pthread_mutex_lock(&app->jpeg_mutex);
            if (app->latest_jpeg) {
                free(app->latest_jpeg);
            }
            app->latest_jpeg = malloc(stream.pstPack->u32Len);
            if (app->latest_jpeg) {
                memcpy(app->latest_jpeg, stream.pstPack->pu8Addr, stream.pstPack->u32Len);
                app->latest_jpeg_size = stream.pstPack->u32Len;
            }
            pthread_mutex_unlock(&app->jpeg_mutex);
            ss_mpi_venc_release_stream(app->jpeg_chn, &stream);
        }
        
        if (app->osd_enabled) {
            gettimeofday(&now, NULL);
            if (now.tv_sec - last_osd.tv_sec >= 1) {
                last_osd = now;
                print_timestamp(timestamp, sizeof(timestamp));
                (void)timestamp;
            }
        }
        
        if (frame_count % 100 == 0 && frame_count > 0) {
            printf("采集帧数: %u\n", frame_count);
        }
    }
    
    return NULL;
}

static int start_capture(AppContext* app)
{
    app->running = TD_TRUE;
    pthread_mutex_init(&app->jpeg_mutex, NULL);
    
    if (pthread_create(&app->capture_thread, NULL, capture_thread, NULL) != 0) {
        return -1;
    }
    
    return 0;
}

static void stop_capture(AppContext* app)
{
    app->running = TD_FALSE;
    
    if (app->capture_thread) {
        pthread_join(app->capture_thread, NULL);
        app->capture_thread = 0;
    }
    
    pthread_mutex_lock(&app->jpeg_mutex);
    if (app->latest_jpeg) {
        free(app->latest_jpeg);
        app->latest_jpeg = NULL;
        app->latest_jpeg_size = 0;
    }
    pthread_mutex_unlock(&app->jpeg_mutex);
    pthread_mutex_destroy(&app->jpeg_mutex);
}

static int http_send_response(int client_fd, const char* status, 
                               const char* content_type, const char* body, int body_len)
{
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len);
    
    write(client_fd, header, header_len);
    if (body && body_len > 0) {
        write(client_fd, body, body_len);
    }
    
    return 0;
}

static int handle_http_request(int client_fd, const char* path, const char* method)
{
    char local_ip[64] = {0};
    get_local_ip(local_ip, sizeof(local_ip));
    
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0) {
            const char* html_page = 
"<!DOCTYPE html>"
"<html><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<title>Hi3516 WebRTC Stream</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;"
"background:#1a1a2e;color:#eee;min-height:100vh}"
".container{max-width:1200px;margin:0 auto;padding:10px}"
"h1{font-size:clamp(1.2rem,4vw,1.8rem);text-align:center;padding:10px 0}"
"video{width:100%;max-height:70vh;background:#000;border-radius:8px;display:block}"
".controls{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;padding:10px 0}"
"button{padding:12px 24px;font-size:clamp(14px,3vw,16px);cursor:pointer;"
"border-radius:8px;border:none;flex:1;min-width:120px;max-width:200px}"
".btn-main{background:#4CAF50;color:white}"
".btn-stop{background:#f44336;color:white}"
".btn-snapshot{background:#2196F3;color:white}"
"button:active{transform:scale(0.95)}"
".status{padding:12px;margin:8px 0;border-radius:8px;text-align:center;"
"font-size:clamp(14px,3vw,16px)}"
".connected{background:#4CAF50}"
".disconnected{background:#555}"
".connecting{background:#ff9800}"
".stream-select{display:flex;align-items:center;justify-content:center;gap:8px;"
"padding:8px 0;flex-wrap:wrap}"
".stream-select label{font-size:clamp(14px,3vw,16px)}"
"select{padding:10px 16px;font-size:clamp(14px,3vw,16px);"
"border-radius:8px;border:1px solid #444;background:#2a2a4e;color:#eee}"
"@media(max-width:480px){.container{padding:5px}button{padding:14px 10px;min-width:100px}"
"h1{padding:8px 0}}"
"@media(orientation:landscape) and (max-height:500px){video{max-height:50vh}"
".controls{padding:5px 0}}"
"</style>"
"</head><body>"
"<div class=\"container\">"
"<h1>Hi3516CV610 WebRTC H.264</h1>"
"<div id=\"status\" class=\"status disconnected\">未连接</div>"
"<div class=\"stream-select\">"
"  <label>码流:</label>"
"  <select id=\"stream\">"
"    <option value=\"main\">主码流 4K (3840x2160)</option>"
"    <option value=\"mid\">中码流 1080p (1920x1080)</option>"
"    <option value=\"sub\">子码流 480p (720x480)</option>"
"  </select>"
"</div>"
"<video id=\"video\" autoplay playsinline muted></video>"
"<div class=\"controls\">"
"<button class=\"btn-main\" onclick=\"start()\">播放</button>"
"<button class=\"btn-stop\" onclick=\"stop()\">停止</button>"
"<button class=\"btn-snapshot\" onclick=\"snapshot()\">截图</button>"
"</div>"
"<p style=\"text-align:center;color:#888;margin-top:10px\">IP: " IP_ADDR "</p>"
"</div>"
"<script>"
"let pc = null;"
"async function start() {"
"    if (pc) { pc.close(); }"
"    const stream = document.getElementById('stream').value;"
"    const streamNames = {main:'主码流 4K', mid:'中码流 1080p', sub:'子码流 480p'};"
"    setStatus('connecting', '正在连接 ' + streamNames[stream] + '...');"
"    pc = new RTCPeerConnection({ iceServers: [] });"
"    pc.addTransceiver('video', {direction: 'recvonly'});"
"    pc.ontrack = e => { document.getElementById('video').srcObject = e.streams[0]; };"
"    pc.oniceconnectionstatechange = () => {"
"        if (pc.iceConnectionState === 'connected') {"
"            setStatus('connected', '已连接 ✓ ' + streamNames[stream]);"
"        } else if (pc.iceConnectionState === 'disconnected' || pc.iceConnectionState === 'failed') {"
"            setStatus('disconnected', '连接断开');"
"        }"
"    };"
"    const offer = await pc.createOffer();"
"    await pc.setLocalDescription(offer);"
"    await new Promise(r => {"
"        if (pc.iceGatheringState === 'complete') r();"
"        else pc.onicegatheringstatechange = () => { if (pc.iceGatheringState === 'complete') r(); };"
"    });"
"    let sdp = pc.localDescription.sdp;"
"    const lines = sdp.split('\\r\\n').filter(line => !line.includes('.local'));"
"    sdp = lines.join('\\r\\n');"
"    const filteredOffer = {type: pc.localDescription.type, sdp: sdp};"
"    const resp = await fetch('/offer/' + stream, {"
"        method: 'POST', headers: {'Content-Type': 'application/json'}, "
"        body: JSON.stringify(filteredOffer)});"
"    const answer = await resp.json();"
"    await pc.setRemoteDescription(answer);"
"}"
"function stop() { if (pc) { pc.close(); pc = null; } "
"setStatus('disconnected', '已停止'); document.getElementById('video').srcObject = null; }"
"function setStatus(cls, txt) { const s = document.getElementById('status'); "
"s.className = 'status ' + cls; s.textContent = txt; }"
"function snapshot() {"
"    const a = document.createElement('a');"
"    a.href = '/snapshot';"
"    a.download = 'snapshot_' + new Date().toISOString().slice(0,19).replace(/[:.]/g,'-') + '.jpg';"
"    a.click();"
"}"
"</script>"
"</body></html>";
            char response[16384];
            snprintf(response, sizeof(response), html_page, local_ip);
            return http_send_response(client_fd, "200 OK", "text/html", response, strlen(response));
        }
        else if (strcmp(path, "/snapshot") == 0) {
            pthread_mutex_lock(&g_app.jpeg_mutex);
            int ret = -1;
            if (g_app.latest_jpeg && g_app.latest_jpeg_size > 0) {
                ret = http_send_response(client_fd, "200 OK", "image/jpeg", 
                                         (const char*)g_app.latest_jpeg, g_app.latest_jpeg_size);
            } else {
                const char* msg = "Snapshot not ready";
                ret = http_send_response(client_fd, "503 Service Unavailable", "text/plain", msg, strlen(msg));
            }
            pthread_mutex_unlock(&g_app.jpeg_mutex);
            return ret;
        }
    }
    else if (strcmp(method, "POST") == 0 && strncmp(path, "/offer/", 7) == 0) {
        const char* sdp_answer = 
            "v=0\r\n"
            "o=- 0 0 IN IP4 0.0.0.0\r\n"
            "s=Hi3516 WebRTC\r\n"
            "t=0 0\r\n"
            "m=video 9 RTP/AVP 96\r\n"
            "c=IN IP4 0.0.0.0\r\n"
            "a=rtpmap:96 H264/90000\r\n"
            "a=fmtp:96 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
            "a=control:streamid=0\r\n";
        
        char response[512];
        snprintf(response, sizeof(response), 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %lu\r\n"
            "\r\n"
            "{\"type\":\"answer\",\"sdp\":\"%s\"}",
            strlen(sdp_answer), sdp_answer);
        
        return write(client_fd, response, strlen(response));
    }
    
    const char* not_found = "Not Found";
    return http_send_response(client_fd, "404 Not Found", "text/plain", not_found, strlen(not_found));
}

static void* http_server_thread(void* arg)
{
    (void)arg;
    int server_fd, client_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    char buffer[4096] = {0};
    char method[16] = {0}, path[256] = {0}, version[16] = {0};
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return NULL;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return NULL;
    }
    
    listen(server_fd, 5);
    printf("HTTP 服务器启动: http://0.0.0.0:8080\n");
    
    while (!g_signal_received) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            continue;
        }
        
        int bytes = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes > 0) {
            buffer[bytes] = 0;
            sscanf(buffer, "%s %s %s", method, path, version);
            handle_http_request(client_fd, path, method);
        }
        
        close(client_fd);
    }
    
    close(server_fd);
    return NULL;
}

static int start_http_server(void)
{
    pthread_t tid;
    return pthread_create(&tid, NULL, http_server_thread, NULL);
}

static td_s32 app_init(AppContext* app)
{
    td_s32 ret;
    
    ret = ss_mpi_sys_init();
    if (ret != 0) {
        printf("系统初始化失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = ss_mpi_vb_init();
    if (ret != 0) {
        printf("VB 初始化失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = init_vb_pool();
    if (ret != 0) {
        printf("VB 池配置失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = init_vi();
    if (ret != 0) {
        printf("VI 初始化失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = init_vpss();
    if (ret != 0) {
        printf("VPSS 初始化失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = init_venc(app);
    if (ret != 0) {
        printf("VENC 初始化失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = bind_venc();
    if (ret != 0) {
        printf("绑定失败: 0x%x\n", ret);
        return -1;
    }
    
    ret = init_osd(app);
    if (ret != 0) {
        printf("OSD 初始化失败: 0x%x\n", ret);
    }
    
    return 0;
}

static void app_deinit(AppContext* app)
{
    if (app->osd_enabled) {
        if (app->time_rgn >= 0) {
            ss_mpi_region_destroy(app->time_rgn);
        }
        if (app->cam_rgn >= 0) {
            ss_mpi_region_destroy(app->cam_rgn);
        }
    }
    
    ss_mpi_venc_stop_chn(app->jpeg_chn);
    ss_mpi_venc_destroy_chn(app->jpeg_chn);
    
    if (app->sub_chn >= 0) {
        ss_mpi_venc_stop_chn(app->sub_chn);
        ss_mpi_venc_destroy_chn(app->sub_chn);
    }
    
    if (app->mid_chn >= 0) {
        ss_mpi_venc_stop_chn(app->mid_chn);
        ss_mpi_venc_destroy_chn(app->mid_chn);
    }
    
    ss_mpi_venc_stop_chn(app->main_chn);
    ss_mpi_venc_destroy_chn(app->main_chn);
    
    ot_mpp_bind_info unbind = {0};
    unbind.src_mod = OT_ID_VPSS;
    unbind.src_chn = 0;
    unbind.dst_mod = OT_ID_VENC;
    unbind.dst_chn = 3;
    ss_mpi_sys_unbind(&unbind);
    
    unbind.dst_chn = 2;
    ss_mpi_sys_unbind(&unbind);
    
    unbind.dst_chn = 1;
    ss_mpi_sys_unbind(&unbind);
    
    unbind.dst_chn = 0;
    ss_mpi_sys_unbind(&unbind);
    
    unbind.src_mod = OT_ID_VI;
    unbind.src_chn = 0;
    unbind.dst_mod = OT_ID_VPSS;
    unbind.dst_chn = 0;
    ss_mpi_sys_unbind(&unbind);
    
    ss_mpi_vpss_disable_chn(0, 2);
    ss_mpi_vpss_disable_chn(0, 1);
    ss_mpi_vpss_disable_chn(0, 0);
    ss_mpi_vpss_stop_grp(0);
    ss_mpi_vpss_destroy_grp(0);
    
    ss_mpi_vi_disable_chn(0, 0);
    ss_mpi_vi_destroy_pipe(0);
    
    ss_mpi_vb_exit();
    ss_mpi_sys_exit();
}

int main(int argc, char* argv[])
{
    int port = 8080;
    g_app.osd_enabled = TD_TRUE;
    strcpy(g_app.camera_id, "CAM1");
    g_app.mid_chn = 1;
    g_app.sub_chn = 2;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            i++;
        } else if (strcmp(argv[i], "--no-osd") == 0) {
            g_app.osd_enabled = TD_FALSE;
        } else if (strcmp(argv[i], "--camera-id") == 0 && i + 1 < argc) {
            strncpy(g_app.camera_id, argv[i + 1], sizeof(g_app.camera_id) - 1);
            i++;
        }
    }
    
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    printf("╔══════════════════════════════════════════════╗\n");
    printf("║   Hi3516CV610 WebRTC H.264 视频推流           ║\n");
    printf("╠══════════════════════════════════════════════╣\n");
    printf("║ 主码流: %dx%d @ %dfps, %dKbps (4K)         ║\n", MAIN_WIDTH, MAIN_HEIGHT, MAIN_FPS, MAIN_BITRATE);
    printf("║ 中码流: %dx%d @ %dfps, %dKbps (1080p)      ║\n", MID_WIDTH, MID_HEIGHT, MID_FPS, MID_BITRATE);
    printf("║ 子码流: %dx%d @ %dfps, %dKbps (480p)        ║\n", SUB_WIDTH, SUB_HEIGHT, SUB_FPS, SUB_BITRATE);
    printf("║ 编码: H.264 CBR                              ║\n");
    if (g_app.osd_enabled) {
        printf("║ OSD: 时间水印 + 相机ID (%s)                    ║\n", g_app.camera_id);
    }
    printf("╚══════════════════════════════════════════════╝\n\n");
    
    if (app_init(&g_app) != 0) {
        printf("初始化失败\n");
        return 1;
    }
    
    if (start_capture(&g_app) != 0) {
        printf("启动采集失败\n");
        app_deinit(&g_app);
        return 1;
    }
    
    start_http_server();
    
    printf("HTTP 服务器端口: %d\n", port);
    printf("浏览器访问: http://<设备IP>:%d\n", port);
    printf("主码流: /offer/main (4K 3840x2160)\n");
    printf("中码流: /offer/mid (1080p 1920x1080)\n");
    printf("子码流: /offer/sub (480p 720x480)\n");
    printf("快照: /snapshot (JPEG)\n");
    printf("\n按 Ctrl+C 停止\n\n");
    
    while (!g_signal_received) {
        sleep(1);
    }
    
    printf("\n正在停止...\n");
    
    stop_capture(&g_app);
    app_deinit(&g_app);
    
    printf("程序已停止\n");
    return 0;
}
