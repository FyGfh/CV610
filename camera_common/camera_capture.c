#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#define CAMERA_DEVICE "/dev/video0"
#define DEFAULT_WIDTH  1920
#define DEFAULT_HEIGHT 1080
#define DEFAULT_FORMAT V4L2_PIX_FMT_MJPEG
#define BUFFER_COUNT   4

typedef struct {
    void *start;
    size_t length;
} buffer_t;

typedef struct {
    int fd;
    buffer_t *buffers;
    int buffer_count;
    int width;
    int height;
    int format;
} camera_t;

/* 打开摄像头设备 */
int camera_open(camera_t *camera, const char *device)
{
    camera->fd = open(device, O_RDWR);
    if (camera->fd < 0) {
        perror("Failed to open camera device");
        return -1;
    }
    return 0;
}

/* 设置摄像头格式和分辨率 */
int camera_set_format(camera_t *camera, int width, int height, int format)
{
    struct v4l2_format fmt = {0};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = format;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(camera->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("Failed to set camera format");
        return -1;
    }

    camera->width = fmt.fmt.pix.width;
    camera->height = fmt.fmt.pix.height;
    camera->format = fmt.fmt.pix.pixelformat;

    printf("Camera format set: %dx%d, format: %c%c%c%c\n",
           camera->width, camera->height,
           (format & 0xff), (format >> 8 & 0xff),
           (format >> 16 & 0xff), (format >> 24 & 0xff));

    return 0;
}

/* 分配和映射视频缓冲区 */
int camera_alloc_buffers(camera_t *camera, int count)
{
    struct v4l2_requestbuffers req = {0};
    req.count = count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(camera->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("Failed to request buffers");
        return -1;
    }

    camera->buffer_count = req.count;
    camera->buffers = calloc(req.count, sizeof(buffer_t));
    if (!camera->buffers) {
        perror("Failed to allocate buffer memory");
        return -1;
    }

    for (int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {0};
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(camera->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("Failed to query buffer");
            free(camera->buffers);
            return -1;
        }

        camera->buffers[i].length = buf.length;
        camera->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, camera->fd, buf.m.offset);
        if (camera->buffers[i].start == MAP_FAILED) {
            perror("Failed to map buffer");
            free(camera->buffers);
            return -1;
        }

        printf("Buffer %d mapped: %p, length: %zu\n", i, camera->buffers[i].start, camera->buffers[i].length);
    }

    return 0;
}

/* 开始视频流 */
int camera_start_stream(camera_t *camera)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("Failed to start stream");
        return -1;
    }

    /* 将所有缓冲区加入队列 */
    for (int i = 0; i < camera->buffer_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.index = i;
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (ioctl(camera->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("Failed to queue buffer");
            return -1;
        }
    }

    return 0;
}

/* 停止视频流 */
int camera_stop_stream(camera_t *camera)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(camera->fd, VIDIOC_STREAMOFF, &type) < 0) {
        perror("Failed to stop stream");
        return -1;
    }

    return 0;
}

/* 捕获一帧图像 */
int camera_capture_frame(camera_t *camera, void **data, size_t *size)
{
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(camera->fd, VIDIOC_DQBUF, &buf) < 0) {
        perror("Failed to dequeue buffer");
        return -1;
    }

    *data = camera->buffers[buf.index].start;
    *size = buf.bytesused;

    return buf.index;
}

/* 将缓冲区重新加入队列 */
int camera_queue_buffer(camera_t *camera, int index)
{
    struct v4l2_buffer buf = {0};
    buf.index = index;
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (ioctl(camera->fd, VIDIOC_QBUF, &buf) < 0) {
        perror("Failed to queue buffer");
        return -1;
    }

    return 0;
}

/* 保存图像到文件 */
int save_image(void *data, size_t size, const char *filename)
{
    FILE *file = fopen(filename, "wb");
    if (!file) {
        perror("Failed to open output file");
        return -1;
    }

    size_t written = fwrite(data, 1, size, file);
    if (written != size) {
        perror("Failed to write to file");
        fclose(file);
        return -1;
    }

    fclose(file);
    printf("Image saved to %s, size: %zu bytes\n", filename, size);

    return 0;
}

/* 释放资源 */
void camera_cleanup(camera_t *camera)
{
    if (camera->buffers) {
        for (int i = 0; i < camera->buffer_count; i++) {
            if (camera->buffers[i].start) {
                munmap(camera->buffers[i].start, camera->buffers[i].length);
            }
        }
        free(camera->buffers);
    }

    if (camera->fd >= 0) {
        close(camera->fd);
    }
}

/* 检查设备是否支持特定控制 */
int camera_check_control(int fd, unsigned int control_id)
{
    struct v4l2_queryctrl qctrl = {0};
    qctrl.id = control_id;

    if (ioctl(fd, VIDIOC_QUERYCTRL, &qctrl) < 0) {
        return -1; /* 不支持此控制 */
    }

    return 0; /* 支持此控制 */
}

/* 设置控制参数 */
int camera_set_control(int fd, unsigned int control_id, int value)
{
    struct v4l2_control ctrl = {0};
    ctrl.id = control_id;
    ctrl.value = value;

    if (ioctl(fd, VIDIOC_S_CTRL, &ctrl) < 0) {
        perror("Failed to set control");
        return -1;
    }

    return 0;
}

/* 获取当前控制参数值 */
int camera_get_control(int fd, unsigned int control_id, int *value)
{
    struct v4l2_control ctrl = {0};
    ctrl.id = control_id;

    if (ioctl(fd, VIDIOC_G_CTRL, &ctrl) < 0) {
        perror("Failed to get control");
        return -1;
    }

    *value = ctrl.value;
    return 0;
}

/* 启用自动白平衡 */
int camera_enable_awb(int fd)
{
    if (camera_check_control(fd, V4L2_CID_AUTO_WHITE_BALANCE) < 0) {
        printf("Auto white balance not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_AUTO_WHITE_BALANCE, 1);
}

/* 禁用自动白平衡 */
int camera_disable_awb(int fd)
{
    if (camera_check_control(fd, V4L2_CID_AUTO_WHITE_BALANCE) < 0) {
        printf("Auto white balance not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_AUTO_WHITE_BALANCE, 0);
}

/* 设置白平衡模式 */
int camera_set_awb_mode(int fd, int mode)
{
    if (camera_check_control(fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE) < 0) {
        printf("White balance mode not supported\n");
        return -1;
    }

    /* 先禁用自动白平衡 */
    if (camera_set_control(fd, V4L2_CID_AUTO_WHITE_BALANCE, 0) < 0) {
        return -1;
    }

    /* 设置手动白平衡温度 */
    return camera_set_control(fd, V4L2_CID_WHITE_BALANCE_TEMPERATURE, mode);
}

/* 启用自动曝光 */
int camera_enable_ae(int fd)
{
    if (camera_check_control(fd, V4L2_CID_AUTO_EXPOSURE) < 0) {
        printf("Auto exposure not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_AUTO_EXPOSURE, V4L2_EXPOSURE_AUTO);
}

/* 禁用自动曝光 */
int camera_disable_ae(int fd)
{
    if (camera_check_control(fd, V4L2_CID_AUTO_EXPOSURE) < 0) {
        printf("Auto exposure not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_AUTO_EXPOSURE, V4L2_EXPOSURE_MANUAL);
}

/* 设置曝光模式 */
int camera_set_exposure_mode(int fd, int mode)
{
    if (camera_check_control(fd, V4L2_CID_AUTO_EXPOSURE) < 0) {
        printf("Exposure mode not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_AUTO_EXPOSURE, mode);
}

/* 手动设置曝光值 */
int camera_set_exposure(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_EXPOSURE) < 0) {
        printf("Manual exposure not supported\n");
        return -1;
    }

    /* 先禁用自动曝光 */
    if (camera_set_control(fd, V4L2_CID_AUTO_EXPOSURE, V4L2_EXPOSURE_MANUAL) < 0) {
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_EXPOSURE, value);
}

/* 设置亮度 */
int camera_set_brightness(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_BRIGHTNESS) < 0) {
        printf("Brightness control not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_BRIGHTNESS, value);
}

/* 设置对比度 */
int camera_set_contrast(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_CONTRAST) < 0) {
        printf("Contrast control not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_CONTRAST, value);
}

/* 设置饱和度 */
int camera_set_saturation(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_SATURATION) < 0) {
        printf("Saturation control not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_SATURATION, value);
}

/* 设置色调 */
int camera_set_hue(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_HUE) < 0) {
        printf("Hue control not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_HUE, value);
}

/* 设置增益 */
int camera_set_gain(int fd, int value)
{
    if (camera_check_control(fd, V4L2_CID_GAIN) < 0) {
        printf("Gain control not supported\n");
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_GAIN, value);
}

/* 检查自动对焦支持 */
int camera_check_af_support(int fd)
{
    if (camera_check_control(fd, V4L2_CID_FOCUS_AUTO) < 0) {
        printf("Auto focus not supported\n");
        return -1;
    }

    return 0;
}

/* 启用自动对焦 */
int camera_enable_af(int fd)
{
    if (camera_check_af_support(fd) < 0) {
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_FOCUS_AUTO, 1);
}

/* 禁用自动对焦 */
int camera_disable_af(int fd)
{
    if (camera_check_af_support(fd) < 0) {
        return -1;
    }

    return camera_set_control(fd, V4L2_CID_FOCUS_AUTO, 0);
}

/* 触发自动对焦 */
int camera_trigger_af(int fd)
{
    if (camera_check_af_support(fd) < 0) {
        return -1;
    }

    /* 确保自动对焦已启用 */
    if (camera_set_control(fd, V4L2_CID_FOCUS_AUTO, 1) < 0) {
        return -1;
    }

    /* 对于某些设备，可能需要使用专用的触发控制 */
    if (camera_check_control(fd, V4L2_CID_FOCUS_ABSOLUTE) >= 0) {
        /* 对于支持绝对对焦的设备，我们可以先启用自动对焦 */
        printf("Auto focus triggered\n");
        return 0;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    camera_t camera = {0};
    int ret = 0;
    char *output_file = "capture.jpg";
    int enable_awb = 1; /* 默认启用自动白平衡 */
    int enable_ae = 1;  /* 默认启用自动曝光 */
    int enable_af = 1;  /* 默认启用自动对焦 */
    
    /* 检查命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--no-awb") == 0) {
            enable_awb = 0;
        } else if (strcmp(argv[i], "--no-ae") == 0) {
            enable_ae = 0;
        } else if (strcmp(argv[i], "--no-af") == 0) {
            enable_af = 0;
        }
    }

    printf("Camera capture program starting...\n");

    /* 1. 打开摄像头设备 */
    if (camera_open(&camera, CAMERA_DEVICE) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 2. 设置摄像头格式 */
    if (camera_set_format(&camera, DEFAULT_WIDTH, DEFAULT_HEIGHT, DEFAULT_FORMAT) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 3. 应用相机设置 */
    printf("Applying camera settings...\n");

    /* 3.1 自动白平衡 */
    if (enable_awb) {
        if (camera_enable_awb(camera.fd) == 0) {
            printf("Auto white balance enabled\n");
        }
    }

    /* 3.2 自动曝光 */
    if (enable_ae) {
        if (camera_enable_ae(camera.fd) == 0) {
            printf("Auto exposure enabled\n");
        }
    }

    /* 3.3 自动对焦 */
    if (enable_af) {
        if (camera_enable_af(camera.fd) == 0) {
            printf("Auto focus enabled\n");
            /* 触发一次自动对焦 */
            camera_trigger_af(camera.fd);
            /* 给对焦一些时间 */
            usleep(1000000); /* 1秒 */
        }
    }

    /* 4. 分配缓冲区 */
    if (camera_alloc_buffers(&camera, BUFFER_COUNT) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 5. 开始视频流 */
    if (camera_start_stream(&camera) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 6. 捕获一帧图像 */
    void *data;
    size_t size;
    int buffer_index = camera_capture_frame(&camera, &data, &size);
    if (buffer_index < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 7. 保存图像 */
    if (save_image(data, size, output_file) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 8. 将缓冲区重新加入队列 */
    if (camera_queue_buffer(&camera, buffer_index) < 0) {
        ret = -1;
        goto cleanup;
    }

    /* 9. 停止视频流 */
    if (camera_stop_stream(&camera) < 0) {
        ret = -1;
        goto cleanup;
    }

    printf("Camera capture completed successfully!\n");

cleanup:
    camera_cleanup(&camera);
    return ret;
}
