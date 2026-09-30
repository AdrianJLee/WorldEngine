cmake_minimum_required(VERSION 3.16)

# 设置输出目录 (类似 Premake 的 outputdir)
# PROJ-8/T2:引擎仓库根 = **本文件所在目录**。子项目模式下(游戏项目自带 CMakeLists 用
# `add_subdirectory(<引擎根>)` 引用本仓库)CMAKE_SOURCE_DIR 是**项目根**;引擎自己的编译期
# 宏与第三方路径必须继续锚在引擎仓库 ⇒ 本仓库统一走 WLD_ENGINE_ROOT。
set(WLD_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

# 子项目模式(PROJ-8/T2):
#   * 宿主项目 `add_subdirectory(<引擎根>)` ⇒ 自动为 ON(本仓库不是顶层 CMAKE_SOURCE_DIR);
#   * 也可以显式 `-DWORLD_AS_SUBPROJECT=ON`(引擎仍是顶层 CMakeLists 时同样适用);
#   * ON = 只构建 World(+ schema-compiler 生成器)与 Game,不构建 Editor/Runtime/Launcher/tests。
option(WORLD_AS_SUBPROJECT "Build only the engine runtime + game module (the host project owns the binaries)" OFF)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(WORLD_AS_SUBPROJECT ON)
endif()

set(OUTPUT_CONFIG "${CMAKE_BUILD_TYPE}-${CMAKE_SYSTEM_NAME}-x64")

set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${WLD_ENGINE_ROOT}")

project(World VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 查找 Vulkan SDK(可选):headers+volk 已 vendor,SDK 仅用于可选验证层
find_package(Vulkan QUIET)

# --- Slang(Slang-T5 起是**唯一**的着色器编译器;Slang-T6b 删掉了旧的两套外部工具)---
# 工具入库:CLI 子集(vendor/tools/slang/bin,~25MiB)随仓库分发,取件/升级走
# tools/agents/update-slang-tool.ps1(固定版本 + sha256);理由与实测见
# vendor/tools/slang/README.md。整包(含 slangd/LSP)仍可选走 fetch-slang.ps1 落到仓库外。
# WLD_SLANG_DIR = **含 slangc.exe 的目录**,是"去哪儿找编译器"的唯一事实源:
# 运行时代码只读这个编译期常量,不再有 env / 同级目录扫描 / PATH 兜底猜测。
if(NOT DEFINED WLD_SLANG_DIR)
    if(DEFINED ENV{WLD_SLANG_DIR} AND NOT "$ENV{WLD_SLANG_DIR}" STREQUAL "")
        set(WLD_SLANG_DIR "$ENV{WLD_SLANG_DIR}")
    else()
        set(WLD_SLANG_DIR "${WLD_ENGINE_ROOT}/vendor/tools/slang/bin")
    endif()
endif()
file(TO_CMAKE_PATH "${WLD_SLANG_DIR}" WLD_SLANG_DIR)
message(STATUS "WorldEngine Slang tool dir: ${WLD_SLANG_DIR}")
if(NOT EXISTS "${WLD_SLANG_DIR}/slangc.exe")
    message(WARNING "slangc.exe not found in ${WLD_SLANG_DIR}; dev-time shader compilation will "
        "fail (packaged builds use baked artifacts). Run tools/agents/update-slang-tool.ps1 to "
        "restore the in-repo tool, or configure with -DWLD_SLANG_DIR=<dir containing slangc.exe>.")
endif()

# --- 添加子项目---
add_subdirectory(Engine)

if(WORLD_AS_SUBPROJECT)
    # 子项目模式:宿主项目只拿引擎运行时 + Game 模块;编辑器/运行时/启动器/tests 都不在这里建。
    add_subdirectory(Game)
else()
    add_subdirectory(Editor)
    add_subdirectory(Game)
    add_subdirectory(Runtime)
    # PROJ-3/T2:项目侧小启动器(WeEdit.exe / WePlay.exe)——零引擎依赖,可单独复制进项目目录。
    add_subdirectory(Launcher)

    add_dependencies(Runtime Game)
    add_dependencies(Editor Runtime)
endif()

# --- 引擎插件(方案 §1:引擎插件住在 <引擎根>/plugins/<name>/,**可选**收集)--------
#   * 存在性判断:plugins/<name>/CMakeLists.txt 存在才加入构建;没有 plugins/ = 零开销,
#     与今天完全一致(不破坏现有 checkout 的构建)。
#   * 子项目模式下宿主项目自己拥有二进制,不在这里收集插件(需要时由宿主显式加子目录)。
#   * 产物落构建树(bin/<cfg>/plugins),不写仓库;新增插件目录后重跑 configure。
option(WORLD_BUILD_ENGINE_PLUGINS "Build engine plugins under <engine>/plugins/**" ON)
if(WORLD_BUILD_ENGINE_PLUGINS AND NOT WORLD_AS_SUBPROJECT)
    file(GLOB WLD_ENGINE_PLUGIN_CMAKELISTS "${WLD_ENGINE_ROOT}/plugins/*/CMakeLists.txt")
    foreach(_we_plugin_cmakelists IN LISTS WLD_ENGINE_PLUGIN_CMAKELISTS)
        get_filename_component(_we_plugin_dir "${_we_plugin_cmakelists}" DIRECTORY)
        get_filename_component(_we_plugin_name "${_we_plugin_dir}" NAME)
        message(STATUS "WorldEngine engine plugin: ${_we_plugin_dir}")
        add_subdirectory("${_we_plugin_dir}" "plugins/${_we_plugin_name}")
    endforeach()
endif()


target_compile_definitions(World PUBLIC
    WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
    # PLUG-CLEAN-1:宿主引擎版本(唯一事实源 = 本文件顶部的 project(World VERSION …))。
    # 插件清单的 engine: 最低版本约束按它做语义化比较(见 World/Plugins/PluginManifest.h)。
    WLD_ENGINE_VERSION="${PROJECT_VERSION}"
    # 开发期绝对锚点(不带尾分隔符):供"与进程 CWD 无关"的路径拼接(如定位 Game.dll)。
    # PROJ-8/T2:一律取引擎根 —— 子项目模式下 CMAKE_SOURCE_DIR 是**项目根**,不能用。
    WLD_REPO_ROOT="${WLD_ENGINE_ROOT}"
    WLD_EDITOR_DIR="${WLD_ENGINE_ROOT}/Editor/"
    WLD_LOCAL_DIR="${WLD_ENGINE_ROOT}/local/"
    # WLD_GAME_DIR = Game **模块**目录(只放 gameplay DLL 源码);项目内容根见 WLD_PROJECT_DIR。
    WLD_GAME_DIR="${WLD_ENGINE_ROOT}/Game/"
    # Layout-S7:项目内容(清单/关卡表/assets)在 projects/<名>/。
    # PROJ-7/T3:引擎不再内置默认项目 —— projects/ 只是"项目默认放在仓库里"的容器目录
    # (见 projects/README.md),本身不是项目。宏名与运行期语义不变:--project / 环境变量
    # WLD_PROJECT_DIR 仍是唯一入口;下面两个默认值指向不存在的资产目录是**有意的**
    # (= "没有当前项目":没有清单就没有内容根,宿主据此只出项目启动器)。
    WLD_PROJECT_DIR="${WLD_ENGINE_ROOT}/projects/"
    WLD_ASSETPATH="${WLD_ENGINE_ROOT}/projects/assets"
    # PROJ-7/T1:测试夹具内容根 —— 只给 tests/ 用,把"测试真的读的项目内容"从项目目录搬进
    # 仓库内夹具(tests/fixtures/content);WLD_PROJECT_DIR / WLD_ASSETPATH 的运行期语义不变。
    WLD_TEST_ASSETPATH="${WLD_ENGINE_ROOT}/tests/fixtures/content"
    WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    WLD_WORLD_DIR="${WLD_ENGINE_ROOT}/Engine/"
    # Slang-T5:唯一工具目录入口(见上面的 WLD_SLANG_DIR 说明)。
    WLD_SLANG_DIR="${WLD_SLANG_DIR}"
)

# Debug 配置启用引擎断言与调试期诊断(内存哨兵/污染填充等):
# 之前 WLD_DEBUG 从未定义,导致 WLD_CORE_ASSERT 在所有配置下都是空操作。
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(World PUBLIC WLD_DEBUG)
endif()

option(WORLD_BUILD_SCRIPT_TESTS "Build WorldEngine script lifecycle checks" OFF)
if(WORLD_BUILD_SCRIPT_TESTS AND NOT WORLD_AS_SUBPROJECT)
    enable_testing()
    add_subdirectory(tests)
endif()
