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

# 引擎自带的**空模块**注册(committed 生成物):零 schema 声明的项目直接编这一份,
# 不在项目里制造任何生成物,也不会把机器相关路径写进项目文件。
set(GAME_SCHEMA_ENGINE_SKELETON_SOURCE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Generated/Game/GameSchemaRegistration.cpp")

# ---- Game schema 生成:输入自动发现(CPPT-6)+ 项目化(PROJ-8/T2)----
# 输入 = <Game 源码根>/{Components,Scripts}/*.h。没有任何 schema 声明时:
#   * 引擎自身构建 ⇒ 用 GameAPI.h 当唯一输入(零声明 = 空模块注册;生成物回写入库);
#   * 项目构建     ⇒ 完全不生成,直接编引擎自带的空模块注册。
file(GLOB GAME_SCHEMA_INPUTS CONFIGURE_DEPENDS
    "${WLD_GAME_SRC_DIR}/Components/*.h"
    "${WLD_GAME_SRC_DIR}/Scripts/*.h"
)
set(GAME_SCHEMA_ENGINE_SKELETON FALSE)
if(NOT GAME_SCHEMA_INPUTS)
    if(WLD_GAME_PROJECT_DIR)
        set(GAME_SCHEMA_ENGINE_SKELETON TRUE)
        message(STATUS "WorldEngine Game module: project declares no schema types; "
            "compiling the engine's empty-module registration")
    else()
        set(GAME_SCHEMA_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/src/GameAPI.h")
    endif()
endif()

set(GAME_SCHEMA_SRC_DIR "${WLD_GAME_SRC_DIR}/Generated")
set(GAME_SCHEMA_MANIFEST "${GAME_SCHEMA_SRC_DIR}/Game.manifest")
set(GAME_SCHEMA_GEN_DIR "${CMAKE_BINARY_DIR}/game-schema")
set(GAME_SCHEMA_STAMP "${GAME_SCHEMA_GEN_DIR}/game-schema.stamp")
set(GAME_SCHEMA_GENERATED_HEADER "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.h")
set(GAME_SCHEMA_GENERATED_SOURCE "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.cpp")

if(NOT GAME_SCHEMA_ENGINE_SKELETON)
    # 类型账本是**双向清单**(schema-compiler 两个方向都拒:"声明未登记" / "登记未声明")。
    # 首次配置时它还不存在(项目刚放进一批 C++、或模板刚建出的项目)⇒ 从源码里的
    # WE_SCHEMA_BODY / WE_ENUM_SCHEMA 声明派生一份;此后以账本为准(编辑器 "New C++ Script…"
    # 也在同一路径追加登记)。命名空间按项目层约定取 World::(与编辑器生成的模板一致)。
    if(NOT EXISTS "${GAME_SCHEMA_MANIFEST}")
        set(_wld_schema_entries "")
        foreach(_input IN LISTS GAME_SCHEMA_INPUTS)
            file(READ "${_input}" _wld_schema_text)
            string(REPLACE "\r\n" "\n" _wld_schema_text "${_wld_schema_text}")
            string(REPLACE "\n" ";" _wld_schema_lines "${_wld_schema_text}")
            foreach(_line IN LISTS _wld_schema_lines)
                # 注释行里的示意写法(例:`WE_SCHEMA_BODY(..., Struct)` 的说明)不算声明。
                string(STRIP "${_line}" _stripped)
                if(_stripped MATCHES "^//" OR _stripped MATCHES "^#")
                    continue()
                endif()
                string(REGEX MATCHALL
                    "WE_SCHEMA_BODY[ \t]*\\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*[A-Za-z_][A-Za-z0-9_]*"
                    _wld_struct_hits "${_line}")
                foreach(_hit IN LISTS _wld_struct_hits)
                    string(REGEX REPLACE "^.*,[ \t]*" "" _wld_type "${_hit}")
                    list(APPEND _wld_schema_entries "struct World::${_wld_type}")
                endforeach()
                string(REGEX MATCHALL
                    "WE_ENUM_SCHEMA[ \t]*\\([ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*,[ \t]*[A-Za-z_][A-Za-z0-9_]*"
                    _wld_enum_hits "${_line}")
                foreach(_hit IN LISTS _wld_enum_hits)
                    string(REGEX REPLACE "^.*,[ \t]*" "" _wld_type "${_hit}")
                    list(APPEND _wld_schema_entries "enum World::${_wld_type}")
                endforeach()
            endforeach()
        endforeach()
        if(_wld_schema_entries)
            list(REMOVE_DUPLICATES _wld_schema_entries)
            list(SORT _wld_schema_entries)
            string(REPLACE ";" "\n" _wld_schema_body "${_wld_schema_entries}")
        else()
            set(_wld_schema_body "")
        endif()
        file(MAKE_DIRECTORY "${GAME_SCHEMA_SRC_DIR}")
        file(WRITE "${GAME_SCHEMA_MANIFEST}"
            "# Game 模块类型账本(双向清单):每行 \"struct <限定名>\" 或 \"enum <限定名>\",# 之后是注释。\n"
            "# 首次配置时由 Game/CMakeLists.txt 从源码声明派生;此后由人和编辑器维护。\n"
            "${_wld_schema_body}")
        message(STATUS "WorldEngine Game schema: seeded ${GAME_SCHEMA_MANIFEST}")
    endif()

    set(GAME_SCHEMA_ARGS "")
    foreach(_input IN LISTS GAME_SCHEMA_INPUTS)
        list(APPEND GAME_SCHEMA_ARGS --input "${_input}")
    endforeach()
    foreach(_input IN LISTS GAME_SCHEMA_INPUTS)
        file(RELATIVE_PATH _relative "${WLD_GAME_SRC_DIR}" "${_input}")
        list(APPEND GAME_SCHEMA_ARGS --reg-include "${_relative}")
    endforeach()

    add_custom_command(OUTPUT "${GAME_SCHEMA_STAMP}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_SCHEMA_GEN_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAME_SCHEMA_MANIFEST}" "${GAME_SCHEMA_GEN_DIR}/Game.manifest"
        COMMAND $<TARGET_FILE:schema-compiler> ${GAME_SCHEMA_ARGS} --module Game
                --output "${GAME_SCHEMA_GEN_DIR}" --manifest "${GAME_SCHEMA_GEN_DIR}/Game.manifest"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_SCHEMA_SRC_DIR}/Game"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAME_SCHEMA_GEN_DIR}/Game/GameSchemaRegistration.h"
                "${GAME_SCHEMA_GENERATED_HEADER}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${GAME_SCHEMA_GEN_DIR}/Game/GameSchemaRegistration.cpp"
                "${GAME_SCHEMA_GENERATED_SOURCE}"
        COMMAND ${CMAKE_COMMAND} -E touch "${GAME_SCHEMA_STAMP}"
        DEPENDS ${GAME_SCHEMA_INPUTS} "${GAME_SCHEMA_MANIFEST}" schema-compiler
        WORKING_DIRECTORY "${WLD_ENGINE_ROOT}"
        COMMENT "Game schema: regenerate registration from globbed inputs (${WLD_GAME_SRC_DIR})"
        VERBATIM
    )
    add_custom_target(GameSchema DEPENDS "${GAME_SCHEMA_STAMP}")
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
