add_executable(WorldScriptTests ScriptLifecycleTests.cpp)
target_link_libraries(WorldScriptTests PRIVATE World)
target_compile_features(WorldScriptTests PRIVATE cxx_std_17)
target_compile_definitions(WorldScriptTests PRIVATE
    WORLD_SCRIPT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldScriptTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldScriptTests PRIVATE /utf-8)
endif()

# Deliberately no /WHOLEARCHIVE: the real ScriptEngine initialization must retain
# Entity and matrix bindings in an ordinary static-library consumer.
add_test(NAME World.ScriptLifecycle COMMAND WorldScriptTests)
set_tests_properties(World.ScriptLifecycle PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# Schema 2.0 核心回归:注册表、稳定身份、生成访问器、事务化模块注册。
add_executable(WorldSchemaTests
    SchemaTests.cpp
    schema/Generated/TestKit/TestKitSchemaRegistration.cpp
)
target_include_directories(WorldSchemaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(WorldSchemaTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
target_link_libraries(WorldSchemaTests PRIVATE World)
target_compile_features(WorldSchemaTests PRIVATE cxx_std_17)
if(WIN32)
    target_compile_definitions(WorldSchemaTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldSchemaTests PRIVATE /utf-8)
endif()

add_test(NAME World.Schema COMMAND WorldSchemaTests)
set_tests_properties(World.Schema PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# WUI Core 纯逻辑回归:布局/样式/命中/焦点(无 GL)。
add_executable(WorldWuiTests WuiTests.cpp)
target_link_libraries(WorldWuiTests PRIVATE World)
target_compile_features(WorldWuiTests PRIVATE cxx_std_17)
target_compile_definitions(WorldWuiTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldWuiTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldWuiTests PRIVATE /utf-8)
endif()

add_test(NAME World.Wui COMMAND WorldWuiTests)
set_tests_properties(World.Wui PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)
