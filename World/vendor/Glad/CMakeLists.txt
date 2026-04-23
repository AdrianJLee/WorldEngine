cmake_minimum_required(VERSION 3.15)
project(Glad LANGUAGES C)

# --- 1. 设置输出目录 ---
# 模拟 Premake 的 bin/%{cfg.buildcfg} 结构
set(OUTPUT_DIR "bin/${CMAKE_BUILD_TYPE}")
set(OBJ_DIR "bin-int/${CMAKE_BUILD_TYPE}/Glad")

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${OUTPUT_DIR}/Glad")
# CMake 通常不手动指定 obj 目录，由生成器自动处理，但可以指定库输出路径

# --- 2. 静态运行时 (staticruntime "on") ---
# 现代 CMake (3.15+) 处理 MSVC 静态运行时的标准方式
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- 3. 定义项目 ---
add_library(Glad STATIC
    "include/glad/glad.h"
    "include/KHR/khrplatform.h"
    "src/glad.c"
)

# --- 4. 包含目录 ---
target_include_directories(Glad PUBLIC "include")

# --- 5. 系统特定设置 (Windows) ---
if(WIN32)
    # 对应 systemversion "latest"，CMake 默认使用最新 SDK
    # 如果需要明确指定，通常在命令行传入或设置 CMAKE_SYSTEM_VERSION
endif()

# --- 6. 编译配置 (Debug/Release) ---
# CMake 默认处理 symbols (Debug) 和 optimize (Release)
# 如果需要额外指定：
target_compile_options(Glad PRIVATE
    $<$<CONFIG:Debug>:/Zi> 
    $<$<CONFIG:Release>:/O2>
)