# P2 W1:Luau 运行时封装(沙箱/错误定位),headless 可跑。
add_executable(WorldLuauVmTests LuauVmTests.cpp)
target_link_libraries(WorldLuauVmTests PRIVATE World)
target_compile_features(WorldLuauVmTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauVmTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauVmTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauVmTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauVm COMMAND WorldLuauVmTests)
set_tests_properties(World.LuauVm PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W1b:绑定层地基(environment/全局注入/受保护调用+traceback/引用失效/usertype)。
add_executable(WorldLuauBindingTests LuauBindingTests.cpp)
target_link_libraries(WorldLuauBindingTests PRIVATE World)
target_compile_features(WorldLuauBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauBinding COMMAND WorldLuauBindingTests)
set_tests_properties(World.LuauBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W2a:语言无关的行为注册层(两类行为的描述/重复注册拒绝/按实体枚举的确定性/共存调度)。
add_executable(WorldBehaviorRegistryTests BehaviorRegistryTests.cpp)
target_link_libraries(WorldBehaviorRegistryTests PRIVATE World)
target_compile_features(WorldBehaviorRegistryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldBehaviorRegistryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldBehaviorRegistryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldBehaviorRegistryTests PRIVATE /utf-8)
endif()

add_test(NAME World.BehaviorRegistry COMMAND WorldBehaviorRegistryTests)
set_tests_properties(World.BehaviorRegistry PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3a-A1:Entity:GetComponent 返回的 schema 字段代理(映射表/读写/失败路径/下一帧可见)。
add_executable(WorldLuauSchemaBindingTests LuauSchemaBindingTests.cpp)
target_link_libraries(WorldLuauSchemaBindingTests PRIVATE World)
target_compile_features(WorldLuauSchemaBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauSchemaBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauSchemaBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauSchemaBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauSchemaBinding COMMAND WorldLuauSchemaBindingTests)
set_tests_properties(World.LuauSchemaBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3d:Entity 生成/查询/prefab 绑定(回调内同步白名单、层级、快照可见性、失败路径)。
add_executable(WorldLuauEntityBindingTests LuauEntityBindingTests.cpp)
target_link_libraries(WorldLuauEntityBindingTests PRIVATE World)
target_compile_features(WorldLuauEntityBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauEntityBindingTests PRIVATE
    WORLD_ENTITY_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauEntityBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauEntityBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauEntityBinding COMMAND WorldLuauEntityBindingTests)
set_tests_properties(World.LuauEntityBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3a-A2:存根随 schema 自动更新(组件块/字段注解/顺序规则/确定性)。
add_executable(WorldLuauStubSchemaTests LuauStubSchemaTests.cpp)
target_link_libraries(WorldLuauStubSchemaTests PRIVATE World)
target_compile_features(WorldLuauStubSchemaTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauStubSchemaTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauStubSchemaTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauStubSchemaTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauStubSchema COMMAND WorldLuauStubSchemaTests)
set_tests_properties(World.LuauStubSchema PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3b:游戏服务面的脚本绑定(Input/Level/Save;headless 夹具走真实 GameHost.Tick)。
add_executable(WorldLuauServiceBindingTests LuauServiceBindingTests.cpp)
target_link_libraries(WorldLuauServiceBindingTests PRIVATE World)
target_compile_features(WorldLuauServiceBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauServiceBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauServiceBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauServiceBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauServiceBinding COMMAND WorldLuauServiceBindingTests)
set_tests_properties(World.LuauServiceBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3c:UI 面的脚本绑定(ui.* 立即模式控件;headless 手工输入 + WuiContext 命令断言)。
add_executable(WorldLuauUiBindingTests LuauUiBindingTests.cpp)
target_link_libraries(WorldLuauUiBindingTests PRIVATE World)
target_compile_features(WorldLuauUiBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauUiBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauUiBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauUiBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauUiBinding COMMAND WorldLuauUiBindingTests)
set_tests_properties(World.LuauUiBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W5:L2 脚本热重载(源指纹/字段迁移/失败回滚/轮询监听;headless,脚本写构建产物临时目录)。
add_executable(WorldScriptHotReloadTests ScriptHotReloadTests.cpp)
target_link_libraries(WorldScriptHotReloadTests PRIVATE World)
target_compile_features(WorldScriptHotReloadTests PRIVATE cxx_std_17)
target_compile_definitions(WorldScriptHotReloadTests PRIVATE
    WORLD_HOTRELOAD_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldScriptHotReloadTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldScriptHotReloadTests PRIVATE /utf-8)
endif()

add_test(NAME World.ScriptHotReload COMMAND WorldScriptHotReloadTests)
set_tests_properties(World.ScriptHotReload PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3e:相机字段的嵌套对象代理(CameraComponent.Camera → SceneCamera;headless)。
add_executable(WorldLuauCameraBindingTests LuauCameraBindingTests.cpp)
target_link_libraries(WorldLuauCameraBindingTests PRIVATE World)
target_compile_features(WorldLuauCameraBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauCameraBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauCameraBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauCameraBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauCameraBinding COMMAND WorldLuauCameraBindingTests)
set_tests_properties(World.LuauCameraBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W3f:2D 物理运行时 API(速度/冲量/力/运动学同步/运行时建刚体;headless)。
add_executable(WorldLuauPhysicsBindingTests LuauPhysicsBindingTests.cpp)
target_link_libraries(WorldLuauPhysicsBindingTests PRIVATE World)
target_compile_features(WorldLuauPhysicsBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauPhysicsBindingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauPhysicsBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauPhysicsBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauPhysicsBinding COMMAND WorldLuauPhysicsBindingTests)
set_tests_properties(World.LuauPhysicsBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W4:事件 / 计时器脚本绑定(顺序/off/盒装往返/失败隔离/退订/热重载重建;headless)。
add_executable(WorldLuauEventBindingTests LuauEventBindingTests.cpp)
target_link_libraries(WorldLuauEventBindingTests PRIVATE World)
target_compile_features(WorldLuauEventBindingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauEventBindingTests PRIVATE
    WORLD_EVENT_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauEventBindingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauEventBindingTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauEventBinding COMMAND WorldLuauEventBindingTests)
set_tests_properties(World.LuauEventBinding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W6:沙箱预算(指令/时间预算 + 中断 + 粘住语义;headless)。
add_executable(WorldLuauSandboxTests LuauSandboxTests.cpp)
target_link_libraries(WorldLuauSandboxTests PRIVATE World)
target_compile_features(WorldLuauSandboxTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauSandboxTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauSandboxTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauSandboxTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauSandbox COMMAND WorldLuauSandboxTests)
set_tests_properties(World.LuauSandbox PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W7-1:脚本字节码容器(WSL1)+ LuauVm::LoadChunk 统一装载入口(源码/容器分流;headless)。
add_executable(WorldScriptArtifactTests ScriptArtifactTests.cpp)
target_link_libraries(WorldScriptArtifactTests PRIVATE World)
target_compile_features(WorldScriptArtifactTests PRIVATE cxx_std_17)
target_compile_definitions(WorldScriptArtifactTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldScriptArtifactTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldScriptArtifactTests PRIVATE /utf-8)
endif()

add_test(NAME World.ScriptArtifact COMMAND WorldScriptArtifactTests)
set_tests_properties(World.ScriptArtifact PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W7-3:运行时分流(容器/源码字节;U3 无源码树 headless:只挂包 provider)。
add_executable(WorldScriptBinaryLoadTests ScriptBinaryLoadTests.cpp)
target_link_libraries(WorldScriptBinaryLoadTests PRIVATE World)
target_compile_features(WorldScriptBinaryLoadTests PRIVATE cxx_std_17)
target_compile_definitions(WorldScriptBinaryLoadTests PRIVATE
    WORLD_SCRIPT_BINARY_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/artifacts/$<CONFIG>"
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldScriptBinaryLoadTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldScriptBinaryLoadTests PRIVATE /utf-8)
endif()

add_test(NAME World.ScriptBinaryLoad COMMAND WorldScriptBinaryLoadTests)
set_tests_properties(World.ScriptBinaryLoad PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# P2 W8:脚本工作流(逻辑路径 → 可编辑磁盘路径 + LSP 脚手架 create-if-missing;headless)。
add_executable(WorldScriptWorkflowTests ScriptWorkflowTests.cpp)
target_link_libraries(WorldScriptWorkflowTests PRIVATE World)
target_compile_features(WorldScriptWorkflowTests PRIVATE cxx_std_17)
# 存根漂移门禁要复现"编辑器同一输入":额外编译 Game 的生成式 schema 注册源
# (GameAPI.cpp 运行时走同一条 RegisterGameSchemaModule)。
target_include_directories(WorldScriptWorkflowTests PRIVATE "${CMAKE_SOURCE_DIR}/Game/src")
target_sources(WorldScriptWorkflowTests PRIVATE
    "${CMAKE_SOURCE_DIR}/Game/src/Generated/Game/GameSchemaRegistration.cpp"
    "${CMAKE_SOURCE_DIR}/Game/src/Scripts/ExampleScript.cpp"
    "${CMAKE_SOURCE_DIR}/Game/src/Scripts/StressTest.cpp"
)
target_compile_definitions(WorldScriptWorkflowTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldScriptWorkflowTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldScriptWorkflowTests PRIVATE /utf-8)
endif()

add_test(NAME World.ScriptWorkflow COMMAND WorldScriptWorkflowTests)
set_tests_properties(World.ScriptWorkflow PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

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

# D10-13:WUI 模态组件(整窗输入封锁 / 居中外框+遮罩+标题+Esc / 底部按钮条)headless 回归。
add_executable(WorldWuiModalTests WuiModalTests.cpp)
target_link_libraries(WorldWuiModalTests PRIVATE World)
target_compile_features(WorldWuiModalTests PRIVATE cxx_std_17)
target_compile_definitions(WorldWuiModalTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldWuiModalTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldWuiModalTests PRIVATE /utf-8)
endif()

add_test(NAME World.WuiModal COMMAND WorldWuiModalTests)
set_tests_properties(World.WuiModal PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# W9-1:WUI 文本编辑内核(UTF-8 行索引/选区/撤销/快捷键)与代码编辑器控件的 headless 回归。
add_executable(WorldWuiTextBufferTests WuiTextBufferTests.cpp)
target_link_libraries(WorldWuiTextBufferTests PRIVATE World)
target_compile_features(WorldWuiTextBufferTests PRIVATE cxx_std_17)
target_compile_definitions(WorldWuiTextBufferTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldWuiTextBufferTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldWuiTextBufferTests PRIVATE /utf-8)
endif()

add_test(NAME World.WuiTextBuffer COMMAND WorldWuiTextBufferTests)
set_tests_properties(World.WuiTextBuffer PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# W9-2:Luau 逐行语法高亮(关键字/字符串/长括号/跨行块注释/中文串边界)+ 按行缓存。
add_executable(WorldLuauHighlighterTests LuauHighlighterTests.cpp)
target_link_libraries(WorldLuauHighlighterTests PRIVATE World)
target_compile_features(WorldLuauHighlighterTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauHighlighterTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauHighlighterTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauHighlighterTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauHighlighter COMMAND WorldLuauHighlighterTests)
set_tests_properties(World.LuauHighlighter PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# W9.5-1:Luau 补全符号表(入库存根解析/上下文/过滤排序)+ 文件内符号(headless)。
add_executable(WorldLuauCompletionTests LuauCompletionTests.cpp)
target_link_libraries(WorldLuauCompletionTests PRIVATE World)
target_compile_features(WorldLuauCompletionTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauCompletionTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauCompletionTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauCompletionTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauCompletion COMMAND WorldLuauCompletionTests)
set_tests_properties(World.LuauCompletion PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# W9.5-2:代码编辑器补全浮层(触发/选择/接受/抑制/无障碍节点)+ headless。
add_executable(WorldWuiCompletionTests WuiCompletionTests.cpp)
target_link_libraries(WorldWuiCompletionTests PRIVATE World)
target_compile_features(WorldWuiCompletionTests PRIVATE cxx_std_17)
target_compile_definitions(WorldWuiCompletionTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldWuiCompletionTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldWuiCompletionTests PRIVATE /utf-8)
endif()

add_test(NAME World.WuiCompletion COMMAND WorldWuiCompletionTests)
set_tests_properties(World.WuiCompletion PROPERTIES
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

# P1b D8a:视锥剔除数学(平面抽取/AABB 变换/相交判定)。
add_executable(WorldFrustumCullTests FrustumCullTests.cpp)
target_link_libraries(WorldFrustumCullTests PRIVATE World)
target_compile_features(WorldFrustumCullTests PRIVATE cxx_std_17)
target_compile_definitions(WorldFrustumCullTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldFrustumCullTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldFrustumCullTests PRIVATE /utf-8)
endif()

add_test(NAME World.Frustum COMMAND WorldFrustumCullTests)
set_tests_properties(World.Frustum PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D8a2:项目渲染设置(清单 rendering 区块 / 运行期生效值 / 环境变量覆盖)。
add_executable(WorldRenderSettingsTests RenderSettingsTests.cpp)
target_link_libraries(WorldRenderSettingsTests PRIVATE World)
target_compile_features(WorldRenderSettingsTests PRIVATE cxx_std_17)
target_compile_definitions(WorldRenderSettingsTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldRenderSettingsTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldRenderSettingsTests PRIVATE /utf-8)
endif()

add_test(NAME World.RenderSettings COMMAND WorldRenderSettingsTests)
set_tests_properties(World.RenderSettings PROPERTIES
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

# D3:材质资产(解析/版本/默认/夹紧/往返)+ 材质库(缓存/保存/热重载)。
add_executable(WorldMaterialTests MaterialTests.cpp)
target_link_libraries(WorldMaterialTests PRIVATE World)
target_compile_features(WorldMaterialTests PRIVATE cxx_std_17)
target_compile_definitions(WorldMaterialTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldMaterialTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldMaterialTests PRIVATE /utf-8)
endif()

add_test(NAME World.Material COMMAND WorldMaterialTests)
set_tests_properties(World.Material PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D4:光照系统(灯光收集/打包上限、组件 schema 往返)。
add_executable(WorldLightingTests LightingTests.cpp)
target_link_libraries(WorldLightingTests PRIVATE World)
target_compile_features(WorldLightingTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLightingTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLightingTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLightingTests PRIVATE /utf-8)
endif()

add_test(NAME World.Lighting COMMAND WorldLightingTests)
set_tests_properties(World.Lighting PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D5:模型导入(.wmodel 往返 / glTF 导入 / MeshIndex schema)。
add_executable(WorldModelTests ModelIOTests.cpp)
target_link_libraries(WorldModelTests PRIVATE World)
target_compile_features(WorldModelTests PRIVATE cxx_std_17)
target_compile_definitions(WorldModelTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldModelTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldModelTests PRIVATE /utf-8)
endif()

add_test(NAME World.Model COMMAND WorldModelTests)
set_tests_properties(World.Model PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# D5c-3a:CPU 蒙皮/动画数学(SampleAnimation / BuildJointMatrices 纯函数,headless)。
add_executable(WorldSkinningTests SkinningTests.cpp)
target_link_libraries(WorldSkinningTests PRIVATE World)
target_compile_features(WorldSkinningTests PRIVATE cxx_std_17)
target_compile_definitions(WorldSkinningTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldSkinningTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldSkinningTests PRIVATE /utf-8)
endif()

add_test(NAME World.Skinning COMMAND WorldSkinningTests)
set_tests_properties(World.Skinning PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# D5c-4a:骨骼动画系统(节点树采样/时间推进/调色板缓存;headless 纯 CPU)。
add_executable(WorldAnimationTests AnimationTests.cpp)
target_link_libraries(WorldAnimationTests PRIVATE World)
target_compile_features(WorldAnimationTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAnimationTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAnimationTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAnimationTests PRIVATE /utf-8)
endif()

add_test(NAME World.Animation COMMAND WorldAnimationTests)
set_tests_properties(World.Animation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P1b D6:3D 物理(Jolt)落体/堆叠/共存/接触/调试线段。
add_executable(WorldPhysics3DTests Physics3DTests.cpp)
target_link_libraries(WorldPhysics3DTests PRIVATE World)
target_compile_features(WorldPhysics3DTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPhysics3DTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldPhysics3DTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPhysics3DTests PRIVATE /utf-8)
endif()

add_test(NAME World.Physics3D COMMAND WorldPhysics3DTests)
set_tests_properties(World.Physics3D PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# P2a W7:输入映射资产 + InputService(动作/轴/多玩家/边沿判定)。
add_executable(WorldInputTests InputTests.cpp)
target_link_libraries(WorldInputTests PRIVATE World)
target_compile_features(WorldInputTests PRIVATE cxx_std_17)
target_compile_definitions(WorldInputTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldInputTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldInputTests PRIVATE /utf-8)
endif()

add_test(NAME World.Input COMMAND WorldInputTests)
set_tests_properties(World.Input PROPERTIES
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

# P2a W8:存档服务(槽位/全局块/场景块/版本迁移/坏档保护)。
add_executable(WorldSaveTests SaveTests.cpp)
target_link_libraries(WorldSaveTests PRIVATE World)
target_compile_features(WorldSaveTests PRIVATE cxx_std_17)
target_compile_definitions(WorldSaveTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldSaveTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldSaveTests PRIVATE /utf-8)
endif()

add_test(NAME World.Save COMMAND WorldSaveTests)
set_tests_properties(World.Save PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2a W10-1(D3):最小 C++ IBehavior 示例(固定步长确定性 + 生命周期)。
add_executable(WorldBehaviorTests BehaviorTests.cpp)
target_link_libraries(WorldBehaviorTests PRIVATE World)
target_compile_features(WorldBehaviorTests PRIVATE cxx_std_17)
target_compile_definitions(WorldBehaviorTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldBehaviorTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldBehaviorTests PRIVATE /utf-8)
endif()

add_test(NAME World.Behavior COMMAND WorldBehaviorTests)
set_tests_properties(World.Behavior PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2 W3a:字段 id / 生成物漂移门禁。schema-compiler --check 逐字节比较生成物;
# 注解(含显式 Id(...))改了却没重跑生成器 → 此测试红灯。生成器只构建、不参与产品二进制。
add_test(NAME World.SchemaDrift
    COMMAND schema-compiler
        --input "${CMAKE_SOURCE_DIR}/World/src/World/Scene/Components.h"
        --input "${CMAKE_SOURCE_DIR}/World/src/World/Core/UUID.h"
        --input "${CMAKE_SOURCE_DIR}/World/src/World/Scene/SceneCamera.h"
        --module World
        --output "${CMAKE_SOURCE_DIR}/World/src/World/Schema/Generated"
        --manifest "${CMAKE_SOURCE_DIR}/World/src/World/Schema/Generated/World.manifest"
        --reg-include "World/Scene/Components.h"
        --reg-include "World/Core/UUID.h"
        --reg-include "World/Scene/SceneCamera.h"
        --check)
set_tests_properties(World.SchemaDrift PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P4-UX16:"新建资产"类型注册表(注册/覆盖/反注册/排序/Create 落盘;菜单数据源)。
add_executable(WorldAssetTypeRegistryTests AssetTypeRegistryTests.cpp)
target_link_libraries(WorldAssetTypeRegistryTests PRIVATE World)
target_compile_features(WorldAssetTypeRegistryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAssetTypeRegistryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAssetTypeRegistryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAssetTypeRegistryTests PRIVATE /utf-8)
endif()

add_test(NAME World.AssetTypeRegistry COMMAND WorldAssetTypeRegistryTests)
set_tests_properties(World.AssetTypeRegistry PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P4-U4:两个"设置作用域"的回归 —— project.we.yaml 的 imports:(导入默认值)与
# .wd 头部的 World: 块(场景级设置)。含"合并保存不抹注释"与优先级(.wimport 优先)。
add_executable(WorldImportWorldSettingsTests ImportWorldSettingsTests.cpp)
target_link_libraries(WorldImportWorldSettingsTests PRIVATE World)
target_compile_features(WorldImportWorldSettingsTests PRIVATE cxx_std_17)
target_compile_definitions(WorldImportWorldSettingsTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldImportWorldSettingsTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldImportWorldSettingsTests PRIVATE /utf-8)
endif()

add_test(NAME World.ImportWorldSettings COMMAND WorldImportWorldSettingsTests)
set_tests_properties(World.ImportWorldSettings PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

foreach(_we_test_target WorldScriptTests WorldSchemaTests WorldWuiTests WorldWuiModalTests WorldWuiTextBufferTests WorldLuauHighlighterTests WorldLuauCompletionTests WorldWuiCompletionTests WorldVfsTests WorldAssetTests WorldRhiContractTests WorldVulkanDeviceTests WorldShaderPipelineTests WorldRendererHotswapTests WorldMemoryTests WorldTaskTests WorldGameplayTests WorldLevelTests WorldMeshTests WorldMaterialTests WorldLightingTests WorldModelTests WorldAnimationTests WorldPhysics3DTests WorldHierarchyTests WorldEventTests WorldSystemTests WorldPrefabTests WorldInputTests WorldSaveTests WorldBehaviorTests WorldLuauVmTests WorldLuauBindingTests WorldBehaviorRegistryTests WorldLuauSchemaBindingTests WorldLuauEntityBindingTests WorldLuauStubSchemaTests WorldLuauServiceBindingTests WorldLuauUiBindingTests WorldScriptHotReloadTests WorldLuauCameraBindingTests WorldLuauPhysicsBindingTests WorldLuauEventBindingTests WorldLuauSandboxTests WorldScriptArtifactTests WorldScriptBinaryLoadTests WorldScriptWorkflowTests WorldAssetTypeRegistryTests WorldImportWorldSettingsTests ${_we_optional_test_targets})
    add_custom_command(TARGET ${_we_test_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:World>"
            "$<TARGET_FILE_DIR:${_we_test_target}>"
        COMMENT "Copying WorldRuntime.dll next to ${_we_test_target}.exe"
        VERBATIM
    )
endforeach()

