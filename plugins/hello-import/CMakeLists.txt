# PLUG-T2 引擎插件示例的构建(由根 CMakeLists.txt 的可选 plugins/** 收集加入)。
#
# 边界:示例插件**不链接 World** —— 只依赖公共 ABI 头 WePluginApi.h,和真实第三方插件
# 的边界一致(宿主通过 DynamicLibrary 取 WePluginQuery 符号)。
add_library(WePlugin_hello-import SHARED src/HelloImport.cpp)

set_target_properties(WePlugin_hello-import PROPERTIES
    PREFIX ""
    OUTPUT_NAME "hello-import"
    # 产物落构建树:bin/<cfg>/plugins/hello-import.dll(与方案 §1 的引擎插件产物位置一致;
    # VS 多配置生成器会再追加一层 <config>/ —— 与 Game/Editor 的输出布局同款)。
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}/plugins"
)

target_compile_features(WePlugin_hello-import PRIVATE cxx_std_17)
target_include_directories(WePlugin_hello-import PRIVATE "${WLD_ENGINE_ROOT}/Engine/src")
target_compile_definitions(WePlugin_hello-import PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WePlugin_hello-import PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WePlugin_hello-import PRIVATE /utf-8)
endif()
