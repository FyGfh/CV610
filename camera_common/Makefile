# 摄像头拍照程序编译脚本

# 编译器设置
CROSS_COMPILE := arm-v01c02-linux-musleabi-
CC  := $(CROSS_COMPILE)gcc

# 编译选项
CFLAGS = -Wall -Wextra -g -O0

# 链接选项
LDFLAGS = -lpthread -lm -ldl

# 源文件
SRC = camera_capture.c

# 目标文件
TARGET = camera_capture

# 默认目标
all: $(TARGET)

# 编译规则
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 清理
clean:
	rm -f $(TARGET)
