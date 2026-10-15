add_library(WePlugin_we-studio SHARED
    src/StudioPlugin.cpp
    src/StudioPolicy.cpp
    src/BacklogManager.cpp
    src/StudioValidator.cpp
    src/AgentsDocGenerator.cpp
)

set_target_properties(WePlugin_we-studio PROPERTIES
    PREFIX ""
    OUTPUT_NAME "we-studio"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
)

target_compile_features(WePlugin_we-studio PRIVATE cxx_std_17)
target_include_directories(WePlugin_we-studio PRIVATE
    "${WLD_ENGINE_ROOT}/Engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(WePlugin_we-studio PRIVATE
    World
    yaml-cpp
)
target_compile_definitions(WePlugin_we-studio PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WePlugin_we-studio PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WePlugin_we-studio PRIVATE /utf-8)
endif()
