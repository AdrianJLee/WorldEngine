cmake_minimum_required(VERSION 3.16)

# 设置输出目录 (类似 Premake 的 outputdir)
set(OUTPUT_CONFIG "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

project(World VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 Vulkan SDK(可选):headers+volk 已 vendor,SDK 仅用于可选验证层
find_package(Vulkan QUIET)

# --- Slang(Slang-T5 起是**唯一**的着色器编译器;Slang-T6b 删掉了旧的两套外部工具)---
# 工具不入库("重物不入库"):由 tools/agents/fetch-slang.ps1 按固定版本 + sha256 下载到
# 仓库外的依赖根(默认 <repo 同级>/WorldEngine-deps/slang-<版本>)。
# WLD_SLANG_DIR = **含 slangc.exe 的目录**,是"去哪儿找编译器"的唯一事实源:
# 运行时代码只读这个编译期常量,不再有 env / vendor / PATH 兜底猜测。
if(NOT DEFINED WLD_SLANG_DIR)
    if(DEFINED ENV{WLD_SLANG_DIR} AND NOT "$ENV{WLD_SLANG_DIR}" STREQUAL "")
        set(WLD_SLANG_DIR "$ENV{WLD_SLANG_DIR}")
    else()
        set(WLD_SLANG_DIR "${CMAKE_SOURCE_DIR}/../WorldEngine-deps/slang-2026.18.2/bin")
    endif()
endif()
file(TO_CMAKE_PATH "${WLD_SLANG_DIR}" WLD_SLANG_DIR)
message(STATUS "WorldEngine Slang tool dir: ${WLD_SLANG_DIR}")
if(NOT EXISTS "${WLD_SLANG_DIR}/slangc.exe")
    message(WARNING "slangc.exe not found in ${WLD_SLANG_DIR}; dev-time shader compilation will "
        "fail (packaged builds use baked artifacts). Run tools/agents/fetch-slang.ps1, or "
        "configure with -DWLD_SLANG_DIR=<dir containing slangc.exe>.")
endif()

# --- 添加子项目---
add_subdirectory(Engine)

add_subdirectory(Editor)
add_subdirectory(Game)
add_subdirectory(Runtime)

add_dependencies(Runtime Game)
add_dependencies(Editor Runtime)


target_compile_definitions(World PUBLIC
    WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
    # 开发期绝对锚点(不带尾分隔符):供"与进程 CWD 无关"的路径拼接(如定位 Game.dll)。
    WLD_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    WLD_EDITOR_DIR="${CMAKE_SOURCE_DIR}/Editor/"
    WLD_LOCAL_DIR="${CMAKE_SOURCE_DIR}/local/"
    # WLD_GAME_DIR = Game **模块**目录(只放 gameplay DLL 源码);项目内容根见 WLD_PROJECT_DIR。
    WLD_GAME_DIR="${CMAKE_SOURCE_DIR}/Game/"
    # Layout-S7:项目内容(清单/关卡表/assets)在 projects/<名>/;默认项目 = projects/default/。
    WLD_PROJECT_DIR="${CMAKE_SOURCE_DIR}/projects/default/"
    WLD_ASSETPATH="${CMAKE_SOURCE_DIR}/projects/default/assets"
    WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    WLD_WORLD_DIR="${CMAKE_SOURCE_DIR}/Engine/"
    # Slang-T5:唯一工具目录入口(见上面的 WLD_SLANG_DIR 说明)。
    WLD_SLANG_DIR="${WLD_SLANG_DIR}"
)

# Debug 配置启用引擎断言与调试期诊断(内存哨兵/污染填充等):
# 之前 WLD_DEBUG 从未定义,导致 WLD_CORE_ASSERT 在所有配置下都是空操作。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(World PUBLIC WLD_DEBUG)
endif()

option(WORLD_BUILD_SCRIPT_TESTS "Build WorldEngine script lifecycle checks" OFF)
if(WORLD_BUILD_SCRIPT_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
