cmake_minimum_required(VERSION 3.16)

# ============================================================================
# 这个项目自己的 `Game.dll` 构建(PROJ-8/T2 建;PROJ-12/T1 增加"快路径")。
#
# 两种模式,产物一致:
#   * 源码模式(默认):`add_subdirectory(<引擎根>)` —— 引擎侧自动进入子项目模式,
#     只构建引擎运行时(World,自定义类型 = 本项目的 schema)与本项目的 Game 模块;
#     引擎自己的 Editor / Runtime / Launcher / tests 都不在这里构建。
#   * 快路径(WE_ENGINE_BUILD_DIR 指向一份已编译好的引擎构建目录,例如
#     E:\WorldEngine\build\x64-Debug):不再编译引擎,只编本项目的 src/** 与引擎的
#     Game 模块骨架,链接该目录里预编译的 WorldRuntime.lib,并用同一目录里预编译的
#     schema-compiler.exe 跑 schema 生成。构建耗时从"整套引擎"降到"一个 DLL"。
#
# 产物(两种模式一致):
#   <项目根>/build/x64-<配置>/bin/<配置>/Game/<配置>/Game.dll
#   (同目录还会有 WorldRuntime.dll —— 引擎运行时;快路径下从引擎构建目录复制)
#   (快路径下同时复制 WorldRuntime.pdb —— 引擎的调试符号,引擎构建目录里没有该文件时只警告一行)
# 编辑器用 `--project <项目根>` 打开时会**优先**加载项目构建出来的这份 Game.dll。
#
# 引擎根解析顺序(与同目录 build.cmd 同一口径):
#   1) `-DWE_ROOT=<引擎根>`(命令行 / CMakePresets.json 的 cacheVariables)或环境变量 WE_ROOT;
#   2) `<项目根>/.we/engine-root.txt`(构建入口生成:一行文本 = 引擎根);
#   3) 相对推导 `<项目根>\..\..`(模板位于 `<引擎根>\templates\project-example` 时的口径);
#   4) 旧项目兼容:向导生成的 `<项目名>-Edit.cmd` / `-Play.cmd` 里记录的引擎根(最后兜底)。
#
# 快路径判定:
#   * 显式给了 WE_ENGINE_BUILD_DIR(-D / 环境变量)⇒ 该目录必须完整,缺产物直接报错,不静默回落;
#   * 没给 ⇒ 若 `<引擎根>/build/x64-<配置>` 同时具备 WorldRuntime.lib 与 schema-compiler.exe
#     就自动使用,否则回落源码模式(并说明原因)。
#   * WE_FORCE_SOURCE_MODE=ON(或同名环境变量)⇒ 忽略以上探测,强制源码模式
#     (改了引擎源码时必须用它:快路径复用的是引擎构建目录里的旧二进制)。
#
# cache 陈旧守卫(评审 #10):
#   WE_ROOT / WE_ENGINE_BUILD_DIR 一旦被写进 CMakeCache.txt(显式 -D、预设、或上一次配置),
#   下一次配置里 cache 会**盖过**文件与环境变量 —— 用户改过引擎根却仍在编旧引擎,而且看不出来。
#   配置期比对"本次解析值 / 磁盘上那份 cache(此刻仍是上一次配置的值)/ .we/engine-root.txt":
#   * 本次解析值与磁盘 cache 不同 ⇒ 本次有显式覆盖(-D / 预设 / GUI),覆盖优先,只提示一行;
#   * 本次解析值就是从 cache 沿用的 ⇒ 若 .we/engine-root.txt 与它不同(都非空),或本次传入的
#     WE_ENGINE_BUILD_DIR 与它不同(都非空),直接 FATAL_ERROR,提示删 build/ 或 -D 覆盖。
# 命令行构建见同目录 `build.cmd`;VS 打开项目文件夹按 CMakePresets.json 配置。
# ============================================================================
# ---- 1. 引擎根 --------------------------------------------------------------
# 先无条件读"事实源"文件(一行路径):解析时拿它兜底,1b 的 cache 陈旧守卫还要用它比对。
set(_we_root_file "${CMAKE_CURRENT_SOURCE_DIR}/.we/engine-root.txt")
set(_we_fact_root "")
if(EXISTS "${_we_root_file}")
    file(READ "${_we_root_file}" _we_root_text)
    string(REPLACE "\r" "" _we_root_text "${_we_root_text}")
    string(REGEX REPLACE "\n.*" "" _we_root_text "${_we_root_text}")
    string(STRIP "${_we_root_text}" _we_root_text)
    if(_we_root_text)
        file(TO_CMAKE_PATH "${_we_root_text}" _we_fact_root)
    endif()
endif()

set(WE_ROOT "" CACHE PATH "WorldEngine engine root (the checkout that owns CMakeLists.txt, Engine/ and Game/)")
if(NOT WE_ROOT AND DEFINED ENV{WE_ROOT})
    file(TO_CMAKE_PATH "$ENV{WE_ROOT}" WE_ROOT)
    message(STATUS "WorldEngine engine root (environment WE_ROOT): ${WE_ROOT}")
endif()

if(NOT WE_ROOT AND _we_fact_root)
    set(WE_ROOT "${_we_fact_root}")
    message(STATUS "WorldEngine engine root (.we/engine-root.txt): ${WE_ROOT}")
endif()

if(NOT WE_ROOT)
    get_filename_component(_we_relative_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    if(EXISTS "${_we_relative_root}/Engine/CMakeLists.txt" AND EXISTS "${_we_relative_root}/Game/CMakeLists.txt")
        set(WE_ROOT "${_we_relative_root}")
        message(STATUS "WorldEngine engine root (relative layout): ${WE_ROOT}")
    endif()
endif()

if(NOT WE_ROOT)
    # 旧项目兜底:向导生成的 *-Edit.cmd / *-Play.cmd 里记录的引擎根(新项目不再依赖它们)。
    file(GLOB _we_launcher_cmds "${CMAKE_CURRENT_SOURCE_DIR}/*-Edit.cmd" "${CMAKE_CURRENT_SOURCE_DIR}/*-Play.cmd")
    foreach(_we_launcher IN LISTS _we_launcher_cmds)
        if(NOT WE_ROOT)
            file(READ "${_we_launcher}" _we_launcher_text)
            string(REGEX MATCH "WE_ROOT=([^\"\r\n]+)" _we_launcher_match "${_we_launcher_text}")
            if(_we_launcher_match)
                file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" WE_ROOT)
                message(STATUS "WorldEngine engine root (legacy ${_we_launcher}): ${WE_ROOT}")
            endif()
        endif()
    endforeach()
endif()

if(NOT WE_ROOT OR NOT EXISTS "${WE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "WorldEngine engine root not found. Pass -DWE_ROOT=<engine checkout>, set the WE_ROOT "
        "environment variable, or write the path into <project>/.we/engine-root.txt; it must be the "
        "directory that owns CMakeLists.txt, Engine/ and Game/.")
endif()
file(TO_CMAKE_PATH "${WE_ROOT}" WE_ROOT)

# ---- 1b. cache 陈旧守卫(评审 #10)--------------------------------------------
# CMakeCache.txt 在 configure **结束**时才回写,所以此刻磁盘上那份还是上一次配置的值 ——
# 用它区分"本次显式给的值"与"从 cache 沿用的旧值"(实测:第一次 -D AAA、第二次 -D BBB 时,
# 第二次执行期间磁盘上读到的仍是 AAA)。
set(_we_cached_root "")
set(_we_cached_engine_build_dir "")
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
    file(READ "${CMAKE_BINARY_DIR}/CMakeCache.txt" _we_cache_text)
    if(_we_cache_text MATCHES "\nWE_ROOT:[A-Za-z]*=([^\r\n]*)")
        set(_we_cached_root "${CMAKE_MATCH_1}")
    endif()
    if(_we_cache_text MATCHES "\nWE_ENGINE_BUILD_DIR:[A-Za-z]*=([^\r\n]*)")
        set(_we_cached_engine_build_dir "${CMAKE_MATCH_1}")
    endif()
endif()

# 路径比对口径:统一斜杠、去尾部斜杠;Windows 下盘符/路径大小写不敏感。
macro(_we_canonical_path _out _value)
    file(TO_CMAKE_PATH "${_value}" ${_out})
    string(REGEX REPLACE "/+$" "" ${_out} "${${_out}}")
    if(WIN32)
        string(TOLOWER "${${_out}}" ${_out})
    endif()
endmacro()

if(_we_fact_root AND _we_cached_root)
    _we_canonical_path(_we_cached_root_canon "${_we_cached_root}")
    _we_canonical_path(_we_root_canon "${WE_ROOT}")
    _we_canonical_path(_we_fact_root_canon "${_we_fact_root}")
    if(NOT _we_fact_root_canon STREQUAL _we_cached_root_canon)
        if(_we_root_canon STREQUAL _we_cached_root_canon)
            message(FATAL_ERROR
                "改过引擎根后请删除 <项目>/build/ 重新配置\n"
                "（或传 -DWE_ROOT=… 覆盖）:\n"
                "  ${_we_root_file} 记录的是:\n    ${_we_fact_root}\n"
                "  而 CMakeCache.txt 里还是上一次配置的:\n    ${_we_cached_root}\n"
                "  这份 cache 会盖过文件,继续构建就会用旧引擎。\n"
                "The engine root changed after this build directory was configured, so the cached root\n"
                "would win. Delete the project build directory and configure again:\n"
                "    ${CMAKE_BINARY_DIR}")
        else()
            message(STATUS "WE_ROOT=${WE_ROOT} was given explicitly and overrides "
                "${_we_root_file} (${_we_fact_root}); the cached value was ${_we_cached_root}")
        endif()
    endif()
endif()

# ---- 2. 配置 + 快路径判定 ----------------------------------------------------
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type: Debug or Release (fast mode selects the engine build dir with it)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release)
    message(STATUS "CMAKE_BUILD_TYPE was empty; defaulting to Debug")
endif()

set(WE_ENGINE_BUILD_DIR "" CACHE PATH
    "Prebuilt engine build directory to reuse (e.g. <engine>/build/x64-Debug); empty = probe <engine>/build/x64-<config>, then source mode")
if(NOT WE_ENGINE_BUILD_DIR AND DEFINED ENV{WE_ENGINE_BUILD_DIR})
    file(TO_CMAKE_PATH "$ENV{WE_ENGINE_BUILD_DIR}" WE_ENGINE_BUILD_DIR)
endif()
# 留一份未经 get_filename_component 改写的原值:守卫要比对"上一次配置留在 cache 里的写法"
# 与"本次解析出的写法",而 ABSOLUTE 在 Windows 上会把 8.3 短名(ADMINI~1)展开成长名,
# 同一个目录会因此比较不相等。
set(_we_build_dir_raw "${WE_ENGINE_BUILD_DIR}")
if(WE_ENGINE_BUILD_DIR)
    get_filename_component(WE_ENGINE_BUILD_DIR "${WE_ENGINE_BUILD_DIR}" ABSOLUTE)
    file(TO_CMAKE_PATH "${WE_ENGINE_BUILD_DIR}" WE_ENGINE_BUILD_DIR)
    set(_we_fast_path_explicit TRUE)
else()
    set(_we_fast_path_explicit FALSE)
endif()

# 排障/对照开关:强制源码模式(改引擎源码时必须用它 —— 快路径复用的引擎二进制是旧的)。
set(WE_FORCE_SOURCE_MODE OFF CACHE BOOL "Ignore prebuilt engine build dirs and build the engine from source")
if(DEFINED ENV{WE_FORCE_SOURCE_MODE} AND NOT "$ENV{WE_FORCE_SOURCE_MODE}" STREQUAL ""
        AND NOT "$ENV{WE_FORCE_SOURCE_MODE}" STREQUAL "0")
    set(WE_FORCE_SOURCE_MODE ON)
endif()

if(WE_FORCE_SOURCE_MODE)
    if(WE_ENGINE_BUILD_DIR)
        message(STATUS "WE_FORCE_SOURCE_MODE is on; ignoring WE_ENGINE_BUILD_DIR=${WE_ENGINE_BUILD_DIR}")
    endif()
    set(WE_ENGINE_BUILD_DIR "")
    set(_we_fast_path_explicit FALSE)
elseif(NOT WE_ENGINE_BUILD_DIR)
    set(_we_probe_build_dir "${WE_ROOT}/build/x64-${CMAKE_BUILD_TYPE}")
    if(EXISTS "${_we_probe_build_dir}/Engine/${CMAKE_BUILD_TYPE}/WorldRuntime.lib"
            AND EXISTS "${_we_probe_build_dir}/Engine/generators/schema-compiler/${CMAKE_BUILD_TYPE}/schema-compiler.exe")
        set(WE_ENGINE_BUILD_DIR "${_we_probe_build_dir}")
        message(STATUS "Prebuilt engine found, using the fast path: ${WE_ENGINE_BUILD_DIR}")
    elseif(EXISTS "${_we_probe_build_dir}")
        message(STATUS "Engine build dir ${_we_probe_build_dir} is missing the ${CMAKE_BUILD_TYPE} "
            "WorldRuntime.lib or schema-compiler.exe; building the engine from source instead "
            "(build the engine there for the fast path).")
    endif()
endif()

# cache 沿用的 WE_ENGINE_BUILD_DIR 会盖过本次传入的环境变量(同 1b 口径):用户以为换了
# 引擎构建目录,实际还在编旧引擎 —— 两者都非空且不同 ⇒ 硬失败,不静默继续。
if(_we_cached_engine_build_dir AND WE_ENGINE_BUILD_DIR)
    _we_canonical_path(_we_cached_build_dir_canon "${_we_cached_engine_build_dir}")
    _we_canonical_path(_we_build_dir_canon "${_we_build_dir_raw}")
    set(_we_env_build_dir "")
    if(DEFINED ENV{WE_ENGINE_BUILD_DIR} AND NOT "$ENV{WE_ENGINE_BUILD_DIR}" STREQUAL "")
        file(TO_CMAKE_PATH "$ENV{WE_ENGINE_BUILD_DIR}" _we_env_build_dir)
    endif()
    if(_we_build_dir_canon STREQUAL _we_cached_build_dir_canon AND _we_env_build_dir)
        _we_canonical_path(_we_env_build_dir_canon "${_we_env_build_dir}")
        if(NOT _we_env_build_dir_canon STREQUAL _we_build_dir_canon)
            message(FATAL_ERROR
                "改过引擎根后请删除 <项目>/build/ 重新配置\n"
                "（或传 -DWE_ROOT=… 覆盖）:\n"
                "  本次传入的 WE_ENGINE_BUILD_DIR 是:\n    ${_we_env_build_dir}\n"
                "  而 CMakeCache.txt 里还是上一次配置的:\n    ${_we_cached_engine_build_dir}\n"
                "  这份 cache 会盖过环境变量,继续构建就会复用旧引擎构建目录。\n"
                "The prebuilt engine build directory passed in this run was dropped because the cached\n"
                "value wins; delete the project build directory and configure again, or pass\n"
                "-DWE_ENGINE_BUILD_DIR=<new engine build dir> to override:\n"
                "    ${CMAKE_BINARY_DIR}")
        endif()
    endif()
endif()

if(WE_ENGINE_BUILD_DIR)
    set(WE_FAST_WORLD_LIB "${WE_ENGINE_BUILD_DIR}/Engine/${CMAKE_BUILD_TYPE}/WorldRuntime.lib")
    set(WE_FAST_WORLD_DLL "${WE_ENGINE_BUILD_DIR}/Engine/${CMAKE_BUILD_TYPE}/WorldRuntime.dll")
    set(WE_FAST_WORLD_PDB "${WE_ENGINE_BUILD_DIR}/Engine/${CMAKE_BUILD_TYPE}/WorldRuntime.pdb")
    set(WE_FAST_SCHEMA_COMPILER "${WE_ENGINE_BUILD_DIR}/Engine/generators/schema-compiler/${CMAKE_BUILD_TYPE}/schema-compiler.exe")
    set(_we_missing_artifacts "")
    foreach(_we_artifact "${WE_FAST_WORLD_LIB}" "${WE_FAST_WORLD_DLL}" "${WE_FAST_SCHEMA_COMPILER}")
        if(NOT EXISTS "${_we_artifact}")
            list(APPEND _we_missing_artifacts "${_we_artifact}")
        endif()
    endforeach()
    if(_we_missing_artifacts)
        if(_we_fast_path_explicit)
            string(REPLACE ";" "\n  " _we_missing_text "${_we_missing_artifacts}")
            message(FATAL_ERROR
                "WE_ENGINE_BUILD_DIR=${WE_ENGINE_BUILD_DIR} is not a usable prebuilt engine build for "
                "configuration '${CMAKE_BUILD_TYPE}'.\nMissing:\n  ${_we_missing_text}\n"
                "Either the engine was not built for '${CMAKE_BUILD_TYPE}' (configuration mismatch) or "
                "the path is wrong. Build the engine there (configure with "
                "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}, build with --config ${CMAKE_BUILD_TYPE}), or "
                "unset WE_ENGINE_BUILD_DIR to build the engine from source.")
        else()
            message(STATUS "Auto-detected engine build directory is incomplete; falling back to source mode.")
            set(WE_ENGINE_BUILD_DIR "")
        endif()
    endif()
endif()

get_filename_component(_we_project_name "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
project("${_we_project_name}" LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Game 模块源码 = 本项目的 src/**(组件 / 脚本 / 你加进去的一切);
# schema 的输入发现、生成物(<项目根>/src/Generated/**)与类型账本(Game.manifest)同样落在本项目里。
set(WLD_GAME_PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

# ---- 项目启动器目标(ProjectRun,PROJ-15/T1)-----------------------------------
# VS 的 CMake 工程模式只在 Startup Item 下拉里列**可执行 CMake 目标**(官方文档口径);
# 项目侧 World 是库、Game 是 DLL、Editor/Runtime 属引擎侧,所以这里给项目补**一个**自己的
# 可执行目标,并把"用哪个引擎、项目根在哪、开编辑器还是跑游戏"收进
# src/Tools/ProjectRun.cpp(零引擎依赖,只链接系统库)。默认 = 打开编辑器,加 `--play`
# 才拉起 Runtime —— 下拉里因此只有一个 ProjectRun.exe,不用在两项之间挑。
# 产物布局与 .vs/launch.vs.json 的 projectTarget 字符串逐字对齐:<build>/bin/<配置>/。
set(WE_PROJECT_RUNNER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/src/Tools/ProjectRun.cpp")

function(_we_add_project_runner _we_target)
    add_executable(${_we_target} WIN32 "${WE_PROJECT_RUNNER_SOURCE}")
    target_compile_definitions(${_we_target} PRIVATE
        # 与引擎侧 World 的 PUBLIC 定义逐字一致(启动器不链接 World,拿不到它们):
        WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
        WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    )
    if(MSVC)
        target_compile_options(${_we_target} PRIVATE "/utf-8")
        # 与 Launcher 一致:目标机器不装 VC 运行库也能跑(启动器不跨 CRT 边界)。
        set_property(TARGET ${_we_target} PROPERTY MSVC_RUNTIME_LIBRARY
            "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
    # F5 先从 VS 构建项目自己的 C++(Game.dll),再拉起引擎宿主。
    add_dependencies(${_we_target} Game)
    # 只用系统库(零第三方依赖)。
    target_link_libraries(${_we_target} PRIVATE user32 shell32)
    # VS 多配置生成器会自动再补 <配置> 子目录 ⇒ 产物恒为 bin/<配置>/<目标>.exe。
    set_target_properties(${_we_target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endfunction()

if(NOT WE_ENGINE_BUILD_DIR)
    # ---- 3a. 源码模式:引擎作为子项目构建 ------------------------------------
    # 引擎根不是顶层 CMAKE_SOURCE_DIR ⇒ 引擎侧自动进入子项目模式(只建 World + Game)。
    message(STATUS "WorldEngine build mode: source (add_subdirectory ${WE_ROOT})")
    add_subdirectory("${WE_ROOT}" "${CMAKE_BINARY_DIR}/engine")
    # 注意:引擎侧 Game/CMakeLists.txt 的 src/** 通配也会看到 src/Tools/ProjectRun.cpp;
    # 该 TU 在 GAME_BUILD_DLL 编译里整份跳过(见 ProjectRun.cpp 顶部说明)。
    _we_add_project_runner(ProjectRun)
    return()
endif()

# ---- 3b. 快路径:复用已编译引擎,只编 Game -------------------------------------
message(STATUS "WorldEngine build mode: fast path (prebuilt engine: ${WE_ENGINE_BUILD_DIR})")

# game-schema-build.cmake 的工作目录需要它(-D 传参之外也保持项目作用域可见)。
set(WLD_ENGINE_ROOT "${WE_ROOT}")

# 与 Engine/CMakeLists.txt 里 `World` 目标的 PUBLIC 接口对齐:源码模式下 Game 从 target 上
# 继承到的 include/宏,快路径手动复刻一份(内容不一致时以"源码模式能编过"为准核对)。
add_library(World SHARED IMPORTED GLOBAL)
set(_we_world_include_dirs
    "${WLD_ENGINE_ROOT}/Engine/src"
    "${WLD_ENGINE_ROOT}/third_party/glm"
    "${WLD_ENGINE_ROOT}/third_party/entt/include"
    "${WLD_ENGINE_ROOT}/third_party/yaml/include"
    "${WLD_ENGINE_ROOT}/third_party/spdlog/include"
    "${WLD_ENGINE_ROOT}/third_party/box2d/src"
    "${WLD_ENGINE_ROOT}/third_party/box2d/include"
)
set_target_properties(World PROPERTIES
    IMPORTED_IMPLIB "${WE_FAST_WORLD_LIB}"
    IMPORTED_LOCATION "${WE_FAST_WORLD_DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${_we_world_include_dirs}"
)

# 根 CMakeLists.txt 的工具目录:PUBLIC 宏。引擎二进制里已固化它自己的值,这里只影响
# 项目自己调用着色器编译的代码;默认值与引擎根 CMakeLists 保持一致 —— 指向仓库内的
# CLI 子集 vendor/tools/slang/bin(引擎升级 Slang 版本时同步这一行),WLD_SLANG_DIR 环境变量优先。
if(DEFINED ENV{WLD_SLANG_DIR} AND NOT "$ENV{WLD_SLANG_DIR}" STREQUAL "")
    set(_we_slang_dir "$ENV{WLD_SLANG_DIR}")
else()
    set(_we_slang_dir "${WLD_ENGINE_ROOT}/vendor/tools/slang/bin")
endif()
file(TO_CMAKE_PATH "${_we_slang_dir}" _we_slang_dir)

set(_we_world_public_defines
    "WLD_BUILD_TYPE=\"${CMAKE_BUILD_TYPE}/\""
    "WLD_REPO_ROOT=\"${WLD_ENGINE_ROOT}\""
    "WLD_EDITOR_DIR=\"${WLD_ENGINE_ROOT}/Editor/\""
    "WLD_LOCAL_DIR=\"${WLD_ENGINE_ROOT}/local/\""
    "WLD_GAME_DIR=\"${WLD_ENGINE_ROOT}/Game/\""
    "WLD_PROJECT_DIR=\"${WLD_ENGINE_ROOT}/projects/\""
    "WLD_ASSETPATH=\"${WLD_ENGINE_ROOT}/projects/assets\""
    "WLD_TEST_ASSETPATH=\"${WLD_ENGINE_ROOT}/tests/fixtures/content\""
    "WLD_OUTPUT_DIR=\"build/x64-${CMAKE_BUILD_TYPE}/\""
    "WLD_WORLD_DIR=\"${WLD_ENGINE_ROOT}/Engine/\""
    "WLD_SLANG_DIR=\"${_we_slang_dir}\""
    "WLD_CORE_DIR=\"${WLD_ENGINE_ROOT}/Engine/\""
    "WLD_ROOT_DIR=\"${WLD_ENGINE_ROOT}/\""
    "WLD_INTERMEDIATE_DIR=\"${CMAKE_BINARY_DIR}/intermediate/\""
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _we_world_public_defines "WLD_DEBUG")
endif()
target_compile_definitions(World INTERFACE ${_we_world_public_defines})

# Game 模块源码根 = <项目根>/src(与引擎 Game/CMakeLists.txt 的项目模式同一口径)。
set(WLD_GAME_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(NOT IS_DIRECTORY "${WLD_GAME_SRC_DIR}")
    message(FATAL_ERROR "game project has no src/ directory: ${WLD_GAME_SRC_DIR} "
        "(项目层 C++ 按 docs/dev/project-standard.md 放在 <项目根>/src/**)")
endif()

# schema 输入发现:配置期只决定"零声明项目编引擎自带空模块注册"这一件事;
# 真正的输入列表由构建期脚本(Game/cmake/game-schema-build.cmake)每次构建 glob。
file(GLOB GAME_SCHEMA_DISCOVERY CONFIGURE_DEPENDS
    "${WLD_GAME_SRC_DIR}/Components/*.h"
    "${WLD_GAME_SRC_DIR}/Scripts/*.h"
)
set(GAME_SCHEMA_ENGINE_SKELETON FALSE)
if(NOT GAME_SCHEMA_DISCOVERY)
    set(GAME_SCHEMA_ENGINE_SKELETON TRUE)
    message(STATUS "WorldEngine Game module: project declares no schema types; "
        "compiling the engine's empty-module registration")
endif()

set(GAME_SCHEMA_SRC_DIR "${WLD_GAME_SRC_DIR}/Generated")
set(GAME_SCHEMA_MANIFEST "${GAME_SCHEMA_SRC_DIR}/Game.manifest")
set(GAME_SCHEMA_GEN_DIR "${CMAKE_BINARY_DIR}/game-schema")
set(GAME_SCHEMA_STAMP "${GAME_SCHEMA_GEN_DIR}/game-schema.stamp")
set(GAME_SCHEMA_GENERATED_HEADER "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.h")
set(GAME_SCHEMA_GENERATED_SOURCE "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.cpp")
set(GAME_SCHEMA_ENGINE_SKELETON_SOURCE "${WLD_ENGINE_ROOT}/Game/src/Generated/Game/GameSchemaRegistration.cpp")

if(GAME_SCHEMA_ENGINE_SKELETON)
    add_custom_target(GameSchema)
else()
    # 与引擎 Game/CMakeLists.txt 同一套构建期脚本/增量门;这里把 schema-compiler 从
    # "本构建里编译出来的 target"换成"引擎构建目录里预编译的 exe"(快路径的全部区别)。
    add_custom_target(GameSchema
        COMMAND ${CMAKE_COMMAND}
            "-DWLD_GAME_SRC_DIR=${WLD_GAME_SRC_DIR}"
            "-DWLD_GAME_PROJECT_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DWLD_GAME_SCHEMA_COMPILER=${WE_FAST_SCHEMA_COMPILER}"
            "-DWLD_GAME_SCHEMA_GEN_DIR=${GAME_SCHEMA_GEN_DIR}"
            "-DWLD_GAME_SCHEMA_STAMP=${GAME_SCHEMA_STAMP}"
            "-DWLD_GAME_SCHEMA_MANIFEST=${GAME_SCHEMA_MANIFEST}"
            "-DWLD_GAME_SCHEMA_HEADER=${GAME_SCHEMA_GENERATED_HEADER}"
            "-DWLD_GAME_SCHEMA_SOURCE=${GAME_SCHEMA_GENERATED_SOURCE}"
            "-DWLD_GAME_SCHEMA_ENGINE_API=${WLD_ENGINE_ROOT}/Game/src/GameAPI.h"
            "-DWLD_ENGINE_ROOT=${WLD_ENGINE_ROOT}"
            -P "${WLD_ENGINE_ROOT}/Game/cmake/game-schema-build.cmake"
        WORKING_DIRECTORY "${WLD_ENGINE_ROOT}"
        COMMENT "Game schema: build-time input discovery + manifest sync (prebuilt schema-compiler, ${WLD_GAME_SRC_DIR})"
        VERBATIM
    )
endif()

add_library(Game SHARED)
set_target_properties(Game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/Game"
)

if(WIN32)
    target_compile_definitions(Game PRIVATE WLD_PLATFORM_WINDOWS)
    if(MSVC)
        # /bigobj:项目注册单元(GameSchemaRegistration.cpp)随 schema 类型增长会撞上 MSVC 的
        # 每 .obj 节数上限(C1128);引擎的 World 目标同样带这个开关。
        target_compile_options(Game PRIVATE "/utf-8" "/bigobj")
    endif()
endif()

target_compile_definitions(Game PRIVATE
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    WLD_CURRENT_DIR="${WLD_ENGINE_ROOT}/Game/"
    GAME_BUILD_DLL
)

# 模块骨架(WeGameModuleQuery / GameLayer)永远来自引擎;其余源码来自 <项目根>/src。
set(GAME_SKELETON_SOURCES
    "${WLD_ENGINE_ROOT}/Game/src/GameAPI.cpp"
    "${WLD_ENGINE_ROOT}/Game/src/GameLayer.cpp"
)
file(GLOB_RECURSE GAME_MODULE_SOURCES CONFIGURE_DEPENDS
    "${WLD_GAME_SRC_DIR}/*.h"
    "${WLD_GAME_SRC_DIR}/*.cpp"
)
foreach(_skeleton IN LISTS GAME_SKELETON_SOURCES)
    list(REMOVE_ITEM GAME_MODULE_SOURCES "${_skeleton}")
endforeach()
# src/Tools/ 是项目自己的工具程序(ProjectRun 的启动器源码),不属于 Game.dll。
list(REMOVE_ITEM GAME_MODULE_SOURCES "${WE_PROJECT_RUNNER_SOURCE}")
list(REMOVE_ITEM GAME_MODULE_SOURCES "${GAME_SCHEMA_GENERATED_SOURCE}")
list(REMOVE_ITEM GAME_MODULE_SOURCES "${GAME_SCHEMA_ENGINE_SKELETON_SOURCE}")

if(GAME_SCHEMA_ENGINE_SKELETON)
    set(GAME_SCHEMA_ACTIVE_SOURCE "${GAME_SCHEMA_ENGINE_SKELETON_SOURCE}")
else()
    set_source_files_properties("${GAME_SCHEMA_GENERATED_SOURCE}" PROPERTIES GENERATED TRUE)
    set(GAME_SCHEMA_ACTIVE_SOURCE "${GAME_SCHEMA_GENERATED_SOURCE}")
endif()

target_sources(Game PRIVATE
    ${GAME_SKELETON_SOURCES}
    ${GAME_MODULE_SOURCES}
    "${GAME_SCHEMA_ACTIVE_SOURCE}"
)
target_include_directories(Game PRIVATE
    "${WLD_ENGINE_ROOT}/Game/src"
    "${WLD_GAME_SRC_DIR}"
)
add_dependencies(Game GameSchema)

# 快路径下 World 是导入目标:只提供 include/宏与导入库,二进制在引擎构建目录里。
target_link_libraries(Game PRIVATE World)

add_custom_command(TARGET Game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WE_FAST_WORLD_DLL}"
        "$<TARGET_FILE_DIR:Game>"
    COMMENT "Copying WorldRuntime.dll next to Game.dll (from ${WE_FAST_WORLD_DLL})"
    VERBATIM
)

# 引擎 PDB:VS 里调试引擎代码(WorldRuntime.dll)要靠它;和 DLL 一起复制到产物目录。
# 引擎构建目录里确实没有这份 PDB(例如只装了运行时产物)时**不阻断构建**,只警告一行。
if(EXISTS "${WE_FAST_WORLD_PDB}")
    add_custom_command(TARGET Game POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WE_FAST_WORLD_PDB}"
            "$<TARGET_FILE_DIR:Game>"
        COMMENT "Copying WorldRuntime.pdb next to Game.dll (engine debug symbols, from ${WE_FAST_WORLD_PDB})"
        VERBATIM
    )
else()
    message(WARNING
        "WorldEngine engine PDB is missing: ${WE_FAST_WORLD_PDB}\n"
        "引擎 PDB 缺失，调试符号不可用 —— Game.dll 与 WorldRuntime.dll 仍会正常构建,"
        "但在 VS 里调试引擎代码时只有地址没有符号;用 Debug 配置重新构建引擎即可补上。")
endif()

# ---- 4. 项目启动器目标(ProjectRun)--------------------------------------------
_we_add_project_runner(ProjectRun)
