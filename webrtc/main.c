#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "ot_type.h"
#include "ot_common_vi.h"
#include "ot_common_vpss.h"
#include "ot_common_venc.h"
#include "ot_common_rc.h"
#include "ot_common_region.h"
#include "ot_common_sys.h"
#include "ss_mpi_vi.h"
#include "ss_mpi_vpss.h"
#include "ss_mpi_venc.h"
#include "ss_mpi_region.h"
#include "ss_mpi_sys.h"
#include "ot_common_vb.h"
#include "ss_mpi_vb.h"
#include "ot_common_sns.h"
#include "hi_mipi_rx.h"
#include "ss_mpi_isp.h"
#include "ss_mpi_sys_bind.h"
#include "ss_mpi_ae.h"
#include "ss_mpi_awb.h"
#include "ot_sns_ctrl.h"

extern ot_isp_sns_obj g_sns_imx415_obj;

#define MAIN_WIDTH 3840
#define MAIN_HEIGHT 2160
#define MAIN_BITRATE 8000000
#define MAIN_FPS 25
#define MAIN_GOP 50

#define MID_WIDTH 1920
#define MID_HEIGHT 1080
#define MID_BITRATE 4000000
#define MID_FPS 25
#define MID_GOP 50

#define SUB_WIDTH 720
#define SUB_HEIGHT 480
#define SUB_BITRATE 1000000
#define SUB_FPS 25
#define SUB_GOP 50

#define JPEG_WIDTH 1920
#define JPEG_HEIGHT 1080

typedef struct {
    ot_vi_pipe vi_pipe;
    ot_vi_chn vi_chn;
    ot_vpss_grp vpss_grp;
    ot_vpss_chn vpss_chn_main;
    ot_vpss_chn vpss_chn_mid;
    ot_vpss_chn vpss_chn_sub;
    ot_venc_chn venc_chn_main;
    ot_venc_chn venc_chn_mid;
    ot_venc_chn venc_chn_sub;
    ot_venc_chn jpeg_chn;
    ot_rgn_handle time_rgn;
    ot_rgn_handle cam_rgn;
    uint8_t* latest_jpeg;
    size_t latest_jpeg_size;
    pthread_mutex_t jpeg_mutex;
    int osd_enabled;
} AppContext;

AppContext g_app = {
    .latest_jpeg = NULL,
    .latest_jpeg_size = 0,
    .osd_enabled = 1
};

static int http_send_response(int client_fd, const char* status, const char* content_type, const char* body, size_t body_len)
{
    char header[512];
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body_len);
    
    write(client_fd, header, header_len);
    if (body && body_len > 0) {
        write(client_fd, body, body_len);
    }
    
    return 0;
}

static void get_local_ip(char* ip, size_t size)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        strncpy(ip, "127.0.0.1", size);
        return;
    }
    
    struct sockaddr_in serv;
    memset(&serv, 0, sizeof(serv));
    serv.sin_family = AF_INET;
    serv.sin_addr.s_addr = inet_addr("8.8.8.8");
    serv.sin_port = htons(53);
    
    if (connect(sock, (struct sockaddr*)&serv, sizeof(serv)) < 0) {
        strncpy(ip, "127.0.0.1", size);
        close(sock);
        return;
    }
    
    struct sockaddr_in local;
    socklen_t local_len = sizeof(local);
    getsockname(sock, (struct sockaddr*)&local, &local_len);
    inet_ntop(AF_INET, &local.sin_addr, ip, size);
    close(sock);
}

static int handle_http_request(int client_fd, const char* path, const char* method)
{
    char local_ip[64] = {0};
    get_local_ip(local_ip, sizeof(local_ip));
    
    if (strcmp(method, "GET") == 0) {
        if (strcmp(path, "/") == 0) {
            const char* html_page = "<!DOCTYPE html>"
"<html><head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<title>Hi3516 WebRTC Stream</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}"
".container{max-width:1200px;margin:0 auto;padding:10px}"
"h1{font-size:clamp(1.2rem,4vw,1.8rem);text-align:center;padding:10px 0}"
"h2{font-size:clamp(1rem,3vw,1.4rem);text-align:center;padding:8px 0}"
"video{width:100%;max-height:50vh;background:#000;border-radius:8px;display:block}"
".controls{display:flex;flex-wrap:wrap;gap:8px;justify-content:center;padding:10px 0}"
"button{padding:12px 24px;font-size:clamp(14px,3vw,16px);cursor:pointer;border-radius:8px;border:none;flex:1;min-width:120px;max-width:200px}"
".btn-main{background:#4CAF50;color:white}"
".btn-stop{background:#f44336;color:white}"
".btn-snapshot{background:#2196F3;color:white}"
".btn-calib{background:#ff9800;color:white}"
".btn-download{background:#9c27b0;color:white}"
".btn-delete{background:#607d8b;color:white}"
"button:active{transform:scale(0.95)}"
".status{padding:12px;margin:8px 0;border-radius:8px;text-align:center;font-size:clamp(14px,3vw,16px)}"
".connected{background:#4CAF50}"
".disconnected{background:#555}"
".connecting{background:#ff9800}"
".success{background:#4CAF50}"
".error{background:#f44336}"
".stream-select{display:flex;align-items:center;justify-content:center;gap:8px;padding:8px 0;flex-wrap:wrap}"
".stream-select label{font-size:clamp(14px,3vw,16px)}"
"input[type=text]{padding:10px 16px;font-size:clamp(14px,3vw,16px);border-radius:8px;border:1px solid #444;background:#2a2a4e;color:#eee;flex:1;min-width:200px}"
"select{padding:10px 16px;font-size:clamp(14px,3vw,16px);border-radius:8px;border:1px solid #444;background:#2a2a4e;color:#eee}"
".config-section{background:#2a2a4e;border-radius:12px;padding:15px;margin:10px 0}"
".calib-section{background:#2a2a4e;border-radius:12px;padding:15px;margin:10px 0}"
"@media(max-width:480px){.container{padding:5px}button{padding:14px 10px;min-width:100px}h1{padding:8px 0}}"
"@media(orientation:landscape) and (max-height:500px){video{max-height:40vh}.controls{padding:5px 0}}"
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
"<div class=\"config-section\">"
"<h2>服务器配置</h2>"
"<div class=\"stream-select\">"
"  <label>服务器URL:</label>"
"  <input type=\"text\" id=\"serverUrl\" placeholder=\"http://服务器IP:端口\" value=\"http://47.107.225.196:5001\">"
"</div>"
"<div class=\"stream-select\">"
"  <label>棋盘格尺寸:</label>"
"  <input type=\"text\" id=\"chessboardSize\" placeholder=\"宽度,高度\" value=\"6,6\">"
"</div>"
"<div class=\"stream-select\">"
"  <label>方格大小(mm):</label>"
"  <input type=\"text\" id=\"squareSize\" placeholder=\"尺寸\" value=\"35.0\">"
"</div>"
"</div>"
"<div class=\"calib-section\">"
"<h2>相机标定</h2>"
"<div class=\"controls\">"
"<button class=\"btn-snapshot\" onclick=\"uploadCalibImage()\">上传标定图片</button>"
"<button class=\"btn-calib\" onclick=\"startCalibration()\">开始标定</button>"
"<button class=\"btn-download\" onclick=\"downloadCalibFile()\">下载标定文件</button>"
"<button class=\"btn-delete\" onclick=\"deleteCalibImages()\">删除标定图片</button>"
"</div>"
"<div id=\"calibStatus\" class=\"status\">标定状态: 未初始化</div>"
"</div>"
"<p style=\"text-align:center;color:#888;margin-top:10px\">IP: %s</p>"
"</div>"
"<script>"
"let pc = null;"
"let uploadInProgress = false;"
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
"function setCalibStatus(cls, txt) { const s = document.getElementById('calibStatus'); "
"s.className = 'status ' + cls; s.textContent = txt; }"
"function snapshot() {"
"    const a = document.createElement('a');"
"    a.href = '/snapshot';"
"    a.download = 'snapshot_' + new Date().toISOString().slice(0,19).replace(/[:.]/g,'-') + '.jpg';"
"    a.click();"
"}"
"async function uploadCalibImage() {"
"    if (uploadInProgress) return;"
"    uploadInProgress = true;"
"    setCalibStatus('connecting', '正在上传标定图片...');"
"    try {"
"        const response = await fetch('/snapshot');"
"        if (!response.ok) throw new Error('获取截图失败');"
"        const blob = await response.blob();"
"        const serverUrl = document.getElementById('serverUrl').value;"
"        const formData = new FormData();"
"        formData.append('files', blob, 'calib_image.jpg');"
"        const uploadResponse = await fetch(serverUrl + '/upload_calib_image', {"
"            method: 'POST',"
"            body: formData"
"        });"
"        if (!uploadResponse.ok) throw new Error('上传失败');"
"        const result = await uploadResponse.json();"
"        setCalibStatus('success', '标定图片上传成功: ' + result.message);"
"        // 自动检查标定状态"
"        await checkCalibrationStatus();"
"    } catch (error) {"
"        setCalibStatus('error', '上传失败: ' + error.message);"
"    } finally {"
"        uploadInProgress = false;"
"    }"
"}"
"async function startCalibration() {"
"    setCalibStatus('connecting', '正在进行相机标定...');"
"    try {"
"        const serverUrl = document.getElementById('serverUrl').value;"
"        const chessboardSize = document.getElementById('chessboardSize').value;"
"        const squareSize = document.getElementById('squareSize').value;"
"        const formData = new FormData();"
"        formData.append('chessboard_size', chessboardSize);"
"        formData.append('square_size', squareSize);"
"        const response = await fetch(serverUrl + '/start_calibration', {"
"            method: 'POST',"
"            body: formData"
"        });"
"        if (!response.ok) throw new Error('标定失败');"
"        const result = await response.json();"
"        setCalibStatus('success', '标定成功: 有效图片 ' + result.results.valid_images_count + '/' + result.results.total_images_count + ', 误差: ' + result.results.mean_reproj_error.toFixed(4));"
"    } catch (error) {"
"        setCalibStatus('error', '标定失败: ' + error.message);"
"    }"
"}"
"async function downloadCalibFile() {"
"    try {"
"        const serverUrl = document.getElementById('serverUrl').value;"
"        const response = await fetch(serverUrl + '/download/calib/camera_calib_params.xml');"
"        if (!response.ok) throw new Error('下载失败');"
"        const blob = await response.blob();"
"        const a = document.createElement('a');"
"        a.href = URL.createObjectURL(blob);"
"        a.download = 'camera_calib_params.xml';"
"        a.click();"
"        setCalibStatus('success', '标定文件下载成功');"
"    } catch (error) {"
"        setCalibStatus('error', '下载失败: ' + error.message);"
"    }"
"}"
"async function deleteCalibImages() {"
"    if (!confirm('确定要删除所有标定图片吗？')) return;"
"    setCalibStatus('connecting', '正在删除标定图片...');"
"    try {"
"        const serverUrl = document.getElementById('serverUrl').value;"
"        const response = await fetch(serverUrl + '/delete_calib_images', {"
"            method: 'DELETE'"
"        });"
"        if (!response.ok) throw new Error('删除失败');"
"        const result = await response.json();"
"        setCalibStatus('success', '删除成功: ' + result.message);"
"    } catch (error) {"
"        setCalibStatus('error', '删除失败: ' + error.message);"
"    }"
"}"
"async function checkCalibrationStatus() {"
"    try {"
"        const serverUrl = document.getElementById('serverUrl').value;"
"        const response = await fetch(serverUrl + '/calibration/status');"
"        if (!response.ok) throw new Error('获取状态失败');"
"        const result = await response.json();"
"        if (result.is_calibrated) {"
"            setCalibStatus('success', '标定状态: 已标定');"
"        } else if (result.has_calib_params_file) {"
"            setCalibStatus('connected', '标定状态: 有标定文件');"
"        } else {"
"            setCalibStatus('disconnected', '标定状态: 未标定');"
"        }"
"    } catch (error) {"
"        setCalibStatus('error', '获取状态失败: ' + error.message);"
"    }"
"}"
"// 页面加载时检查标定状态"
"window.onload = function() {"
"    checkCalibrationStatus();"
"};"
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
        printf("Received WebRTC offer request for path: %s\n", path);
        
        // 读取请求体
        char body[4096] = {0};
        int n = read(client_fd, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = '\0';
            printf("Received offer body (first 200 chars): %.200s...\n", body);
        }
        
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
        
        char response[1024];
        int sdp_len = strlen(sdp_answer);
        int resp_len = snprintf(response, sizeof(response), 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %u\r\n"
            "\r\n"
            "{\"type\":\"answer\",\"sdp\":\"%s\"}",
            sdp_len, sdp_answer);
        
        printf("Sending WebRTC answer, response length: %d\n", resp_len);
        int ret = write(client_fd, response, resp_len);
        if (ret < 0) {
            printf("write failed: %d\n", errno);
        } else {
            printf("WebRTC answer sent successfully\n");
        }
        return ret;
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
    
    printf("HTTP server thread started\n");
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        printf("socket creation failed: %d\n", errno);
        return NULL;
    }
    printf("Socket created successfully\n");
    
    int opt = 1;
    int ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret < 0) {
        printf("setsockopt failed: %d\n", errno);
        close(server_fd);
        return NULL;
    }
    
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(8080);
    
    ret = bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0) {
        printf("bind failed: %d\n", errno);
        close(server_fd);
        return NULL;
    }
    printf("Bound to port 8080 successfully\n");
    
    ret = listen(server_fd, 10);
    if (ret < 0) {
        printf("listen failed: %d\n", errno);
        close(server_fd);
        return NULL;
    }
    printf("Listening for connections on port 8080\n");
    
    while (1) {
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            printf("accept failed: %d\n", errno);
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &(client_addr.sin_addr), client_ip, INET_ADDRSTRLEN);
        printf("Received connection from %s:%d\n", client_ip, ntohs(client_addr.sin_port));
        
        int n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            buffer[n] = '\0';
            if (sscanf(buffer, "%s %s %s", method, path, version) == 3) {
                printf("Request: %s %s %s\n", method, path, version);
                handle_http_request(client_fd, path, method);
            } else {
                printf("Invalid request format\n");
                const char* error = "HTTP/1.1 400 Bad Request\r\nContent-Length: 15\r\n\r\nBad Request\n";
                write(client_fd, error, strlen(error));
            }
        }
        
        close(client_fd);
        printf("Connection closed\n");
    }
    
    close(server_fd);
    return NULL;
}

static int init_vpss()
{
    g_app.vpss_grp = 0;
    g_app.vpss_chn_main = 0;
    g_app.vpss_chn_mid = 1;
    g_app.vpss_chn_sub = 2;
    
    ot_vpss_grp_attr grp_attr = {
        .ie_en = TD_TRUE,
        .dci_en = TD_TRUE,
        .buf_share_en = TD_FALSE,
        .mcf_en = TD_TRUE,
        .max_width = MAIN_WIDTH,
        .max_height = MAIN_HEIGHT,
        .max_dei_width = MAIN_WIDTH,
        .max_dei_height = MAIN_HEIGHT,
        .dynamic_range = OT_DYNAMIC_RANGE_SDR8,
        .pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
        .dei_mode = OT_VPSS_DEI_MODE_OFF,
        .buf_share_chn = OT_VPSS_INVALID_CHN,
        .frame_rate = {
            .src_frame_rate = MAIN_FPS,
            .dst_frame_rate = MAIN_FPS
        }
    };
    
    int ret = ss_mpi_vpss_create_grp(g_app.vpss_grp, &grp_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_vpss_chn_attr chn_attr = {
        .mirror_en = TD_FALSE,
        .flip_en = TD_FALSE,
        .border_en = TD_FALSE,
        .width = MAIN_WIDTH,
        .height = MAIN_HEIGHT,
        .depth = 4,
        .chn_mode = OT_VPSS_CHN_MODE_AUTO,
        .video_format = OT_VIDEO_FORMAT_LINEAR,
        .dynamic_range = OT_DYNAMIC_RANGE_SDR8,
        .pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
        .compress_mode = OT_COMPRESS_MODE_NONE,
        .frame_rate = {
            .src_frame_rate = MAIN_FPS,
            .dst_frame_rate = MAIN_FPS
        },
        .border_attr = {
            .top_width = 0,
            .bottom_width = 0,
            .left_width = 0,
            .right_width = 0,
            .color = 0
        }
    };
    
    ret = ss_mpi_vpss_set_chn_attr(g_app.vpss_grp, g_app.vpss_chn_main, &chn_attr);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_vpss_enable_chn(g_app.vpss_grp, g_app.vpss_chn_main);
    if (ret != 0) {
        return ret;
    }
    
    chn_attr.width = MID_WIDTH;
    chn_attr.height = MID_HEIGHT;
    ret = ss_mpi_vpss_set_chn_attr(g_app.vpss_grp, g_app.vpss_chn_mid, &chn_attr);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_vpss_enable_chn(g_app.vpss_grp, g_app.vpss_chn_mid);
    if (ret != 0) {
        return ret;
    }
    
    chn_attr.width = SUB_WIDTH;
    chn_attr.height = SUB_HEIGHT;
    ret = ss_mpi_vpss_set_chn_attr(g_app.vpss_grp, g_app.vpss_chn_sub, &chn_attr);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_vpss_enable_chn(g_app.vpss_grp, g_app.vpss_chn_sub);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_vpss_start_grp(g_app.vpss_grp);
    if (ret != 0) {
        return ret;
    }
    
    return 0;
}

static int init_venc()
{
    g_app.venc_chn_main = 0;
    g_app.venc_chn_mid = 1;
    g_app.venc_chn_sub = 2;
    g_app.jpeg_chn = 3;
    
    ot_venc_chn_attr main_attr = {
        .venc_attr = {
            .type = OT_PT_H264,
            .max_pic_width = MAIN_WIDTH,
            .max_pic_height = MAIN_HEIGHT,
            .buf_size = MAIN_WIDTH * MAIN_HEIGHT * 2,
            .profile = 0,
            .is_by_frame = TD_FALSE,
            .pic_width = MAIN_WIDTH,
            .pic_height = MAIN_HEIGHT,
            .h264_attr = {
                .rcn_ref_share_buf_en = TD_FALSE,
                .frame_buf_ratio = 0
            }
        },
        .rc_attr = {
            .rc_mode = OT_VENC_RC_MODE_H264_CBR,
            .h264_cbr = {
                .bit_rate = MAIN_BITRATE,
                .src_frame_rate = MAIN_FPS,
                .dst_frame_rate = MAIN_FPS,
                .gop = MAIN_GOP
            }
        },
        .gop_attr = {
            .gop_mode = OT_VENC_GOP_MODE_NORMAL_P,
            .normal_p = {
                .ip_qp_delta = 0
            }
        }
    };
    
    int ret = ss_mpi_venc_create_chn(g_app.venc_chn_main, &main_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_venc_chn_attr mid_attr = {
        .venc_attr = {
            .type = OT_PT_H264,
            .max_pic_width = MID_WIDTH,
            .max_pic_height = MID_HEIGHT,
            .buf_size = MID_WIDTH * MID_HEIGHT * 2,
            .profile = 0,
            .is_by_frame = TD_FALSE,
            .pic_width = MID_WIDTH,
            .pic_height = MID_HEIGHT,
            .h264_attr = {
                .rcn_ref_share_buf_en = TD_FALSE,
                .frame_buf_ratio = 0
            }
        },
        .rc_attr = {
            .rc_mode = OT_VENC_RC_MODE_H264_CBR,
            .h264_cbr = {
                .bit_rate = MID_BITRATE,
                .src_frame_rate = MID_FPS,
                .dst_frame_rate = MID_FPS,
                .gop = MID_GOP
            }
        },
        .gop_attr = {
            .gop_mode = OT_VENC_GOP_MODE_NORMAL_P,
            .normal_p = {
                .ip_qp_delta = 0
            }
        }
    };
    
    ret = ss_mpi_venc_create_chn(g_app.venc_chn_mid, &mid_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_venc_chn_attr sub_attr = {
        .venc_attr = {
            .type = OT_PT_H264,
            .max_pic_width = SUB_WIDTH,
            .max_pic_height = SUB_HEIGHT,
            .buf_size = SUB_WIDTH * SUB_HEIGHT * 2,
            .profile = 0,
            .is_by_frame = TD_FALSE,
            .pic_width = SUB_WIDTH,
            .pic_height = SUB_HEIGHT,
            .h264_attr = {
                .rcn_ref_share_buf_en = TD_FALSE,
                .frame_buf_ratio = 0
            }
        },
        .rc_attr = {
            .rc_mode = OT_VENC_RC_MODE_H264_CBR,
            .h264_cbr = {
                .bit_rate = SUB_BITRATE,
                .src_frame_rate = SUB_FPS,
                .dst_frame_rate = SUB_FPS,
                .gop = SUB_GOP
            }
        },
        .gop_attr = {
            .gop_mode = OT_VENC_GOP_MODE_NORMAL_P,
            .normal_p = {
                .ip_qp_delta = 0
            }
        }
    };
    
    ret = ss_mpi_venc_create_chn(g_app.venc_chn_sub, &sub_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_venc_chn_attr jpeg_attr = {
        .venc_attr = {
            .type = OT_PT_JPEG,
            .max_pic_width = JPEG_WIDTH,
            .max_pic_height = JPEG_HEIGHT,
            .buf_size = JPEG_WIDTH * JPEG_HEIGHT * 2,
            .profile = 0,
            .is_by_frame = TD_FALSE,
            .pic_width = JPEG_WIDTH,
            .pic_height = JPEG_HEIGHT,
            .jpeg_attr = {
                .dcf_en = TD_FALSE,
                .mpf_cfg = {
                    .large_thumbnail_num = 0
                },
                .recv_mode = OT_VENC_PIC_RECV_SINGLE
            }
        },
        .rc_attr = {
            .rc_mode = OT_VENC_RC_MODE_MJPEG_FIXQP,
            .mjpeg_fixqp = {
                .src_frame_rate = MAIN_FPS,
                .dst_frame_rate = MAIN_FPS,
                .qfactor = 75
            }
        },
        .gop_attr = {
            .gop_mode = OT_VENC_GOP_MODE_NORMAL_P,
            .normal_p = {
                .ip_qp_delta = 0
            }
        }
    };
    
    ret = ss_mpi_venc_create_chn(g_app.jpeg_chn, &jpeg_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_venc_start_param start_param = {
        .recv_pic_num = 1
    };
    ret = ss_mpi_venc_start_chn(g_app.venc_chn_main, &start_param);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_venc_start_chn(g_app.venc_chn_mid, &start_param);
    if (ret != 0) {
        return ret;
    }
    
    ret = ss_mpi_venc_start_chn(g_app.venc_chn_sub, &start_param);
    if (ret != 0) {
        return ret;
    }
    
    return ss_mpi_venc_start_chn(g_app.jpeg_chn, &start_param);
}

static int init_osd()
{
    if (!g_app.osd_enabled) {
        return 0;
    }
    
    ot_rgn_attr time_attr = {
        .type = OT_RGN_OVERLAY,
        .attr = {
            .overlay = {
                .pixel_format = OT_PIXEL_FORMAT_ARGB_CLUT4,
                .bg_color = 0,
                .size = {
                    .width = 400,
                    .height = 30
                },
                .canvas_num = 1,
                .clut = {
                    0x000000,
                    0xFFFFFF,
                    0x808080,
                    0x800000,
                    0x008000,
                    0x808000,
                    0x000080,
                    0x800080,
                    0x008080,
                    0xC0C0C0,
                    0xFF0000,
                    0x00FF00,
                    0xFFFF00,
                    0x0000FF,
                    0xFF00FF,
                    0x00FFFF
                }
            }
        }
    };
    
    g_app.time_rgn = 0;
    int ret = ss_mpi_rgn_create(g_app.time_rgn, &time_attr);
    if (ret != 0) {
        return ret;
    }
    
    ot_rgn_attr cam_attr = {
        .type = OT_RGN_OVERLAY,
        .attr = {
            .overlay = {
                .pixel_format = OT_PIXEL_FORMAT_ARGB_CLUT4,
                .bg_color = 0,
                .size = {
                    .width = 200,
                    .height = 30
                },
                .canvas_num = 1,
                .clut = {
                    0x000000,
                    0xFFFFFF,
                    0x808080,
                    0x800000,
                    0x008000,
                    0x808000,
                    0x000080,
                    0x800080,
                    0x008080,
                    0xC0C0C0,
                    0xFF0000,
                    0x00FF00,
                    0xFFFF00,
                    0x0000FF,
                    0xFF00FF,
                    0x00FFFF
                }
            }
        }
    };
    
    g_app.cam_rgn = 1;
    ret = ss_mpi_rgn_create(g_app.cam_rgn, &cam_attr);
    if (ret != 0) {
        ss_mpi_rgn_destroy(g_app.time_rgn);
        g_app.time_rgn = 0;
        return ret;
    }
    
    return 0;
}

// Dummy implementation for missing ss_mpi_vpss_stop_chn function
static int ss_mpi_vpss_stop_chn(ot_vpss_grp grp, ot_vpss_chn chn)
{
    (void)grp;
    (void)chn;
    return 0;
}

static void app_deinit()
{
    if (g_app.osd_enabled) {
        ss_mpi_rgn_destroy(g_app.time_rgn);
        ss_mpi_rgn_destroy(g_app.cam_rgn);
    }
    
    ss_mpi_venc_stop_chn(g_app.jpeg_chn);
    ss_mpi_venc_destroy_chn(g_app.jpeg_chn);
    
    ss_mpi_venc_stop_chn(g_app.venc_chn_sub);
    ss_mpi_venc_destroy_chn(g_app.venc_chn_sub);
    
    ss_mpi_venc_stop_chn(g_app.venc_chn_mid);
    ss_mpi_venc_destroy_chn(g_app.venc_chn_mid);
    
    ss_mpi_venc_stop_chn(g_app.venc_chn_main);
    ss_mpi_venc_destroy_chn(g_app.venc_chn_main);
    
    ss_mpi_vpss_stop_chn(g_app.vpss_grp, g_app.vpss_chn_sub);
    ss_mpi_vpss_disable_chn(g_app.vpss_grp, g_app.vpss_chn_sub);
    
    ss_mpi_vpss_stop_chn(g_app.vpss_grp, g_app.vpss_chn_mid);
    ss_mpi_vpss_disable_chn(g_app.vpss_grp, g_app.vpss_chn_mid);
    
    ss_mpi_vpss_stop_chn(g_app.vpss_grp, g_app.vpss_chn_main);
    ss_mpi_vpss_disable_chn(g_app.vpss_grp, g_app.vpss_chn_main);
    
    ss_mpi_vpss_stop_grp(g_app.vpss_grp);
    ss_mpi_vpss_destroy_grp(g_app.vpss_grp);
    
    ss_mpi_vi_disable_chn(g_app.vi_pipe, g_app.vi_chn);
    ss_mpi_vi_stop_pipe(g_app.vi_pipe);
    ss_mpi_vi_destroy_pipe(g_app.vi_pipe);
    
    // Stop ISP
    printf("Stopping ISP...\n");
    ss_mpi_isp_exit(g_app.vi_pipe);
    
    if (g_app.latest_jpeg) {
        free(g_app.latest_jpeg);
    }
    
    pthread_mutex_destroy(&g_app.jpeg_mutex);
    
    printf("Exiting system...\n");
    int ret = ss_mpi_sys_exit();
    if (ret != 0) {
        printf("ss_mpi_sys_exit failed: %d\n", ret);
    }
    
    // Deinitialize VB
    printf("Deinitializing VB...\n");
    ss_mpi_vb_exit();
    printf("VB deinitialized\n");
}

static int init_vb()
{
    printf("Initializing VB (Video Buffer)...\n");
    
    // Define VB parameters for 8MP sensor
    ot_vb_cfg vb_cfg = {
        .max_pool_cnt = 3,
        .common_pool = {
            {
                .blk_size = MAIN_WIDTH * MAIN_HEIGHT * 1.5, // YUV 420
                .blk_cnt = 10,
                .remap_mode = OT_VB_REMAP_MODE_NONE,
                .mmz_name = ""
            },
            {
                .blk_size = MAIN_WIDTH * MAIN_HEIGHT * 2, // Bayer 12bpp
                .blk_cnt = 6,
                .remap_mode = OT_VB_REMAP_MODE_NONE,
                .mmz_name = ""
            },
            {
                .blk_size = 720 * 480 * 1.5, // Small YUV
                .blk_cnt = 3,
                .remap_mode = OT_VB_REMAP_MODE_NONE,
                .mmz_name = ""
            }
        }
    };
    
    // Set VB configuration
    int ret = ss_mpi_vb_set_cfg(&vb_cfg);
    if (ret != 0) {
        printf("ss_mpi_vb_set_cfg failed: %d\n", ret);
        return ret;
    }
    
    // Initialize VB
    ret = ss_mpi_vb_init();
    if (ret != 0) {
        printf("ss_mpi_vb_init failed: %d\n", ret);
        return ret;
    }
    
    printf("VB initialized successfully\n");
    return 0;
}



static int register_sensor_and_libs(ot_vi_pipe vi_pipe)
{
    int ret;
    ot_isp_3a_alg_lib ae_lib;
    ot_isp_3a_alg_lib awb_lib;
    ot_isp_sns_obj *sns_obj = &g_sns_imx415_obj;
    ot_isp_sns_commbus sns_bus_info;
    ot_isp_init_attr init_attr = {0};

    // 1. Bind Sensor Bus (I2C 0) first
    sns_bus_info.i2c_dev = 0; // Assuming I2C 0
    if (sns_obj->pfn_set_bus_info != NULL) {
        ret = sns_obj->pfn_set_bus_info(vi_pipe, sns_bus_info);
        if (ret != 0) {
             printf("Sensor set bus info failed: %#x\n", ret);
             return ret;
        }
    } else {
        printf("Sensor set bus info callback is NULL\n");
        return -1;
    }

    // 2. Set sensor init attributes
    if (sns_obj->pfn_set_init != NULL) {
        ret = sns_obj->pfn_set_init(vi_pipe, &init_attr);
        if (ret != 0) {
            printf("Sensor set init failed: %#x\n", ret);
            return ret;
        }
    }

    // 3. Register AE Lib
    ae_lib.id = vi_pipe;
    strncpy(ae_lib.lib_name, OT_AE_LIB_NAME, sizeof(ae_lib.lib_name));
    ret = ss_mpi_ae_register(vi_pipe, &ae_lib);
    if (ret != 0) {
         printf("ss_mpi_ae_register failed: %#x\n", ret);
         return ret;
    }

    // 4. Register AWB Lib
    awb_lib.id = vi_pipe;
    strncpy(awb_lib.lib_name, OT_AWB_LIB_NAME, sizeof(awb_lib.lib_name));
    ret = ss_mpi_awb_register(vi_pipe, &awb_lib);
    if (ret != 0) {
         printf("ss_mpi_awb_register failed: %#x\n", ret);
         return ret;
    }

    // 5. Register Sensor Callback
    if (sns_obj->pfn_register_callback != NULL) {
        ret = sns_obj->pfn_register_callback(vi_pipe, &ae_lib, &awb_lib);
        if (ret != 0) {
            printf("Sensor register callback failed: %#x\n", ret);
            return ret;
        }
    } else {
        printf("Sensor register callback is NULL\n");
        return -1;
    }
    
    return 0;
}

static int init_isp()
{
    printf("Initializing ISP...\n");
    
    // 1. Register Sensor and 3A Libs first
    int ret = register_sensor_and_libs(g_app.vi_pipe);
    if (ret != 0) {
        printf("register_sensor_and_libs failed: %d\n", ret);
        return ret;
    }

    // 2. Set ISP Public Attributes
    ot_isp_pub_attr pub_attr = {0};
    pub_attr.wnd_rect.x = 0;
    pub_attr.wnd_rect.y = 0;
    pub_attr.wnd_rect.width = MAIN_WIDTH;
    pub_attr.wnd_rect.height = MAIN_HEIGHT;
    pub_attr.sns_size.width = MAIN_WIDTH;
    pub_attr.sns_size.height = MAIN_HEIGHT;
    pub_attr.frame_rate = MAIN_FPS;
    pub_attr.bayer_format = OT_ISP_BAYER_GBRG; // IMX415 GBRG
    pub_attr.wdr_mode = OT_WDR_MODE_NONE;
    pub_attr.sns_mode = 0;

    ret = ss_mpi_isp_set_pub_attr(g_app.vi_pipe, &pub_attr);
    if (ret != 0) {
        printf("ss_mpi_isp_set_pub_attr failed: %d\n", ret);
        return ret;
    }

    // 3. ISP Mem Init
    ret = ss_mpi_isp_mem_init(g_app.vi_pipe);
    if (ret != 0) {
        printf("ss_mpi_isp_mem_init failed: %d\n", ret);
        return ret;
    }

    // 4. ISP Init
    ret = ss_mpi_isp_init(g_app.vi_pipe);
    if (ret != 0) {
        printf("ss_mpi_isp_init failed: %d\n", ret);
        return ret;
    }
    
    // 5. ISP Run
    ret = ss_mpi_isp_run(g_app.vi_pipe);
    if (ret != 0) {
        printf("ss_mpi_isp_run failed: %d\n", ret);
        ss_mpi_isp_exit(g_app.vi_pipe);
        return ret;
    }
    
    printf("ISP initialized successfully\n");
    return 0;
}

static int bind_modules()
{
    int ret;
    ot_mpp_chn src_chn, dst_chn;

    // 1. VI -> VPSS
    printf("Binding VI to VPSS...\n");
    src_chn.mod_id = OT_ID_VI;
    src_chn.dev_id = g_app.vi_pipe;
    src_chn.chn_id = g_app.vi_chn;
    
    dst_chn.mod_id = OT_ID_VPSS;
    dst_chn.dev_id = g_app.vpss_grp;
    dst_chn.chn_id = 0; // VPSS Group Input
    
    ret = ss_mpi_sys_bind(&src_chn, &dst_chn);
    if (ret != 0) {
        printf("Bind VI->VPSS failed: %d\n", ret);
        return ret;
    }

    // 2. VPSS -> VENC (Main)
    printf("Binding VPSS to VENC (Main)...\n");
    src_chn.mod_id = OT_ID_VPSS;
    src_chn.dev_id = g_app.vpss_grp;
    src_chn.chn_id = g_app.vpss_chn_main;
    
    dst_chn.mod_id = OT_ID_VENC;
    dst_chn.dev_id = 0;
    dst_chn.chn_id = g_app.venc_chn_main;
    
    ret = ss_mpi_sys_bind(&src_chn, &dst_chn);
    if (ret != 0) {
        printf("Bind VPSS->VENC(Main) failed: %d\n", ret);
        return ret;
    }

    // 3. VPSS -> VENC (Mid)
    printf("Binding VPSS to VENC (Mid)...\n");
    src_chn.mod_id = OT_ID_VPSS;
    src_chn.dev_id = g_app.vpss_grp;
    src_chn.chn_id = g_app.vpss_chn_mid;
    
    dst_chn.mod_id = OT_ID_VENC;
    dst_chn.dev_id = 0;
    dst_chn.chn_id = g_app.venc_chn_mid;
    
    ret = ss_mpi_sys_bind(&src_chn, &dst_chn);
    if (ret != 0) {
        printf("Bind VPSS->VENC(Mid) failed: %d\n", ret);
        return ret;
    }

    // 4. VPSS -> VENC (Sub)
    printf("Binding VPSS to VENC (Sub)...\n");
    src_chn.mod_id = OT_ID_VPSS;
    src_chn.dev_id = g_app.vpss_grp;
    src_chn.chn_id = g_app.vpss_chn_sub;
    
    dst_chn.mod_id = OT_ID_VENC;
    dst_chn.dev_id = 0;
    dst_chn.chn_id = g_app.venc_chn_sub;
    
    ret = ss_mpi_sys_bind(&src_chn, &dst_chn);
    if (ret != 0) {
        printf("Bind VPSS->VENC(Sub) failed: %d\n", ret);
        return ret;
    }

    // 5. VPSS -> VENC (JPEG) - Shares Main Channel (0)
    printf("Binding VPSS to VENC (JPEG)...\n");
    src_chn.mod_id = OT_ID_VPSS;
    src_chn.dev_id = g_app.vpss_grp;
    src_chn.chn_id = g_app.vpss_chn_main; 
    
    dst_chn.mod_id = OT_ID_VENC;
    dst_chn.dev_id = 0;
    dst_chn.chn_id = g_app.jpeg_chn;
    
    ret = ss_mpi_sys_bind(&src_chn, &dst_chn);
    if (ret != 0) {
        printf("Bind VPSS->VENC(JPEG) failed: %d\n", ret);
        return ret;
    }

    return 0;
}

static void print_error(const char* func, int ret)
{
    printf("[%s] Failed with error code: %d\n", func, ret);
}

int main()
{
    printf("Starting Hi3516CV610 WebRTC H.264 streaming server...\n");
    
    int ret = pthread_mutex_init(&g_app.jpeg_mutex, NULL);
    if (ret != 0) {
        printf("pthread_mutex_init failed: %d\n", ret);
        return -1;
    }
    
    // Initialize VB first
    ret = init_vb();
    if (ret != 0) {
        printf("VB initialization failed\n");
        goto error;
    }
    
    printf("Initializing system...\n");
    ret = ss_mpi_sys_init();
    if (ret != 0) {
        printf("ss_mpi_sys_init failed: %d\n", ret);
        goto error;
    }
    printf("System initialized successfully\n");
    
    // Set VI VPSS mode
    printf("Setting VI VPSS mode...\n");
    ot_vi_vpss_mode vi_vpss_mode = {
        .mode = {
            OT_VI_OFFLINE_VPSS_OFFLINE,
            OT_VI_OFFLINE_VPSS_OFFLINE,
            OT_VI_OFFLINE_VPSS_OFFLINE,
            OT_VI_OFFLINE_VPSS_OFFLINE
        }
    };
    ret = ss_mpi_sys_set_vi_vpss_mode(&vi_vpss_mode);
    if (ret != 0) {
        printf("ss_mpi_sys_set_vi_vpss_mode failed: %d\n", ret);
        goto error;
    }
    
    // Set AIISP mode
    ot_vi_aiisp_mode aiisp_mode = OT_VI_AIISP_MODE_DEFAULT;
    ret = ss_mpi_sys_set_vi_aiisp_mode(0, aiisp_mode);
    if (ret != 0) {
        printf("ss_mpi_sys_set_vi_aiisp_mode failed: %d\n", ret);
        goto error;
    }
    
    // Create VI pipe first without starting it
    printf("Creating VI pipe...\n");
    g_app.vi_pipe = 0;
    g_app.vi_chn = 0;
    
    ot_vi_pipe_attr pipe_attr = {
        .pipe_bypass_mode = OT_VI_PIPE_BYPASS_NONE,
        .isp_bypass = TD_FALSE,
        .size = {
            .width = MAIN_WIDTH,
            .height = MAIN_HEIGHT
        },
        .pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
        .compress_mode = OT_COMPRESS_MODE_NONE,
        .frame_rate_ctrl = {
            .src_frame_rate = 25,
            .dst_frame_rate = 25
        }
    };
    
    ret = ss_mpi_vi_create_pipe(g_app.vi_pipe, &pipe_attr);
    if (ret != 0) {
        printf("ss_mpi_vi_create_pipe failed: %d\n", ret);
        goto error;
    }
    printf("VI pipe created successfully\n");
    
    // Initialize ISP
    ret = init_isp();
    if (ret != 0) {
        printf("ISP initialization failed\n");
        goto error;
    }
    
    // Complete VI initialization
    printf("Completing VI initialization...\n");
    ot_vi_chn_attr chn_attr = {
        .size = {
            .width = MAIN_WIDTH,
            .height = MAIN_HEIGHT
        },
        .pixel_format = OT_PIXEL_FORMAT_YUV_SEMIPLANAR_420,
        .dynamic_range = OT_DYNAMIC_RANGE_SDR8,
        .video_format = OT_VIDEO_FORMAT_LINEAR,
        .compress_mode = OT_COMPRESS_MODE_NONE,
        .mirror_en = TD_FALSE,
        .flip_en = TD_FALSE,
        .depth = 4,
        .frame_rate_ctrl = {
            .src_frame_rate = 25,
            .dst_frame_rate = 25
        }
    };
    
    ret = ss_mpi_vi_set_chn_attr(g_app.vi_pipe, g_app.vi_chn, &chn_attr);
    if (ret != 0) {
        printf("ss_mpi_vi_set_chn_attr failed: %d\n", ret);
        ss_mpi_vi_destroy_pipe(g_app.vi_pipe);
        goto error;
    }
    
    ret = ss_mpi_vi_enable_chn(g_app.vi_pipe, g_app.vi_chn);
    if (ret != 0) {
        printf("ss_mpi_vi_enable_chn failed: %d\n", ret);
        ss_mpi_vi_destroy_pipe(g_app.vi_pipe);
        goto error;
    }
    
    ret = ss_mpi_vi_start_pipe(g_app.vi_pipe);
    if (ret != 0) {
        printf("ss_mpi_vi_start_pipe failed: %d\n", ret);
        ss_mpi_vi_destroy_pipe(g_app.vi_pipe);
        goto error;
    }
    printf("VI initialized successfully\n");
    
    printf("Initializing VPSS...\n");
    ret = init_vpss();
    if (ret != 0) {
        print_error("init_vpss", ret);
        goto error;
    }
    printf("VPSS initialized successfully\n");
    

    
    printf("Initializing VENC...\n");
    ret = init_venc();
    if (ret != 0) {
        print_error("init_venc", ret);
        goto error;
    }
    printf("VENC initialized successfully\n");
    
    printf("Initializing OSD...\n");
    ret = init_osd();
    if (ret != 0) {
        printf("OSD init failed, continuing without OSD\n");
        g_app.osd_enabled = 0;
    } else {
        printf("OSD initialized successfully\n");
    }
    
    printf("Modules initialized successfully\n");
    
    // Bind modules
    ret = bind_modules();
    if (ret != 0) {
        printf("Binding modules failed\n");
        goto error;
    }
    printf("Modules bound successfully\n");
    
    printf("Starting HTTP server thread...\n");
    pthread_t tid;
    ret = pthread_create(&tid, NULL, http_server_thread, NULL);
    if (ret != 0) {
        printf("pthread_create failed: %d\n", ret);
        goto error;
    }
    
    printf("Hi3516CV610 WebRTC H.264 streaming server started on port 8080\n");
    printf("Please access http://<device-ip>:8080 in your browser\n");
    
    while (1) {
        sleep(1);
    }
    
error:
    printf("Cleaning up resources...\n");
    app_deinit();
    printf("Server stopped with error\n");
    return -1;
}