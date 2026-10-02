# PLUG-T2 示例插件的构建。**这是测试夹具,不是随引擎分发的插件**:
# 它原先住在 <引擎根>/plugins/hello-import,会被根 CMakeLists 的 plugins/** 收集、
# 也会被编辑器的插件发现扫到 ⇒ 每个编辑器会话都会加载它,右键「新建」里就多一条
# "Hello File"(用户 2026-10-02 报的噪音)。示例插件的价值是覆盖测试,所以搬到这里:
# 由 tests/CMakeLists.txt 显式 add_subdirectory 加进来,发布/编辑器都看不到它。
# 面向用户的"资产类型插件"蓝本是 templates/plugin-asset-type/**。
#
# 边界:夹具插件**不链接 World** —— 只依赖公共 ABI 头 WePluginApi.h,和真实第三方插件
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
