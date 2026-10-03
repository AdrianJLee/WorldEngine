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

# 与 Engine 的"一个 target 一个 include 根"同口径:Editor 内部头文件按
# Editor/src 下的规范路径引用(如 "WUI/Panels/X.h"、"App/EditorLayer.h"),
# 不再依赖 include 时相对当前文件的 ../ 形式(2026-10-03 源码分类整理)。
target_include_directories(Editor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(Editor PRIVATE 
    World
    # AI 控制通道(TCP 回环,JSON 行协议):winsock2。
    ws2_32
    # 2026-09-30:复用已打开的 VS 实例(Running Object Table + DTE 晚期绑定)。
    ole32
)

add_custom_command(TARGET Editor POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:World>"
        "$<TARGET_FILE_DIR:Editor>"
    COMMENT "Copying WorldRuntime.dll next to Editor.exe"
    VERBATIM
)
