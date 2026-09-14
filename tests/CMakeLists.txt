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

# VFS 2.0 核心回归:归一化、目录/包 provider、挂载优先级与包格式校验。
add_executable(WorldVfsTests VfsTests.cpp)
target_link_libraries(WorldVfsTests PRIVATE World)
target_compile_features(WorldVfsTests PRIVATE cxx_std_17)
target_compile_definitions(WorldVfsTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldVfsTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldVfsTests PRIVATE /utf-8)
endif()

add_test(NAME World.Vfs COMMAND WorldVfsTests)
set_tests_properties(World.Vfs PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# 资产管线回归:项目清单、指纹缓存、增量烘焙与打包读回一致性。
add_executable(WorldAssetTests AssetCookTests.cpp)
target_link_libraries(WorldAssetTests PRIVATE World)
target_compile_features(WorldAssetTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAssetTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAssetTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAssetTests PRIVATE /utf-8)
endif()

add_test(NAME World.Asset COMMAND WorldAssetTests)
set_tests_properties(World.Asset PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# RHI 合同头自洽性:全部 POD 描述符默认构造与关键语义(无渲染)。
add_executable(WorldRhiContractTests RhiContractTests.cpp)
target_link_libraries(WorldRhiContractTests PRIVATE World)
target_compile_features(WorldRhiContractTests PRIVATE cxx_std_17)
target_compile_definitions(WorldRhiContractTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldRhiContractTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldRhiContractTests PRIVATE /utf-8)
endif()

add_test(NAME World.RhiContract COMMAND WorldRhiContractTests)
set_tests_properties(World.RhiContract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# Vulkan 设备创建(volk 动态加载驱动;无驱动环境优雅跳过)。
add_executable(WorldVulkanDeviceTests VulkanDeviceTests.cpp)
target_link_libraries(WorldVulkanDeviceTests PRIVATE World)
target_compile_features(WorldVulkanDeviceTests PRIVATE cxx_std_17)
target_compile_definitions(WorldVulkanDeviceTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldVulkanDeviceTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldVulkanDeviceTests PRIVATE /utf-8)
endif()

add_test(NAME World.VulkanDevice COMMAND WorldVulkanDeviceTests)
set_tests_properties(World.VulkanDevice PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# 着色器管线回归:烘焙产物、内容寻址缓存、打包读回与运行时解析器优先级。
add_executable(WorldShaderPipelineTests ShaderPipelineTests.cpp)
target_link_libraries(WorldShaderPipelineTests PRIVATE World)
target_compile_features(WorldShaderPipelineTests PRIVATE cxx_std_17)
target_compile_definitions(WorldShaderPipelineTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldShaderPipelineTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldShaderPipelineTests PRIVATE /utf-8)
endif()

add_test(NAME World.ShaderPipeline COMMAND WorldShaderPipelineTests)
set_tests_properties(World.ShaderPipeline PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# 内存基座回归:每线程帧 arena、析构、追加页、双轨与池分配器。
add_executable(WorldMemoryTests MemoryTests.cpp)
target_link_libraries(WorldMemoryTests PRIVATE World)
target_compile_features(WorldMemoryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldMemoryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldMemoryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldMemoryTests PRIVATE /utf-8)
endif()

add_test(NAME World.Memory COMMAND WorldMemoryTests)
set_tests_properties(World.Memory PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# 渲染后端运行时热切换回归:GL↔VK 往返 ≥3 次,含 SceneRenderer 重建。
add_executable(WorldRendererHotswapTests RendererHotswapTests.cpp)
target_link_libraries(WorldRendererHotswapTests PRIVATE World)
target_compile_features(WorldRendererHotswapTests PRIVATE cxx_std_17)
target_compile_definitions(WorldRendererHotswapTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldRendererHotswapTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldRendererHotswapTests PRIVATE /utf-8)
endif()

add_test(NAME World.RendererHotswap COMMAND WorldRendererHotswapTests)
set_tests_properties(World.RendererHotswap PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 180
)

foreach(_we_test_target WorldScriptTests WorldSchemaTests WorldWuiTests WorldVfsTests WorldAssetTests WorldRhiContractTests WorldVulkanDeviceTests WorldShaderPipelineTests WorldRendererHotswapTests WorldMemoryTests)
    add_custom_command(TARGET ${_we_test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:World>"
            "$<TARGET_FILE_DIR:${_we_test_target}>"
        COMMENT "Copying WorldRuntime.dll next to ${_we_test_target}.exe"
        VERBATIM
    )
endforeach()
