add_library(WePlugin_we-gen SHARED
    src/GenPlugin.cpp
    src/CredentialManager.cpp
    src/StyleLock.cpp
    src/BudgetGuard.cpp
    src/GenPipeline.cpp
)

set_target_properties(WePlugin_we-gen PROPERTIES
    PREFIX ""
    OUTPUT_NAME "we-gen"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
)

target_compile_features(WePlugin_we-gen PRIVATE cxx_std_17)
target_include_directories(WePlugin_we-gen PRIVATE
    "${WLD_ENGINE_ROOT}/Engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(WePlugin_we-gen PRIVATE
    World
    yaml-cpp
    crypt32
)
target_compile_definitions(WePlugin_we-gen PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WePlugin_we-gen PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WePlugin_we-gen PRIVATE /utf-8)
endif()
