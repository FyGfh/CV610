# 海思CV610平台图像处理程序编译配置
# 适用于海思CV610 Linux芯片平台

cmake_minimum_required(VERSION 3.10)

# 项目名称和版本
project(image_processor VERSION 1.0.0 LANGUAGES CXX)

# 设置C++标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 设置编译选项
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2 -fPIC")

# 包含目录
include_directories(
    ${PROJECT_SOURCE_DIR}/include
)

# 查找OpenCV库
find_package(OpenCV REQUIRED)
if(OpenCV_FOUND)
    include_directories(${OpenCV_INCLUDE_DIRS})
    message(STATUS "Found OpenCV: ${OpenCV_VERSION}")
    message(STATUS "OpenCV include dirs: ${OpenCV_INCLUDE_DIRS}")
    message(STATUS "OpenCV libraries: ${OpenCV_LIBS}")
else()
    message(FATAL_ERROR "OpenCV not found")
endif()

# 源文件列表
set(SOURCES
    ${PROJECT_SOURCE_DIR}/src/image_processor.cpp
    ${PROJECT_SOURCE_DIR}/src/calibrator.cpp
    ${PROJECT_SOURCE_DIR}/main.cpp
)

# 生成可执行文件
add_executable(${PROJECT_NAME} ${SOURCES})

# 链接OpenCV库
target_link_libraries(${PROJECT_NAME} ${OpenCV_LIBS})

# 安装配置
install(TARGETS ${PROJECT_NAME} DESTINATION bin)

# 打印编译信息
message(STATUS "Project: ${PROJECT_NAME}")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Source dir: ${PROJECT_SOURCE_DIR}")
message(STATUS "Build dir: ${PROJECT_BINARY_DIR}")
message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C++ flags: ${CMAKE_CXX_FLAGS}")
