# PROJ-3/T2:项目侧小启动器(WeEdit.exe / WePlay.exe)。
#
# 目的:把这两个 exe 单独复制进任意项目目录,双击就能用本机引擎打开该项目 ——
#   WeEdit.exe ⇒ <引擎根>/<WLD_OUTPUT_DIR>Editor/<WLD_BUILD_TYPE>/Editor.exe  --project <exe 所在目录>
#   WePlay.exe ⇒ <引擎根>/<WLD_OUTPUT_DIR>Runtime/<WLD_BUILD_TYPE>/Runtime.exe --project <exe 所在目录>
#
# 硬性约束:
#  * 零引擎依赖:不链接 World/Game,产物不加载 WorldRuntime.dll ⇒ 可以单独复制到任意项目目录。
#  * 同一份源码(src/LauncherMain.cpp)编译两次,靠 WLD_LAUNCH_MODE_EDIT / _PLAY 区分模式。
#  * GUI 子系统(WIN32)⇒ 双击不弹控制台窗口。
#  * 引擎根/构建布局来自编译期宏:Launcher 不链接 World,拿不到它的 PUBLIC 定义,
#    所以在这里按根 CMakeLists 的口径逐字注入同样的三个宏(见下面的说明)。
#
# 树内接入:根 CMakeLists.txt 负责 add_subdirectory(Launcher)(主 agent 维护)。
# 独立配置(不依赖根 CMake,便于单独编译自证):
#   cmake -S Launcher -B <dir> -DCMAKE_BUILD_TYPE=Debug -G "Visual Studio 18 2026" -A x64

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(Launcher LANGUAGES CXX)
    # 树内由根 CMakeLists 统一设成 C++17;独立配置时这里补齐(源码用到 C++17 的
    # std::wstring::data() 非常量重载)。
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# 引擎根(编译期常量,写进 WLD_REPO_ROOT):树内 = 顶级源码目录;独立配置时退回父目录。
# 允许 -DWLD_LAUNCHER_REPO_ROOT=<dir> 覆盖,用于在临时布局里验证"引擎根/目标 exe 缺失"
# 分支(不改动共享构建目录里的真实产物)。
if(NOT DEFINED WLD_LAUNCHER_REPO_ROOT)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        get_filename_component(WLD_LAUNCHER_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
    else()
        set(WLD_LAUNCHER_REPO_ROOT "${CMAKE_SOURCE_DIR}")
    endif()
endif()

set(WLD_LAUNCHER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/LauncherMain.cpp")

add_executable(WeEdit WIN32 "${WLD_LAUNCHER_SOURCE}")
add_executable(WePlay WIN32 "${WLD_LAUNCHER_SOURCE}")

foreach(wld_launcher_target IN ITEMS WeEdit WePlay)
    if(wld_launcher_target STREQUAL "WeEdit")
        set(wld_launcher_mode WLD_LAUNCH_MODE_EDIT)
    else()
        set(wld_launcher_mode WLD_LAUNCH_MODE_PLAY)
    endif()

    target_compile_definitions(${wld_launcher_target} PRIVATE
        ${wld_launcher_mode}
        # 与根 CMakeLists 里 World 的 PUBLIC 定义逐字一致(Launcher 不链接 World,拿不到它们):
        WLD_REPO_ROOT="${WLD_LAUNCHER_REPO_ROOT}"
        WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
        WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
    )

    if(MSVC)
        target_compile_options(${wld_launcher_target} PRIVATE "/utf-8")
        # 产物要能单独复制(不要求目标机器装 VC 运行库)⇒ 启动器目标用静态 CRT。
        # 它不链接任何引擎库,不存在跨 CRT 边界的问题;CMP0091 由 cmake_minimum_required(3.16) 打开。
        set_property(TARGET ${wld_launcher_target} PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()

    # 只用系统库(零第三方依赖);输出布局与 Editor/Runtime 一致:<build>/Launcher/<配置>/。
    target_link_libraries(${wld_launcher_target} PRIVATE user32 shell32)
    set_target_properties(${wld_launcher_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Launcher")
endforeach()
