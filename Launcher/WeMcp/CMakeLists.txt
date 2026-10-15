add_executable(WeMcp
    src/main.cpp
    src/EngineBridge.cpp
    src/McpServer.cpp
)

add_executable(we-mcp ALIAS WeMcp)

target_link_libraries(WeMcp PRIVATE
    World
    ws2_32
)

target_compile_features(WeMcp PRIVATE cxx_std_17)
target_compile_definitions(WeMcp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WLD_REPO_ROOT="${CMAKE_SOURCE_DIR}"
    WLD_OUTPUT_DIR="build/x64-${CMAKE_BUILD_TYPE}/"
    WLD_BUILD_TYPE="${CMAKE_BUILD_TYPE}/"
)

if(WIN32)
    target_compile_definitions(WeMcp PRIVATE WLD_PLATFORM_WINDOWS)
endif()

if(MSVC)
    target_compile_options(WeMcp PRIVATE /utf-8)
endif()

set_target_properties(WeMcp PROPERTIES
    OUTPUT_NAME "WeMcp"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Launcher"
)

# 1. 复制 WorldRuntime.dll 到 WeMcp.exe 所在目录，保证随处可执行
add_custom_command(TARGET WeMcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:World>"
        "$<TARGET_FILE_DIR:WeMcp>"
    COMMENT "Copying WorldRuntime.dll next to WeMcp.exe"
    VERBATIM
)

# 2. 兼容生成 we-mcp.exe 产物副本，确保新旧路径与外部工具配置无缝兼容
add_custom_command(TARGET WeMcp POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:WeMcp>"
        "$<TARGET_FILE_DIR:WeMcp>/we-mcp$<TARGET_FILE_SUFFIX:WeMcp>"
    COMMENT "Mirroring WeMcp.exe to we-mcp.exe"
    VERBATIM
)
