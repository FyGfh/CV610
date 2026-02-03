# Air8000 Process Manager Makefile

# 编译器设置
CROSS_COMPILE := arm-v01c02-linux-musleabi-

CC  := $(CROSS_COMPILE)gcc

CFLAGS = -Wall -Wextra -Werror -Iinclude -I../UART/include
LDFLAGS = -static -lpthread

# 目标文件
TARGET = process_manager

# 源文件
SRCS = src/main.c src/process_manager.c src/shared_memory.c src/message_queue.c

# 目标文件
OBJS = $(SRCS:.c=.o)

# 默认目标
all: $(TARGET)

# 编译目标
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

# 编译源文件
src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理目标
clean:
	rm -f $(OBJS) $(TARGET)

# 安装目标
install:
	cp $(TARGET) /usr/local/bin/

# 卸载目标
uninstall:
	rm -f /usr/local/bin/$(TARGET)

.PHONY: all clean install uninstall
