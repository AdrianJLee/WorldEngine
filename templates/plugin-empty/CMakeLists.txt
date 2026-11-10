# {{PluginName}} — 插件包构建(由引擎/项目的插件收集加入;{{PluginScope}} 形态)。
#
# 边界:插件**不链接 World** —— 只依赖公共 ABI 头 WePluginApi.h,和第三方插件的边界一致
# (宿主通过 DynamicLibrary 取 WePluginQuery 符号)。
# 产物落构建树 bin/<配置>/plugins(多配置生成器会再追加一层 <配置>/),
# 发现期按开发产物根找到它。
add_library(WePlugin_{{PluginDir}} SHARED src/plugin.cpp)

set_target_properties(WePlugin_{{PluginDir}} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "{{PluginDir}}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
)

target_compile_features(WePlugin_{{PluginDir}} PRIVATE cxx_std_17)
target_include_directories(WePlugin_{{PluginDir}} PRIVATE "${WLD_ENGINE_ROOT}/Engine/src")
target_compile_definitions(WePlugin_{{PluginDir}} PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WePlugin_{{PluginDir}} PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WePlugin_{{PluginDir}} PRIVATE /utf-8)
endif()
