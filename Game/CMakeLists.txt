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

file(GLOB_RECURSE Game_SOURCES 
    "src/*.h"
    "src/*.cpp"
)
target_sources(Game PRIVATE ${Game_SOURCES})

# Game 层依然依赖引擎核心
target_link_libraries(Game PRIVATE 
    World
)