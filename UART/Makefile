# ======================================================
# Air8000 C SDK 构建脚本
# ======================================================

# ------------------- 编译器设置 -------------------
CROSS_COMPILE := arm-v01c02-linux-musleabi-

CC  := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++

OPENCV_DIR := ../arm_v01c02_softfp_static_install


# ------------------- 编译选项 -------------------
#CFLAGS = -Wall -Wextra -Iinclude -I../process_manager/include -g -O0 -DOPENCV_DISABLE_ITT=1 -DOPENCV_NO_ITT=1 -DOPENCV_NO_GSTREAMER=1
CFLAGS = -Wall -Wextra -Iinclude -I../process_manager/include -g -O0

# 暂时注释掉C++编译选项，因为不再需要编译C++代码
# CXXFLAGS = -Wall -Wextra \
#            -Iinclude \
#            -I../process_manager/include \
#            -I$(OPENCV_DIR)/include/opencv4 \
#            -g -O0 -fPIC \
#            -DOPENCV_DISABLE_ITT=1 \
#            -DOPENCV_NO_ITT=1 \
#            -DOPENCV_NO_GSTREAMER=1 \
#            -D_GLIBCXX_USE_CXX11_ABI=0


# ------------------- 链接选项 -------------------
# 暂时简化链接选项，移除所有与OPENCV和C++相关的链接选项
LDFLAGS = -lpthread \
          -lm


# ------------------- 源文件定义 -------------------
# SDK 核心源文件列表
SRC = src/air8000_protocol.c src/air8000_serial.c src/air8000.c src/air8000_file_transfer.c src/air8000_fota.c
# 暂时注释掉图像处理相关的源文件
# src/air8000_image_process.c
# process_manager 源文件
PROCESS_MANAGER_SRC = ../process_manager/src/message_queue.c
# 暂时注释掉C++源文件，因为不再需要编译C++代码
# C++ 源文件列表 (图像处理)
# CXX_SRC = src/image_processor.cpp
# 将C源文件列表转换为目标文件列表 (.c 替换为 .o)
OBJ = $(SRC:.c=.o) $(PROCESS_MANAGER_SRC:.c=.o)
# 暂时注释掉C++目标文件的转换，因为不再需要编译C++代码
# 将C++源文件列表转换为目标文件列表 (.cpp 替换为 .o)
# CXX_OBJ = $(CXX_SRC:.cpp=.o)
# 合并所有目标文件
# ALL_OBJ = $(OBJ) $(CXX_OBJ)
ALL_OBJ = $(OBJ)

# 示例程序源文件
EXAMPLE_SRC = examples/main.c
# 示例程序目标文件
EXAMPLE_OBJ = $(EXAMPLE_SRC:.c=.o)

# 最终可执行文件名
TARGET = air8000_test

# ------------------- 伪目标定义 -------------------
# 伪目标 (不生成实际文件，用于定义命令集合)
.PHONY: all clean

# ------------------- 构建目标 -------------------
# 默认目标：构建所有内容
all: $(TARGET)

# 链接目标文件生成可执行文件
# $@ 表示目标文件名 (即 $(TARGET))
# $^ 表示所有依赖文件 (即 $(ALL_OBJ) $(EXAMPLE_OBJ))
$(TARGET): $(ALL_OBJ) $(EXAMPLE_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则：将任意 .c 文件编译为对应的 .o 文件
# $< 表示第一个依赖文件 (即源 .c 文件)
# $@ 表示目标文件名 (即 .o 文件)
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# 暂时注释掉C++编译规则，因为不再需要编译C++代码
# 编译规则：将任意 .cpp 文件编译为对应的 .o 文件
# %.o: %.cpp
#	$(CXX) $(CXXFLAGS) -c $< -o $@

# 清理构建产物
# 删除所有目标文件、可执行文件和 Windows 可执行文件
clean: 
	rm -f src/*.o examples/*.o $(TARGET) $(TARGET).exe