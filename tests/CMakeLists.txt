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
target_link_libraries(WorldVulkanDeviceTests PRIVATE World Vulkan::Vulkan)
# 内部 Vulkan 头(上传环形缓冲回归)需要 volk。
target_include_directories(WorldVulkanDeviceTests PRIVATE "${CMAKE_SOURCE_DIR}/World/vendor/volk")
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

# P2a 游戏框架:会话(GameApp)与流程(GameFlow)——固定步长、暂停、安全点切换,无设备依赖。
add_executable(WorldGameplayTests GameplayTests.cpp)
target_link_libraries(WorldGameplayTests PRIVATE World)
target_compile_features(WorldGameplayTests PRIVATE cxx_std_17)
target_compile_definitions(WorldGameplayTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldGameplayTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldGameplayTests PRIVATE /utf-8)
endif()

add_test(NAME World.Gameplay COMMAND WorldGameplayTests)
set_tests_properties(World.Gameplay PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2a W2:关卡清单与关卡服务(加载状态机/进度/场景栈,含 YAML 往返)。
add_executable(WorldLevelTests LevelTests.cpp)
target_link_libraries(WorldLevelTests PRIVATE World)
target_compile_features(WorldLevelTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLevelTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLevelTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLevelTests PRIVATE /utf-8)
endif()

add_test(NAME World.Level COMMAND WorldLevelTests)
set_tests_properties(World.Level PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D1:3D 相机与射线数学(纯 glm,无窗口/无设备)。
# 注意:源文件由 P1b D1 子 agent 产出中,文件落地后再打开本目标(临时禁用以免 configure 失败)。
set(_we_optional_test_targets "")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Camera3DTests.cpp")
add_executable(WorldCamera3DTests Camera3DTests.cpp)
target_link_libraries(WorldCamera3DTests PRIVATE World)
target_compile_features(WorldCamera3DTests PRIVATE cxx_std_17)
target_compile_definitions(WorldCamera3DTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldCamera3DTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldCamera3DTests PRIVATE /utf-8)
endif()

add_test(NAME World.Camera3D COMMAND WorldCamera3DTests)
set_tests_properties(World.Camera3D PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D2:网格资产模型(布局/包围盒/索引校验/默认物体)。
add_executable(WorldMeshTests MeshTests.cpp)
target_link_libraries(WorldMeshTests PRIVATE World)
target_compile_features(WorldMeshTests PRIVATE cxx_std_17)
target_compile_definitions(WorldMeshTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldMeshTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldMeshTests PRIVATE /utf-8)
endif()

add_test(NAME World.Mesh COMMAND WorldMeshTests)
set_tests_properties(World.Mesh PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2a W4:Prefab 子树实例化(深拷贝/UUID 重发/层级重建)。
add_executable(WorldPrefabTests PrefabTests.cpp)
target_link_libraries(WorldPrefabTests PRIVATE World)
target_compile_features(WorldPrefabTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPrefabTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldPrefabTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPrefabTests PRIVATE /utf-8)
endif()

add_test(NAME World.Prefab COMMAND WorldPrefabTests)
set_tests_properties(World.Prefab PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)
# P2a W5:SystemRegistry(阶段/顺序依赖/耗时,无设备依赖)。
add_executable(WorldSystemTests SystemRegistryTests.cpp)
target_link_libraries(WorldSystemTests PRIVATE World)
target_compile_features(WorldSystemTests PRIVATE cxx_std_17)
target_compile_definitions(WorldSystemTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldSystemTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldSystemTests PRIVATE /utf-8)
endif()

add_test(NAME World.Systems COMMAND WorldSystemTests)
set_tests_properties(World.Systems PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)
# P2a W3a:实体层级与世界变换求解(挂载/环路检测/继承开关/深层链)。
add_executable(WorldHierarchyTests HierarchyTests.cpp)
target_link_libraries(WorldHierarchyTests PRIVATE World)
target_compile_features(WorldHierarchyTests PRIVATE cxx_std_17)
target_compile_definitions(WorldHierarchyTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldHierarchyTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldHierarchyTests PRIVATE /utf-8)
endif()

add_test(NAME World.Hierarchy COMMAND WorldHierarchyTests)
set_tests_properties(World.Hierarchy PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2a W6:类型化事件总线 + 计时器(无设备依赖)。
add_executable(WorldEventTests EventTests.cpp)
target_link_libraries(WorldEventTests PRIVATE World)
target_compile_features(WorldEventTests PRIVATE cxx_std_17)
target_compile_definitions(WorldEventTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldEventTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldEventTests PRIVATE /utf-8)
endif()

add_test(NAME World.Events COMMAND WorldEventTests)
set_tests_properties(World.Events PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)
list(APPEND _we_optional_test_targets WorldCamera3DTests)
endif()

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

# 任务系统回归:提交/等待、ParallelFor、嵌套依赖、取消、压力与统计。
add_executable(WorldTaskTests TaskTests.cpp)
target_link_libraries(WorldTaskTests PRIVATE World)
target_compile_features(WorldTaskTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTaskTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTaskTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTaskTests PRIVATE /utf-8)
endif()

add_test(NAME World.Tasks COMMAND WorldTaskTests)
set_tests_properties(World.Tasks PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 180
)

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

foreach(_we_test_target WorldScriptTests WorldSchemaTests WorldWuiTests WorldVfsTests WorldAssetTests WorldRhiContractTests WorldVulkanDeviceTests WorldShaderPipelineTests WorldRendererHotswapTests WorldMemoryTests WorldTaskTests WorldGameplayTests WorldLevelTests WorldMeshTests WorldHierarchyTests WorldEventTests WorldSystemTests WorldPrefabTests ${_we_optional_test_targets})
    add_custom_command(TARGET ${_we_test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:World>"
            "$<TARGET_FILE_DIR:${_we_test_target}>"
        COMMENT "Copying WorldRuntime.dll next to ${_we_test_target}.exe"
        VERBATIM
    )
endforeach()

