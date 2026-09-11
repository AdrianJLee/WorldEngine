cmake_minimum_required(VERSION 3.16)

# 设置输出目录 (类似 Premake 的 outputdir)
set(OUTPUT_CONFIG "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

project(World VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 Vulkan SDK
find_package(Vulkan REQUIRED)


# --- 添加子项目---
add_subdirectory(World)

add_subdirectory(Editor)
add_subdirectory(Game)
add_subdirectory(Runtime)

add_dependencies(Runtime Game)
add_dependencies(Editor Runtime)


target_compile_definitions(World PUBLIC
    WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
    WLD_EDITOR_DIR="${CMAKE_SOURCE_DIR}/Editor/"
    WLD_GAME_DIR="${CMAKE_SOURCE_DIR}/Game/"
    WLD_ASSETPATH="${CMAKE_SOURCE_DIR}/Game/assets"
    WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    WLD_WORLD_DIR="${CMAKE_SOURCE_DIR}/World/"
)

option(WORLD_BUILD_SCRIPT_TESTS "Build WorldEngine script lifecycle checks" OFF)
if(WORLD_BUILD_SCRIPT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
