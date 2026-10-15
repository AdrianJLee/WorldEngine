add_library(WePlugin_we-ai-assistant SHARED
    src/AssistantPlugin.cpp
    src/InterviewSystem.cpp
    src/AssistantEngine.cpp
)

set_target_properties(WePlugin_we-ai-assistant PROPERTIES
    PREFIX ""
    OUTPUT_NAME "we-ai-assistant"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
)

target_compile_features(WePlugin_we-ai-assistant PRIVATE cxx_std_17)
target_include_directories(WePlugin_we-ai-assistant PRIVATE
    "${WLD_ENGINE_ROOT}/Engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(WePlugin_we-ai-assistant PRIVATE
    World
)
target_compile_definitions(WePlugin_we-ai-assistant PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WePlugin_we-ai-assistant PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WePlugin_we-ai-assistant PRIVATE /utf-8)
endif()
