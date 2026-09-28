cmake_minimum_required(VERSION 3.16)

# ============================================================================
# 这个项目自己的 `Game.dll` 构建(PROJ-8/T2)。
#
# 引擎以"子项目模式"被引用:只构建引擎运行时(World)与本项目的 Game 模块
# (空模板 = 还没有自定义 C++ 类型,Game.dll 是一个只有模块骨架的 DLL);
# 引擎自己的 Editor / Runtime / Launcher / tests 都不在这里构建。产物:
#   <项目根>/build/x64-<配置>/bin/<配置>/Game/<配置>/Game.dll
#   (同目录还会有 WorldRuntime.dll —— 引擎运行时)
# 编辑器用 `--project <项目根>` 打开时会**优先**加载项目构建出来的这份 Game.dll。
#
# 引擎根从哪来(按顺序):
#   1) `-DWE_ROOT=<引擎根>` 或环境变量 `WE_ROOT`;
#   2) 项目根下向导生成的 `<项目名>-Edit.cmd` / `-Play.cmd` 里记录的引擎根;
#   3) 相对推导 `<项目根>\..\..`(模板位于 `<引擎根>\templates\project-empty` 时的口径)。
# 命令行构建见同目录 `build.cmd`。
# ============================================================================
set(WE_ROOT "" CACHE PATH "WorldEngine engine root (the checkout that owns Engine/ and Game/)")
if(NOT WE_ROOT AND DEFINED ENV{WE_ROOT})
    file(TO_CMAKE_PATH "$ENV{WE_ROOT}" WE_ROOT)
endif()

if(NOT WE_ROOT)
    file(GLOB _we_launcher_cmds "${CMAKE_CURRENT_SOURCE_DIR}/*-Edit.cmd" "${CMAKE_CURRENT_SOURCE_DIR}/*-Play.cmd")
    foreach(_we_launcher IN LISTS _we_launcher_cmds)
        if(NOT WE_ROOT)
            file(READ "${_we_launcher}" _we_launcher_text)
            string(REGEX MATCH "WE_ROOT=([^\"\r\n]+)" _we_launcher_match "${_we_launcher_text}")
            if(_we_launcher_match)
                file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" WE_ROOT)
                message(STATUS "WorldEngine engine root (from ${_we_launcher}): ${WE_ROOT}")
            endif()
        endif()
    endforeach()
endif()

if(NOT WE_ROOT)
    get_filename_component(_we_relative_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    if(EXISTS "${_we_relative_root}/Engine/CMakeLists.txt" AND EXISTS "${_we_relative_root}/Game/CMakeLists.txt")
        set(WE_ROOT "${_we_relative_root}")
        message(STATUS "WorldEngine engine root (relative layout): ${WE_ROOT}")
    endif()
endif()

if(NOT WE_ROOT OR NOT EXISTS "${WE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "WorldEngine engine root not found. Pass -DWE_ROOT=<engine checkout> (or set the WE_ROOT "
        "environment variable); it must be the directory that owns CMakeLists.txt, Engine/ and Game/.")
endif()

get_filename_component(_we_project_name "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
project("${_we_project_name}" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Game 模块源码 = 本项目的 src/**(组件 / 脚本 / 你加进去的一切);
# schema 的输入发现、生成物(<项目根>/src/Generated/**)与类型账本(Game.manifest)同样落在本项目里。
set(WLD_GAME_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# 引擎根不是顶层 CMAKE_SOURCE_DIR ⇒ 引擎侧自动进入子项目模式(只建 World + Game)。
add_subdirectory("${WE_ROOT}" "${CMAKE_BINARY_DIR}/engine")
