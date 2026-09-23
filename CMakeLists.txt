cmake_minimum_required(VERSION 3.16)

# 设置输出目录 (类似 Premake 的 outputdir)
set(OUTPUT_CONFIG "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

project(World VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 Vulkan SDK(可选):headers+volk 已 vendor,SDK 仅用于可选验证层
find_package(Vulkan QUIET)

# --- DXC 着色器编译器:SPIR-V 代码生成要求 dxc.exe 与 dxcompiler.dll 版本匹配,
# 仓库内的 vendor/dxc/dxc.exe 只带壳。优先用 Vulkan SDK 的完整工具链(与 glslc 同目录),
# 找不到时回退 vendor/dxc(仅 DXIL 场景可用)。
set(WLD_DXC_DIR "${CMAKE_SOURCE_DIR}/vendor/dxc/")
if(Vulkan_GLSLC_EXECUTABLE)
    get_filename_component(_we_vulkan_bin "${Vulkan_GLSLC_EXECUTABLE}" DIRECTORY)
    if(EXISTS "${_we_vulkan_bin}/dxc.exe" AND EXISTS "${_we_vulkan_bin}/dxcompiler.dll")
        set(WLD_DXC_DIR "${_we_vulkan_bin}/")
    endif()
endif()
message(STATUS "WorldEngine DXC tool dir: ${WLD_DXC_DIR}")
if(WLD_DXC_DIR STREQUAL "${CMAKE_SOURCE_DIR}/vendor/dxc/")
    message(WARNING "Complete dxc toolchain not found in the Vulkan SDK; "
        "dev-time shader compilation may fail (packaged builds use baked artifacts).")
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
    WLD_EDITOR_DIR="${CMAKE_SOURCE_DIR}/Editor/"
    WLD_GAME_DIR="${CMAKE_SOURCE_DIR}/Game/"
    WLD_ASSETPATH="${CMAKE_SOURCE_DIR}/Game/assets"
    WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    WLD_WORLD_DIR="${CMAKE_SOURCE_DIR}/Engine/"
    WLD_DXC_DIR="${WLD_DXC_DIR}"
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
