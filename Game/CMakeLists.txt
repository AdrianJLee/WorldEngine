project(Game)
set(OUTPUT_DIR "bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${OUTPUT_DIR}/Game")

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
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/Game/"
    GAME_BUILD_DLL 
)

file(GLOB_RECURSE Game_SOURCES CONFIGURE_DEPENDS
    "src/*.h"
    "src/*.cpp"
)
target_include_directories(Game PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_sources(Game PRIVATE ${Game_SOURCES})

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

# CPPT-6:Game schema 输入自动发现 —— 新增脚本/组件头文件免改任何清单。
# 生成到构建目录,再用 copy_if_different 回写源码树:内容不变时不动 mtime,避免无谓重编。
file(GLOB GAME_SCHEMA_INPUTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Components/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Scripts/*.h"
)
set(GAME_SCHEMA_ARGS "")
foreach(input ${GAME_SCHEMA_INPUTS})
    list(APPEND GAME_SCHEMA_ARGS --input "${input}")
endforeach()
foreach(input ${GAME_SCHEMA_INPUTS})
    file(RELATIVE_PATH rel "${CMAKE_CURRENT_SOURCE_DIR}/src" "${input}")
    list(APPEND GAME_SCHEMA_ARGS --reg-include "${rel}")
endforeach()
# 让 tests/ 的 Game.SchemaDrift 复用同一份输入清单(父作用域 = 根 CMakeLists)。
set(GAME_SCHEMA_ARGS "${GAME_SCHEMA_ARGS}" PARENT_SCOPE)

set(GAME_SCHEMA_GEN_DIR "${CMAKE_BINARY_DIR}/game-schema")
set(GAME_SCHEMA_STAMP "${GAME_SCHEMA_GEN_DIR}/game-schema.stamp")
set(GAME_SCHEMA_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/Generated")
add_custom_command(OUTPUT "${GAME_SCHEMA_STAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GAME_SCHEMA_GEN_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GAME_SCHEMA_SRC_DIR}/Game.manifest" "${GAME_SCHEMA_GEN_DIR}/Game.manifest"
    COMMAND $<TARGET_FILE:schema-compiler> ${GAME_SCHEMA_ARGS} --module Game
            --output "${GAME_SCHEMA_GEN_DIR}" --manifest "${GAME_SCHEMA_GEN_DIR}/Game.manifest"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GAME_SCHEMA_GEN_DIR}/Game/GameSchemaRegistration.h"
            "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GAME_SCHEMA_GEN_DIR}/Game/GameSchemaRegistration.cpp"
            "${GAME_SCHEMA_SRC_DIR}/Game/GameSchemaRegistration.cpp"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GAME_SCHEMA_GEN_DIR}/Game.manifest" "${GAME_SCHEMA_SRC_DIR}/Game.manifest"
    COMMAND ${CMAKE_COMMAND} -E touch "${GAME_SCHEMA_STAMP}"
    DEPENDS ${GAME_SCHEMA_INPUTS} schema-compiler
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Game schema: regenerate from globbed inputs (copy_if_different)"
    VERBATIM
)
add_custom_target(GameSchema DEPENDS "${GAME_SCHEMA_STAMP}")
add_dependencies(Game GameSchema)
