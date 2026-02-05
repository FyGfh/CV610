# WebRTC H.264 视频推流 Makefile
# 适配 Hi3516CV610 目标板
# 单文件实现，直接使用 C SDK MPI API

# ==================== 编译器设置 ====================
CROSS_COMPILE := arm-v01c02-linux-musleabi-

CC  := $(CROSS_COMPILE)gcc
LD  := $(CROSS_COMPILE)gcc
AR  := $(CROSS_COMPILE)ar

# ==================== 目标可执行文件 ====================
TARGET = webrtc_h264_stream

# ==================== 目录设置 ====================
SRC_DIR = .
INCLUDE_DIR = .
HTML_DIR = html

# ==================== SDK 路径设置 ====================
SDK_PATH = ../../Hi3516CV610_SDK_V1.0.2.0

SDK_INCLUDE = $(SDK_PATH)/smp/a7_linux/source/out/include
SDK_CBB_INCLUDE = $(SDK_PATH)/smp/a7_linux/source/mpp/cbb
SDK_PLATFORM_INCLUDE = $(SDK_PATH)/platform/securec/include

# 验证SDK路径是否存在
$(if $(wildcard $(SDK_INCLUDE)/ot_type.h),,$(error SDK路径错误: 找不到 $(SDK_INCLUDE)/ot_type.h))
$(if $(wildcard $(SDK_PLATFORM_INCLUDE)/securec.h),,$(error SDK路径错误: 找不到 $(SDK_PLATFORM_INCLUDE)/securec.h))

# ==================== 源文件 ====================
# 单文件实现，无需其他源文件
SRCS = $(SRC_DIR)/main.c

# ==================== 头文件路径 ====================
INCS = -I$(INCLUDE_DIR) \
       -I$(SDK_INCLUDE) \
       -I$(SDK_CBB_INCLUDE) \
       -I$(SDK_PLATFORM_INCLUDE)

# ==================== 编译选项 ====================
ENABLE_GSTREAMER ?= 0

CFLAGS = -Wall -Wextra -g -O2 \
         $(INCS) \
         -fPIC \
         -DSENSOR0_TYPE=IMX415_MIPI_8M_25FPS_10BIT \
         -DSENSOR1_TYPE=IMX415_MIPI_8M_25FPS_10BIT \
         -DIMX415_MIPI_8M_25FPS_10BIT_SELECT=y

ifeq ($(ENABLE_GSTREAMER),1)
    CFLAGS += -DENABLE_GSTREAMER
endif

# ==================== 链接选项 ====================
LDFLAGS = -static -lpthread -lm

SDK_LIBS = -L$(SDK_PATH)/smp/a7_linux/source/out/lib \
           -lss_mpi \
           -lot_osal \
           -lss_mpi_sysbind \
           -lss_mpi_sysmem \
           -lsecurec \
           -lss_mpi_isp \
           -lss_mpi_ae \
           -lss_mpi_awb \
           -lot_mpi_isp \
           -lextend_stats \
           -laiisp \
           -lcalcflicker \
           -lir_auto \
           -ldehaze \
           -lldci \
           -lacs \
           -lbnr \
           -ldrc \
           -ldnvqe \
           -lsns_imx415 \
           -lss_mpi

ifeq ($(ENABLE_GSTREAMER),1)
    LDFLAGS += -lgstreamer-1.0 \
               -lgstwebrtc-1.0 \
               -lgstsdp-1.0 \
               -lgstbase-1.0 \
               -lglib-2.0 \
               -lgobject-2.0
endif

# ==================== 目标文件 ====================
OBJS = $(SRCS:.c=.o)

# ==================== 默认目标 ====================
all: $(TARGET)

# ==================== 编译规则 ====================
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(SDK_LIBS)
	@echo "编译完成: $@"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# ==================== 清理目标 ====================
clean:
	rm -f $(OBJS) $(TARGET) *.o

# ==================== 安装目标 ====================
install: $(TARGET)
	@echo "安装到目标板 /usr/bin/"
	cp $(TARGET) /usr/bin/
	@echo "复制 HTML 文件到 /usr/www/webrtc/"
	mkdir -p /usr/www/webrtc
	cp -r $(HTML_DIR)/* /usr/www/webrtc/

# ==================== 显示帮助 ====================
help:
	@echo "WebRTC H.264 视频推流 Makefile (Hi3516CV610)"
	@echo ""
	@echo "使用方法:"
	@echo "  make                    - 编译目标 (默认不使用 GStreamer)"
	@echo "  make ENABLE_GSTREAMER=1 - 编译目标 (启用 GStreamer 支持)"
	@echo "  make clean              - 清理目标文件"
	@echo "  make install            - 安装到目标板"
	@echo "  make help               - 显示帮助"
	@echo ""
	@echo "特性:"
	@echo "  - 单文件实现 (main.c)，无需模拟函数"
	@echo "  - 直接调用 C SDK MPI API"
	@echo "  - 支持多码流 (4K/1080p/480p)"
	@echo "  - 内置 HTTP 服务器"
	@echo "  - JPEG 快照支持"
	@echo "  - OSD 水印支持"

.PHONY: all clean install help
