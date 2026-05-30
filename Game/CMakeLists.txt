project(Game)
# 将编辑器的工作目录设置为源代码目录，以便在调试时能够正确加载资源文件
set(CMAKE_VS_DEBUGGER_WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
add_executable(Game)

if(WIN32)
    target_compile_definitions(Game PRIVATE
        WLD_PLATFORM_WINDOWS
    )

    if(MSVC)
        target_compile_options(Game PRIVATE "/utf-8")
    endif()

endif()


target_compile_definitions(Game PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/Game/"
)



file(GLOB_RECURSE Game_SOURCES 
    "src/*.h"
    "src/*.cpp"
)
target_sources(Game PRIVATE ${Game_SOURCES})

target_link_libraries(Game PRIVATE 
    World
)
