project(Game)
set(OUTPUT_DIR "bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${OUTPUT_DIR}/Game")

# ============================================================================
# PROJ-8/T2:Game 模块的**用户源码**可以来自一个游戏项目(<项目根>/src/**)。
#   * WLD_GAME_PROJECT_DIR 为空(默认)= 引擎自身构建:Game/src 只留模块骨架
#     (GameAPI / GameLayer + 生成式 schema 注册),schema 退化成"没有自定义类型的模块";
#   * 非空 = 项目构建(项目自带 CMakeLists 用 add_subdirectory(<引擎根>) 引用引擎时传进来):
#     项目的 src/** 编进 Game.dll,GameSchema 的输入发现 / 生成物 / 类型账本全部落在项目里
#     (<项目根>/src/Generated/**)。
# ============================================================================
if(NOT DEFINED WLD_GAME_PROJECT_DIR)
    set(WLD_GAME_PROJECT_DIR "" CACHE PATH
        "Game project root whose src/** is compiled into Game.dll (empty = engine skeleton)")
endif()

if(WLD_GAME_PROJECT_DIR)
    if(NOT IS_ABSOLUTE "${WLD_GAME_PROJECT_DIR}")
        get_filename_component(WLD_GAME_PROJECT_DIR "${WLD_GAME_PROJECT_DIR}" ABSOLUTE)
    endif()
    if(NOT IS_DIRECTORY "${WLD_GAME_PROJECT_DIR}")
        message(FATAL_ERROR "WLD_GAME_PROJECT_DIR is not a directory: ${WLD_GAME_PROJECT_DIR}")
    endif()
    set(WLD_GAME_SRC_DIR "${WLD_GAME_PROJECT_DIR}/src")
    if(NOT IS_DIRECTORY "${WLD_GAME_SRC_DIR}")
        message(FATAL_ERROR "game project has no src/ directory: ${WLD_GAME_SRC_DIR} "
            "(项目层 C++ 按 docs/dev/project-standard.md 放在 <项目根>/src/**)")
    endif()
    message(STATUS "WorldEngine Game module: project sources = ${WLD_GAME_SRC_DIR}")
else()
    set(WLD_GAME_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src")
    message(STATUS "WorldEngine Game module: engine skeleton only (WLD_GAME_PROJECT_DIR is empty)")
endif()

# ---- PURE-ECS:项目层系统挂载入口的构建期探测 ---------------------------------
# 项目用 C++ 写系统时,在 `<项目根>/src/GameProject.cpp` 里实现
# `World::Game::AttachProjectSystems / DetachProjectSystems`(见 GameAPI.h)。
# 这个文件是**可选**的:缺席时 GameAPI.cpp 编一份空实现(宏在下面传给编译器),
# 老项目/零系统项目不会因为少一个文件而链接失败。
set(WLD_GAME_HOOK_DEFINE "WLD_GAME_NO_PROJECT_HOOK=1")
if(WLD_GAME_PROJECT_DIR AND EXISTS "${WLD_GAME_SRC_DIR}/GameProject.cpp")
    set(WLD_GAME_HOOK_DEFINE "")
    message(STATUS "WorldEngine Game module: project systems hook = ${WLD_GAME_SRC_DIR}/GameProject.cpp")
else()
    message(STATUS "WorldEngine Game module: no src/GameProject.cpp; compiling the empty systems hook")
endif()

# 引擎自带的**空模块**注册(committed 生成物):零 schema 声明的项目直接编这一份,
# 不在项目里制造任何生成物,也不会把机器相关路径写进项目文件。
set(GAME_SCHEMA_ENGINE_SKELETON_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Generated/Game/GameSchemaRegistration.cpp")

# ---- Game schema 生成:输入自动发现(CPPT-6)+ 项目化(PROJ-8/T2)----
# 输入 = <Game 源码根>/Components/*.h,**由构建期脚本在构建时 glob**
# (Game/cmake/game-schema-build.cmake,每次构建跑一次)。
# 为什么不在配置期:配置期 glob 会把"配置时还在、现在已删"的头文件留在 --input /
# --reg-include 里 ⇒ 删一个脚本头后的第一次构建 `schema-compiler: cannot open input file`
# + MSB8066(CPPT-6 残项 / CPPT-7 T2-C1)。构建期脚本同时做账本双向同步与内容哈希门:
# 输入没变就一个文件都不写(生成物 mtime 不动),变了才重跑 + copy_if_different 回写。
#
# 配置期只剩一个决定:零声明的**项目**构建编引擎自带空模块注册(不在项目里制造生成物)。
# 这里仍用 CONFIGURE_DEPENDS glob,但结果只喂给这个判断,不进编译器的命令行。
file(GLOB GAME_SCHEMA_DISCOVERY CONFIGURE_DEPENDS
    "${WLD_GAME_SRC_DIR}/Components/*.h"
)
set(GAME_SCHEMA_ENGINE_SKELETON FALSE)
if(NOT GAME_SCHEMA_DISCOVERY AND WLD_GAME_PROJECT_DIR)
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

if(NOT GAME_SCHEMA_ENGINE_SKELETON)
    # 每次构建都跑:脚本自己看 glob + 账本 + 内容哈希决定要不要重跑 schema-compiler。
    # DEPENDS 里**只有** schema-compiler 这个目标,没有"可能被删"的头文件 ——
    # 头文件列表的事实源是构建期的那次 glob。
    add_custom_target(GameSchema
        COMMAND ${CMAKE_COMMAND}
            "-DWLD_GAME_SRC_DIR=${WLD_GAME_SRC_DIR}"
            "-DWLD_GAME_PROJECT_DIR=${WLD_GAME_PROJECT_DIR}"
            "-DWLD_GAME_SCHEMA_COMPILER=$<TARGET_FILE:schema-compiler>"
            "-DWLD_GAME_SCHEMA_GEN_DIR=${GAME_SCHEMA_GEN_DIR}"
            "-DWLD_GAME_SCHEMA_STAMP=${GAME_SCHEMA_STAMP}"
            "-DWLD_GAME_SCHEMA_MANIFEST=${GAME_SCHEMA_MANIFEST}"
            "-DWLD_GAME_SCHEMA_HEADER=${GAME_SCHEMA_GENERATED_HEADER}"
            "-DWLD_GAME_SCHEMA_SOURCE=${GAME_SCHEMA_GENERATED_SOURCE}"
            "-DWLD_GAME_SCHEMA_ENGINE_API=${CMAKE_CURRENT_SOURCE_DIR}/src/GameAPI.h"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/game-schema-build.cmake"
        WORKING_DIRECTORY "${WLD_ENGINE_ROOT}"
        COMMENT "Game schema: build-time input discovery + manifest sync (${WLD_GAME_SRC_DIR})"
        VERBATIM
    )
    add_dependencies(GameSchema schema-compiler)
else()
    # 目标名保持存在:`cmake --build <dir> --target GameSchema` 在零声明项目里是 no-op。
    add_custom_target(GameSchema)
endif()

# 1. 从 add_executable 修改为 add_library 并指定为 SHARED
add_library(Game SHARED)

if(WIN32)
    target_compile_definitions(Game PRIVATE
        WLD_PLATFORM_WINDOWS
    )
    if(MSVC)
        target_compile_options(Game PRIVATE "/utf-8")
    endif()
endif()

# 2. 增加宏 GAME_BUILD_DLL 用于区分编译期是导出还是导入
target_compile_definitions(Game PRIVATE
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    WLD_CURRENT_DIR="${WLD_ENGINE_ROOT}/Game/"
    GAME_BUILD_DLL
    ${WLD_GAME_HOOK_DEFINE}
)

# 模块骨架(WeGameModuleQuery / GameLayer)永远来自引擎;其余源码来自 <Game 源码根>。
set(GAME_SKELETON_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GameAPI.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/GameLayer.cpp"
)
file(GLOB_RECURSE GAME_MODULE_SOURCES CONFIGURE_DEPENDS
    "${WLD_GAME_SRC_DIR}/*.h"
    "${WLD_GAME_SRC_DIR}/*.cpp"
)
foreach(_skeleton IN LISTS GAME_SKELETON_SOURCES)
    list(REMOVE_ITEM GAME_MODULE_SOURCES "${_skeleton}")
endforeach()
list(REMOVE_ITEM GAME_MODULE_SOURCES "${GAME_SCHEMA_GENERATED_SOURCE}")
list(REMOVE_ITEM GAME_MODULE_SOURCES "${GAME_SCHEMA_ENGINE_SKELETON_SOURCE}")

if(GAME_SCHEMA_ENGINE_SKELETON)
    set(GAME_SCHEMA_ACTIVE_SOURCE "${GAME_SCHEMA_ENGINE_SKELETON_SOURCE}")
else()
    # 生成物必须**显式**进 target:项目首次配置时它还不存在,glob 抓不到;标 GENERATED 后由上面的
    # add_custom_command 产出(GameSchema 是 Game 的构建依赖 ⇒ 先生成、再编译/链接)。
    set_source_files_properties("${GAME_SCHEMA_GENERATED_SOURCE}" PROPERTIES GENERATED TRUE)
    set(GAME_SCHEMA_ACTIVE_SOURCE "${GAME_SCHEMA_GENERATED_SOURCE}")
endif()

target_sources(Game PRIVATE
    ${GAME_SKELETON_SOURCES}
    ${GAME_MODULE_SOURCES}
    "${GAME_SCHEMA_ACTIVE_SOURCE}"
)
target_include_directories(Game PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${WLD_GAME_SRC_DIR}"
)

add_dependencies(Game GameSchema)

# Game 层依然依赖引擎核心
target_link_libraries(Game PRIVATE
    World
)

add_custom_command(TARGET Game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:World>"
        "$<TARGET_FILE_DIR:Game>"
    COMMENT "Copying WorldRuntime.dll next to Game.dll"
    VERBATIM
)
