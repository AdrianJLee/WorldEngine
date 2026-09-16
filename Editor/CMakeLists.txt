project(Editor)

add_executable(Editor)

if(WIN32)
    target_compile_definitions(Editor PRIVATE
        WLD_PLATFORM_WINDOWS
    )

    if(MSVC)
        target_compile_options(Editor PRIVATE "/utf-8")
    endif()

endif()


target_compile_definitions(Editor PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/Editor/"
)



file(GLOB_RECURSE EDITOR_SOURCES 
    "src/*.h"
    "src/*.cpp"
)
target_sources(Editor PRIVATE ${EDITOR_SOURCES})

target_link_libraries(Editor PRIVATE 
    World
    # AI 控制通道(TCP 回环,JSON 行协议):winsock2。
    ws2_32
)

add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:World>"
        "$<TARGET_FILE_DIR:Editor>"
    COMMENT "Copying WorldRuntime.dll next to Editor.exe"
    VERBATIM
)
