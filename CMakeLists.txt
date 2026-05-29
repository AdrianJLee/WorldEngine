cmake_minimum_required(VERSION 3.16)

# 设置输出目录 (类似 Premake 的 outputdir)
set(OUTPUT_CONFIG "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

project(World VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 Vulkan SDK
find_package(Vulkan REQUIRED)


# --- 添加子项目---
add_subdirectory(World)

add_subdirectory(Editor)
add_subdirectory(Game)
target_compile_definitions(World PUBLIC
    WLD_EDITOR_DIR="${CMAKE_SOURCE_DIR}/Editor/"
    WLD_ASSETPATH="assets"
)