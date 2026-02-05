# ======================================================
# Air8000 MQTT Client Library 构建脚本
# ======================================================

# ------------------- 编译器设置 -------------------
CROSS_COMPILE := arm-v01c02-linux-musleabi-
CC := $(CROSS_COMPILE)gcc

# ------------------- mosquitto 配置 -------------------
# 静态库路径（重新编译后的静态库）
MOSQ_STATIC_LIB := ../../mosquitto-2.0.18/lib/libmosquitto.a
# 头文件路径
MOSQ_INCLUDE := ../../mosquitto-2.0.18/include

# ------------------- OpenSSL 配置 -------------------
# OpenSSL头文件路径（相对路径）
OPENSSL_INCLUDE := ../../openssl-3.0.13/cross-openssl/include
# OpenSSL库文件路径（相对路径）
OPENSSL_LIB := ../../openssl-3.0.13/cross-openssl/lib

# ------------------- cJSON 配置 -------------------
# cJSON头文件路径（相对路径）
CJSON_INCLUDE := ../../cJSON-1.7.15/install/include/cjson
# cJSON静态库路径（相对路径）
CJSON_STATIC_LIB := ../../cJSON-1.7.15/install/lib/libcjson.a

# ------------------- 编译选项 -------------------
CFLAGS = \
  -Wall -Wextra \
  -Iinclude \
  -I../process_manager/include \
  -I$(MOSQ_INCLUDE) \
  -I$(OPENSSL_INCLUDE) \
  -I$(CJSON_INCLUDE) \
  -g -O2 \
  -mcpu=cortex-a7

# ------------------- 链接选项 -------------------
LDFLAGS = \
  -static \
  $(MOSQ_STATIC_LIB) \
  $(CJSON_STATIC_LIB) \
  -L$(OPENSSL_LIB) \
  -lpthread \
  -lssl \
  -lcrypto

# ------------------- 源文件定义 -------------------
SRC_MQTT_CLIENT = src/mqtt_client.c src/fota_file_download.c src/mqtt_file_upload.c
SRC_PROCESS_MANAGER = ../process_manager/src/message_queue.c
OBJ_MQTT_CLIENT = $(SRC_MQTT_CLIENT:.c=.o) $(SRC_PROCESS_MANAGER:.c=.o)

EXAMPLE_CLIENT_SRC = examples/mqtt_client_example.c
EXAMPLE_CLIENT_OBJ = $(EXAMPLE_CLIENT_SRC:.c=.o)

TARGET_CLIENT = mqtt_client_test

# ------------------- 伪目标 -------------------
.PHONY: all clean

# ------------------- 构建目标 -------------------
all: $(TARGET_CLIENT)

$(TARGET_CLIENT): $(EXAMPLE_CLIENT_OBJ) $(OBJ_MQTT_CLIENT)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_MQTT_CLIENT) $(EXAMPLE_CLIENT_OBJ) $(TARGET_CLIENT)
