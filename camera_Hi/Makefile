# Cross-compilation toolchain
CROSS_COMPILE ?= arm-v01c02-linux-musleabi-
CC := $(CROSS_COMPILE)gcc
AR := $(CROSS_COMPILE)ar

# SDK paths
SDK_PATH := ../../Hi3516CV610_SDK_V1.0.2.0
REL_INC := $(SDK_PATH)/smp/a7_linux/source/out/include
REL_LIB := $(SDK_PATH)/smp/a7_linux/source/out/lib

# Include paths
INCLUDES := -I.
INCLUDES += -I$(REL_INC)
INCLUDES += -I$(SDK_PATH)/smp/a7_linux/source/mpp/sample/common
INCLUDES += -I../../hi3516_cv610_rust_sdk-main/hi3516_cv610_rust_sdk-main/sdk/include

# Compiler flags
CFLAGS := -Wall -O2 $(INCLUDES)
CFLAGS += -D$(shell uname -m | tr '[:lower:]' '[:upper:]')
CFLAGS += -D__LINUX__
CFLAGS += -D_CONFIG_OT_ISP_SUPPORT=y

# Sensor type definition
SENSOR0_TYPE ?= IMX415_MIPI_8M_25FPS_10BIT
SENSOR1_TYPE ?= IMX415_MIPI_8M_25FPS_10BIT

CFLAGS += -DSENSOR0_TYPE=$(SENSOR0_TYPE)
CFLAGS += -DSENSOR1_TYPE=$(SENSOR1_TYPE)
CFLAGS += -D$(SENSOR0_TYPE)_SELECT=y
CFLAGS += -D$(SENSOR1_TYPE)_SELECT=y

# 静态库文件
STATIC_LIBS := $(REL_LIB)/libextend_stats.a \
               $(REL_LIB)/libaiisp.a \
               $(REL_LIB)/libot_mpi_isp.a \
               $(REL_LIB)/libot_osal.a \
               $(REL_LIB)/libss_mpi_isp.a \
               $(REL_LIB)/libss_mpi_ae.a \
               $(REL_LIB)/libss_mpi_awb.a \
               $(REL_LIB)/libss_mpi.a \
               $(REL_LIB)/libss_mpi_sysbind.a \
               $(REL_LIB)/libss_mpi_sysmem.a \
               $(REL_LIB)/libsns_sc500ai.a \
               ../../hi3516_cv610_rust_sdk-main/hi3516_cv610_rust_sdk-main/sdk/lib_static/libsns_imx415.a \
               $(REL_LIB)/libsecurec.a \
               $(REL_LIB)/libdehaze.a \
               $(REL_LIB)/libdrc.a \
               $(REL_LIB)/libldci.a \
               $(REL_LIB)/libbnr.a \
               $(REL_LIB)/libcalcflicker.a \
               $(REL_LIB)/libir_auto.a \
               $(REL_LIB)/libacs.a \
               $(REL_LIB)/libsvp_acl.a \
               $(REL_LIB)/libprotobuf-c.a

# Library flags
LDFLAGS := -L$(REL_LIB)
LDFLAGS += -Wl,--start-group $(STATIC_LIBS) -Wl,--end-group
LDFLAGS += -lpthread -lm -ldl

# Source files
SRCS := camera_capture_hi3516.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_sys.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_vi.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_isp.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_venc.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_region.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_vpss.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/sample_comm_osd_drawline.c
SRCS += $(SDK_PATH)/smp/a7_linux/source/mpp/sample/common/loadbmp.c

# Object files
OBJS := $(SRCS:.c=.o)

# Target
TARGET := camera_capture_hi3516

# Rules
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f snap_*.jpg
	rm -f *.log

.PHONY: all clean