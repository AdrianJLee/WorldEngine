add_executable(we-mcp
    src/main.cpp
    src/EngineBridge.cpp
    src/McpServer.cpp
)

target_link_libraries(we-mcp PRIVATE
    World
    ws2_32
)

target_compile_features(we-mcp PRIVATE cxx_std_17)
target_compile_definitions(we-mcp PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)

if(WIN32)
    target_compile_definitions(we-mcp PRIVATE WLD_PLATFORM_WINDOWS)
endif()

if(MSVC)
    target_compile_options(we-mcp PRIVATE /utf-8)
endif()
