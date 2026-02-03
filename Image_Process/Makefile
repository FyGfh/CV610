# ======================================================
# Air8000 Image Process C++ SDK 构建脚本
# ======================================================

# ------------------- 编译器设置 -------------------
CROSS_COMPILE := arm-v01c02-linux-musleabi-

CC  := $(CROSS_COMPILE)gcc
CXX := $(CROSS_COMPILE)g++

# OpenCV 安装目录
OPENCV_DIR := ../../opencv-4.5.5/arm_v01c02_softfp_static_install


# ------------------- 编译选项 -------------------
CFLAGS = -Wall -Wextra -Iinclude -g -O0

CXXFLAGS = -Wall -Wextra \
           -Iinclude \
           -I$(OPENCV_DIR)/include/opencv4 \
           -g -O0 -fPIC \
           -DOPENCV_DISABLE_ITT=1 \
           -DOPENCV_NO_ITT=1 \
           -DOPENCV_NO_GSTREAMER=1 \
           -D_GLIBCXX_USE_CXX11_ABI=0


# ------------------- 链接选项 -------------------
LDFLAGS = -lpthread \
          -lm \
          -ldl \
          -L$(OPENCV_DIR)/lib \
          -L$(OPENCV_DIR)/lib/opencv4/3rdparty \
          -lopencv_calib3d \
          -lopencv_highgui \
          -lopencv_imgcodecs \
          -lopencv_imgproc \
          -lopencv_core \
          -l:libittnotify.a \
          -l:libzlib.a \
          -l:liblibjpeg-turbo.a \
          -l:liblibpng.a \
          -l:liblibwebp.a \
          -l:liblibopenjp2.a


# ------------------- 源文件定义 -------------------
# C++ 源文件列表
CXX_SRC = src/image_processor.cpp main.cpp

# 将C++源文件列表转换为目标文件列表 (.cpp 替换为 .o)
CXX_OBJ = $(CXX_SRC:.cpp=.o)

# 合并所有目标文件
ALL_OBJ = $(CXX_OBJ)

# 最终可执行文件名
TARGET = image_processor

# ------------------- 伪目标定义 -------------------
# 伪目标 (不生成实际文件，用于定义命令集合)
.PHONY: all clean

# ------------------- 构建目标 -------------------
# 默认目标：构建所有内容
all: $(TARGET)

# 链接目标文件生成可执行文件
# $@ 表示目标文件名 (即 $(TARGET))
# $^ 表示所有依赖文件 (即 $(ALL_OBJ))
$(TARGET): $(ALL_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则：将任意 .cpp 文件编译为对应的 .o 文件
# $< 表示第一个依赖文件 (即源 .cpp 文件)
# $@ 表示目标文件名 (即 .o 文件)
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 清理构建产物
# 删除所有目标文件、可执行文件和 Windows 可执行文件
clean: 
	rm -f src/*.o *.o $(TARGET) $(TARGET).exe
