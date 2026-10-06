# P2 W1:Luau 运行时封装(沙箱/错误定位),headless 可跑。
add_executable(WorldLuauVmTests World/LuauVmTests.cpp)
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
add_executable(WorldLuauBindingTests World/LuauBindingTests.cpp)
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




# P2 W3a-A2:存根随 schema 自动更新(组件块/字段注解/顺序规则/确定性)。
add_executable(WorldLuauStubSchemaTests World/LuauStubSchemaTests.cpp)
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







# P2 W6:沙箱预算(指令/时间预算 + 中断 + 粘住语义;headless)。
add_executable(WorldLuauSandboxTests World/LuauSandboxTests.cpp)
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
add_executable(WorldScriptArtifactTests World/ScriptArtifactTests.cpp)
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


# P2 W8:脚本工作流(逻辑路径 → 可编辑磁盘路径 + LSP 脚手架 create-if-missing;headless)。
add_executable(WorldScriptWorkflowTests World/ScriptWorkflowTests.cpp)
target_link_libraries(WorldScriptWorkflowTests PRIVATE World)
target_compile_features(WorldScriptWorkflowTests PRIVATE cxx_std_17)
# 存根漂移门禁要复现"编辑器同一输入":额外编译 Game 模块的生成式 schema 注册源
# (GameAPI.cpp 运行时走同一条 RegisterGameSchemaModule)。PROJ-8/T2 起示例 C++ 属于项目层/
# 示例模板,引擎 Game/src 只剩骨架 ⇒ 测试改指**测试夹具** tests/fixtures/game-module/
# (同名同形:Game::SampleDataComponent,存根夹具逐字节可比)。
target_include_directories(WorldScriptWorkflowTests PRIVATE "${CMAKE_SOURCE_DIR}/tests/fixtures/game-module/src")
target_sources(WorldScriptWorkflowTests PRIVATE
    "${CMAKE_SOURCE_DIR}/tests/fixtures/game-module/src/Generated/Game/GameSchemaRegistration.cpp"
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


# Schema 2.0 核心回归:注册表、稳定身份、生成访问器、事务化模块注册。
add_executable(WorldSchemaTests
    World/SchemaTests.cpp
    World/schema/Generated/TestKit/TestKitSchemaRegistration.cpp
)
target_include_directories(WorldSchemaTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/World")
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
add_executable(WorldWuiTests World/WuiTests.cpp)
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
add_executable(WorldWuiModalTests World/WuiModalTests.cpp)
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
add_executable(WorldWuiTextBufferTests World/WuiTextBufferTests.cpp)
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
add_executable(WorldLuauHighlighterTests World/LuauHighlighterTests.cpp)
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
add_executable(WorldLuauCompletionTests World/LuauCompletionTests.cpp)
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
add_executable(WorldWuiCompletionTests World/WuiCompletionTests.cpp)
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
add_executable(WorldVfsTests World/VfsTests.cpp)
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
add_executable(WorldAssetTests World/AssetCookTests.cpp)
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
add_executable(WorldRhiContractTests World/RhiContractTests.cpp)
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
add_executable(WorldVulkanDeviceTests World/VulkanDeviceTests.cpp)
target_link_libraries(WorldVulkanDeviceTests PRIVATE World Vulkan::Vulkan)
# 内部 Vulkan 头(上传环形缓冲回归)需要 volk。
target_include_directories(WorldVulkanDeviceTests PRIVATE "${CMAKE_SOURCE_DIR}/third_party/volk")
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
add_executable(WorldGameplayTests Game/GameplayTests.cpp)
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
add_executable(WorldLevelTests World/LevelTests.cpp)
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
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/World/Camera3DTests.cpp")
add_executable(WorldCamera3DTests World/Camera3DTests.cpp)
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
add_executable(WorldFrustumCullTests World/FrustumCullTests.cpp)
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
add_executable(WorldRenderSettingsTests World/RenderSettingsTests.cpp)
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
add_executable(WorldMeshTests World/MeshTests.cpp)
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
add_executable(WorldMaterialTests World/MaterialTests.cpp)
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
add_executable(WorldLightingTests World/LightingTests.cpp)
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
add_executable(WorldModelTests World/ModelIOTests.cpp)
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
add_executable(WorldSkinningTests World/SkinningTests.cpp)
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
add_executable(WorldAnimationTests World/AnimationTests.cpp)
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
add_executable(WorldPhysics3DTests World/Physics3DTests.cpp)
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

# P5:物理 ↔ ECS 事件与过滤(2D Box2D / 3D Jolt 两侧:接触/触发/层掩码/确定性)。
add_executable(WorldPhysicsEventsTests World/PhysicsEventsTests.cpp)
target_link_libraries(WorldPhysicsEventsTests PRIVATE World)
target_compile_features(WorldPhysicsEventsTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPhysicsEventsTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldPhysicsEventsTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPhysicsEventsTests PRIVATE /utf-8)
endif()

add_test(NAME World.PhysicsEvents COMMAND WorldPhysicsEventsTests)
set_tests_properties(World.PhysicsEvents PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# P7:关节(Fixed/Distance/Hinge)+ CCD(2D Box2D / 3D Jolt)。
add_executable(WorldPhysicsJointTests World/PhysicsJointTests.cpp)
target_link_libraries(WorldPhysicsJointTests PRIVATE World)
target_compile_features(WorldPhysicsJointTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPhysicsJointTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldPhysicsJointTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPhysicsJointTests PRIVATE /utf-8)
endif()

add_test(NAME World.PhysicsJoints COMMAND WorldPhysicsJointTests)
set_tests_properties(World.PhysicsJoints PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# P6:固定步长 → 渲染插值(数学 / 权威不动 / 状态记录 / alpha 来源)。
add_executable(WorldTransformInterpolationTests World/TransformInterpolationTests.cpp)
target_link_libraries(WorldTransformInterpolationTests PRIVATE World)
target_compile_features(WorldTransformInterpolationTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTransformInterpolationTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTransformInterpolationTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTransformInterpolationTests PRIVATE /utf-8)
endif()

add_test(NAME World.TransformInterpolation COMMAND WorldTransformInterpolationTests)
set_tests_properties(World.TransformInterpolation PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# P2a W7:输入映射资产 + InputService(动作/轴/多玩家/边沿判定)。
add_executable(WorldInputTests World/InputTests.cpp)
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
# WP5:帧首输入采样(InputSystem)+ 场景时间服务(TimerSystem)的反假验收
# (确定性/一次采样/计时语义/Lua Time 读数)。
add_executable(WorldFrameTimeTests World/FrameTimeTests.cpp)
target_link_libraries(WorldFrameTimeTests PRIVATE World)
target_compile_features(WorldFrameTimeTests PRIVATE cxx_std_17)
target_compile_definitions(WorldFrameTimeTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldFrameTimeTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldFrameTimeTests PRIVATE /utf-8)
endif()

add_test(NAME World.FrameTime COMMAND WorldFrameTimeTests)
set_tests_properties(World.FrameTime PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)
# P2a W4:Prefab 子树实例化(深拷贝/UUID 重发/层级重建)。
add_executable(WorldPrefabTests World/PrefabTests.cpp)
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
add_executable(WorldSystemTests World/SystemRegistryTests.cpp)
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
add_executable(WorldHierarchyTests World/HierarchyTests.cpp)
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

# Pure ECS M2: 现代 Query DSL 与系统管线统一。
add_executable(WorldQueryTests World/QueryTests.cpp)
target_link_libraries(WorldQueryTests PRIVATE World)
target_compile_features(WorldQueryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldQueryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldQueryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldQueryTests PRIVATE /utf-8)
endif()

add_test(NAME World.Query COMMAND WorldQueryTests)
set_tests_properties(World.Query PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# Pure ECS M3: Luau 纯 ECS 绑定测试 (ecs / world 全局表、Query DSL、AddSystem、EntityCount)。
add_executable(WorldLuauEcsTests World/LuauEcsTests.cpp)
target_link_libraries(WorldLuauEcsTests PRIVATE World)
target_compile_features(WorldLuauEcsTests PRIVATE cxx_std_17)
target_compile_definitions(WorldLuauEcsTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLuauEcsTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLuauEcsTests PRIVATE /utf-8)
endif()

add_test(NAME World.LuauEcs COMMAND WorldLuauEcsTests)
set_tests_properties(World.LuauEcs PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# Pure ECS M6: 端到端纯 ECS 游戏性示例套件 (POD 组件、ISystem 纯系统、Query DSL、响应式 Observer、Luau 系统协同)。
add_executable(WorldPureEcsExampleTests World/PureEcsExampleTests.cpp)
target_link_libraries(WorldPureEcsExampleTests PRIVATE World)
target_compile_features(WorldPureEcsExampleTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPureEcsExampleTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldPureEcsExampleTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPureEcsExampleTests PRIVATE /utf-8)
endif()

add_test(NAME World.PureEcsExample COMMAND WorldPureEcsExampleTests)
set_tests_properties(World.PureEcsExample PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2a W6:类型化事件总线 + 计时器(无设备依赖)。
add_executable(WorldEventTests World/EventTests.cpp)
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
add_executable(WorldShaderPipelineTests World/ShaderPipelineTests.cpp)
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

# M4-TEX:纹理导入设置 + 烘焙产物(确定性 / 格式选择 / mip 链 / 缓存 / 产物解析)。
add_executable(WorldTextureImportTests World/TextureImportTests.cpp)
target_link_libraries(WorldTextureImportTests PRIVATE World)
target_compile_features(WorldTextureImportTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTextureImportTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTextureImportTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTextureImportTests PRIVATE /utf-8)
endif()

add_test(NAME World.TextureImport COMMAND WorldTextureImportTests)
set_tests_properties(World.TextureImport PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 180
)

# M4-TEX P2:运行时消费烘焙产物(产物优先 / 回退源图 / 坏产物保护 / BC5 法线开关)。
add_executable(WorldTextureRuntimeTests World/TextureRuntimeTests.cpp)
target_link_libraries(WorldTextureRuntimeTests PRIVATE World)
target_compile_features(WorldTextureRuntimeTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTextureRuntimeTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTextureRuntimeTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTextureRuntimeTests PRIVATE /utf-8)
endif()

add_test(NAME World.TextureRuntime COMMAND WorldTextureRuntimeTests)
set_tests_properties(World.TextureRuntime PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 180
)

# 内存基座回归:每线程帧 arena、析构、追加页、双轨与池分配器。
# 注意:GlobalAllocHooks.cpp 是按模块生效的替换式 operator new —— 测试进程要统计自己的
# 分配,必须也编译一份(状态在 WorldRuntime.dll 里,两份 hook 共享同一张表)。
# 这正是“没装 hook 的模块的分配不可见”这条口径的实证。

add_executable(WorldMemoryTests World/MemoryTests.cpp)
target_link_libraries(WorldMemoryTests PRIVATE World)
target_sources(WorldMemoryTests PRIVATE
    ${WLD_ENGINE_ROOT}/Engine/src/World/Profiling/GlobalAllocHooks.cpp
)
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

# 遥测(M1)回归:帧统计分位数、采集落盘与回读、溢出计数、并发写入、缓冲复用。
# 反假:分位数用"多数快帧 + 少数慢帧"的已知形状验证;采集回读解析 JSON 逐项核对。
add_executable(WorldTelemetryTests World/TelemetryTests.cpp)
target_link_libraries(WorldTelemetryTests PRIVATE World)
# 同 WorldMemoryTests:测试进程要统计自己的分配,必须也编译一份 hook
# (状态在 WorldRuntime.dll 里,各模块的 hook 共享同一张表)。
target_sources(WorldTelemetryTests PRIVATE
    ${WLD_ENGINE_ROOT}/Engine/src/World/Profiling/GlobalAllocHooks.cpp
)
target_compile_features(WorldTelemetryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTelemetryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTelemetryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTelemetryTests PRIVATE /utf-8)
endif()

add_test(NAME World.Telemetry COMMAND WorldTelemetryTests)
set_tests_properties(World.Telemetry PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)
# 任务系统回归:提交/等待、ParallelFor、嵌套依赖、取消、压力与统计。
add_executable(WorldTaskTests World/TaskTests.cpp)
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
add_executable(WorldRendererHotswapTests World/RendererHotswapTests.cpp)
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
add_executable(WorldSaveTests World/SaveTests.cpp)
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
add_executable(WorldBehaviorTests World/BehaviorTests.cpp)
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

# PECS-T4:项目层 C++ 系统的场景挂载钩子(AddSceneSystemsHook / RunSceneAttachHooks)契约回归。
add_executable(WorldProjectSystemsHookTests World/ProjectSystemsHookTests.cpp)
target_link_libraries(WorldProjectSystemsHookTests PRIVATE World)
target_compile_features(WorldProjectSystemsHookTests PRIVATE cxx_std_17)
target_compile_definitions(WorldProjectSystemsHookTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldProjectSystemsHookTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldProjectSystemsHookTests PRIVATE /utf-8)
endif()

add_test(NAME World.ProjectSystemsHook COMMAND WorldProjectSystemsHookTests)
set_tests_properties(World.ProjectSystemsHook PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# PECS-T5:宿主与 WorldRuntime.dll 的运行期布局契约(sizeof(Scene)/sizeof(WorldContext)/模块 ABI)。
add_executable(WorldRuntimeContractTests World/RuntimeContractTests.cpp)
target_link_libraries(WorldRuntimeContractTests PRIVATE World)
target_compile_features(WorldRuntimeContractTests PRIVATE cxx_std_17)
target_compile_definitions(WorldRuntimeContractTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldRuntimeContractTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldRuntimeContractTests PRIVATE /utf-8)
endif()

add_test(NAME World.RuntimeContract COMMAND WorldRuntimeContractTests)
set_tests_properties(World.RuntimeContract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P2 W3a:字段 id / 生成物漂移门禁。schema-compiler --check 逐字节比较生成物;
# 注解(含显式 Id(...))改了却没重跑生成器 → 此测试红灯。生成器只构建、不参与产品二进制。
add_test(NAME World.SchemaDrift
    COMMAND schema-compiler
        --input "${CMAKE_SOURCE_DIR}/Engine/src/World/Scene/Components.h"
        --input "${CMAKE_SOURCE_DIR}/Engine/src/World/Core/UUID.h"
        --input "${CMAKE_SOURCE_DIR}/Engine/src/World/Scene/SceneCamera.h"
        --module World
        --output "${CMAKE_SOURCE_DIR}/Engine/src/World/Schema/Generated"
        --manifest "${CMAKE_SOURCE_DIR}/Engine/src/World/Schema/Generated/World.manifest"
        --reg-include "World/Scene/Components.h"
        --reg-include "World/Core/UUID.h"
        --reg-include "World/Scene/SceneCamera.h"
        --check)
set_tests_properties(World.SchemaDrift PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# CPPT-2:Game 模块生成物漂移门禁(与 World.SchemaDrift 同形态)。改 WE_FIELD/注解/属性元数据
# 却不重跑 schema-compiler → 此测试红灯。
# PROJ-8/T2:输入改指**测试夹具** tests/fixtures/game-module/src/**(引擎 Game/src 只剩骨架,
# 示例 C++ 随示例模板来到项目层);夹具与 WorldScriptWorkflowTests 编译的是同一套输入/生成物。
add_test(NAME Game.SchemaDrift
    COMMAND schema-compiler
        --input "${CMAKE_SOURCE_DIR}/tests/fixtures/game-module/src/Components/SampleDataComponent.h"
        --module Game
        --output "${CMAKE_SOURCE_DIR}/tests/fixtures/game-module/src/Generated"
        --manifest "${CMAKE_SOURCE_DIR}/tests/fixtures/game-module/src/Generated/Game.manifest"
        --reg-include "Components/SampleDataComponent.h"
        --check)
set_tests_properties(Game.SchemaDrift PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)




# T5c(2026-10-05):纹理异步解码 + 唯一 GPU 驻留(headless 安全契约)。
add_executable(WorldTextureAsyncTests World/TextureAsyncTests.cpp)
target_link_libraries(WorldTextureAsyncTests PRIVATE World)
target_compile_features(WorldTextureAsyncTests PRIVATE cxx_std_17)
target_compile_definitions(WorldTextureAsyncTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldTextureAsyncTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldTextureAsyncTests PRIVATE /utf-8)
endif()

add_test(NAME World.TextureAsync COMMAND WorldTextureAsyncTests)
set_tests_properties(World.TextureAsync PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# T5c(2026-10-04):异步加载器(工作线程解析 + 主线程提交的两阶段)。
add_executable(WorldAsyncLoaderTests World/AsyncLoaderTests.cpp)
target_link_libraries(WorldAsyncLoaderTests PRIVATE World)
target_compile_features(WorldAsyncLoaderTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAsyncLoaderTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAsyncLoaderTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAsyncLoaderTests PRIVATE /utf-8)
endif()

add_test(NAME World.AsyncLoader COMMAND WorldAsyncLoaderTests)
set_tests_properties(World.AsyncLoader PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# T4(2026-10-04):资产稳定身份(AssetId)+ 目录 + 改名不断链 + 组件身份通道。
add_executable(WorldAssetIdentityTests World/AssetIdentityTests.cpp)
target_link_libraries(WorldAssetIdentityTests PRIVATE World)
target_compile_features(WorldAssetIdentityTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAssetIdentityTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAssetIdentityTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAssetIdentityTests PRIVATE /utf-8)
endif()

add_test(NAME World.AssetIdentity COMMAND WorldAssetIdentityTests)
set_tests_properties(World.AssetIdentity PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# PECS 1.3(2026-10-04):L2 资产驻留层 + 显式世界资源表(记账/回收/Pin/归属)。
add_executable(WorldAssetRegistryTests World/AssetRegistryTests.cpp)
target_link_libraries(WorldAssetRegistryTests PRIVATE World)
target_compile_features(WorldAssetRegistryTests PRIVATE cxx_std_17)
target_compile_definitions(WorldAssetRegistryTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldAssetRegistryTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldAssetRegistryTests PRIVATE /utf-8)
endif()

add_test(NAME World.AssetRegistry COMMAND WorldAssetRegistryTests)
set_tests_properties(World.AssetRegistry PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# PECS 1.2(2026-10-04):驻留字符串池(PathId/NameId)——资产定位符与名字标识的 4B POD 化。
add_executable(WorldStringPoolTests World/StringPoolTests.cpp)
target_link_libraries(WorldStringPoolTests PRIVATE World)
target_compile_features(WorldStringPoolTests PRIVATE cxx_std_17)
target_compile_definitions(WorldStringPoolTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldStringPoolTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldStringPoolTests PRIVATE /utf-8)
endif()

# SIMD 规则门禁 E(2026-10-05):手写 intrinsics 只允许出现在 Engine/src/World/Math/Simd/。
# 标准 docs/dev/performance-and-data-layout.md §6.5.2 S1。理由:让"哪里有 SIMD"可枚举、可审查、
# 可移植化 —— 一旦散落,新增 ARM64 目标时的回退与封装就无从检查。
add_test(NAME World.SimdPolicy
    COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/agents/check-simd.ps1"
        -ProjectRoot "${CMAKE_SOURCE_DIR}")
set_tests_properties(World.SimdPolicy PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# SIMD 逐位一致性对照(2026-10-05):批量判定 lane=对象、与标量逐位一致 ⇒ 可默认开启。
add_executable(WorldSimdTests World/SimdTests.cpp)
target_link_libraries(WorldSimdTests PRIVATE World)
target_compile_features(WorldSimdTests PRIVATE cxx_std_17)
target_compile_definitions(WorldSimdTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldSimdTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldSimdTests PRIVATE /utf-8)
endif()

add_test(NAME World.Simd COMMAND WorldSimdTests)
set_tests_properties(World.Simd PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)
# 数据布局基准(2026-10-05):**手动目标,不注册 add_test**。
# 标准 docs/dev/performance-and-data-layout.md §4.1 Q1 / §6 O0 —— 让"要不要为布局优化"有数字可证伪。
# 跑法:cmake --build build/x64-Debug --config Debug --target WorldLayoutBenchmarks
# O3(/arch:AVX2)的同源对照:同一份 .cpp,只多一个 /arch:AVX2 —— 这样"AVX2 有没有用"
# 是同一台机器、同一天、同一份源码下的 A/B,不是跨机器对比。只对 MSVC 生效。
if (MSVC)
    add_executable(WorldLayoutBenchmarksAvx2 World/LayoutBenchmarks.cpp)
    target_link_libraries(WorldLayoutBenchmarksAvx2 PRIVATE World)
    target_compile_features(WorldLayoutBenchmarksAvx2 PRIVATE cxx_std_17)
    target_compile_definitions(WorldLayoutBenchmarksAvx2 PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    if(WIN32)
        target_compile_definitions(WorldLayoutBenchmarksAvx2 PRIVATE WLD_PLATFORM_WINDOWS)
    endif()
    target_compile_options(WorldLayoutBenchmarksAvx2 PRIVATE /utf-8 /arch:AVX2)
endif()

add_executable(WorldLayoutBenchmarks World/LayoutBenchmarks.cpp)
target_link_libraries(WorldLayoutBenchmarks PRIVATE World)
target_compile_features(WorldLayoutBenchmarks PRIVATE cxx_std_17)
target_compile_definitions(WorldLayoutBenchmarks PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldLayoutBenchmarks PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldLayoutBenchmarks PRIVATE /utf-8)
endif()
# 数据布局门禁 A(2026-10-05):schema 组件布局枚举 + 64B 预算 + 快照棘轮。
# 标准:docs/dev/performance-and-data-layout.md §4.2 C1 / §4.4。
add_executable(WorldComponentLayoutTests World/ComponentLayoutTests.cpp)
target_link_libraries(WorldComponentLayoutTests PRIVATE World)
target_compile_features(WorldComponentLayoutTests PRIVATE cxx_std_17)
target_compile_definitions(WorldComponentLayoutTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldComponentLayoutTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldComponentLayoutTests PRIVATE /utf-8)
endif()

add_test(NAME World.ComponentLayout COMMAND WorldComponentLayoutTests)
set_tests_properties(World.ComponentLayout PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

add_test(NAME World.StringPool COMMAND WorldStringPoolTests)
set_tests_properties(World.StringPool PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 60
)

# P4-UX16:"新建资产"类型注册表(注册/覆盖/反注册/排序/Create 落盘;菜单数据源)。
add_executable(WorldAssetTypeRegistryTests World/AssetTypeRegistryTests.cpp)
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
# .wd 头部的 World: 块(场景级设置)。含"合并保存不抹注释"与优先级(资产 meta 优先)。
add_executable(WorldImportWorldSettingsTests World/ImportWorldSettingsTests.cpp)
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

# PLUG-T1:插件系统 T1(清单解析 / 发现 / 校验 / 依赖拓扑 / 加载 / 卸载 / 回滚)headless 单测。
# 测试插件 DLL 不链接 World(只依赖公共 ABI 头 WePluginApi.h),由插件加载器显式取符号,
# 和真实第三方插件的边界一致;产物拷到测试 exe 旁,测试再把它们复制进临时插件包。
add_executable(WorldPluginTests World/PluginTests.cpp)
target_link_libraries(WorldPluginTests PRIVATE World)
target_compile_features(WorldPluginTests PRIVATE cxx_std_17)
target_compile_definitions(WorldPluginTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WE_PLUGIN_TEST_DLL_DIR="$<TARGET_FILE_DIR:WorldPluginTests>"
    # T2a:示例插件夹具目录(用例 ④ 直接读真实清单,验证"包形态 + 清单"可被 Discover 解析)。
    # 2026-10-02:从 <引擎根>/plugins/hello-import 搬到这里 —— 放在 plugins/ 下会被编辑器
    # 每次启动都加载,并在右键「新建」里多出 "Hello File"。
    WE_PLUGIN_EXAMPLE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/plugins/hello-import"
)
if(WIN32)
    target_compile_definitions(WorldPluginTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldPluginTests PRIVATE /utf-8)
endif()

add_test(NAME World.Plugins COMMAND WorldPluginTests)
set_tests_properties(World.Plugins PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 120
)

# T2a:引擎示例插件(plugins/hello-import)在配置期可见时,测试做**真实加载**(不只是清单解析):
# 显式依赖保证 DLL 先于测试产出;路径用生成表达式取,不依赖 VS 多配置的输出层级。
# 2026-10-02:夹具不在引擎的 plugins/** glob 里,所以这里显式把它加进来构建 ——
# 真 DLL 覆盖(资产类型/导入器/导出表/卸载回收)保持不变。
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/fixtures/plugins/hello-import"
    "${CMAKE_CURRENT_BINARY_DIR}/fixture-plugins/hello-import")
if(TARGET WePlugin_hello-import)
    add_dependencies(WorldPluginTests WePlugin_hello-import)
    target_compile_definitions(WorldPluginTests PRIVATE
        WE_PLUGIN_EXAMPLE_DLL="$<TARGET_FILE:WePlugin_hello-import>"
    )
endif()

set(WE_PLUGIN_TEST_DLL_TARGETS
    WePluginTestAlpha
    WePluginTestBeta
    WePluginTestAbiBad
    WePluginTestSizeBad
    WePluginTestSizeLarge
    WePluginTestIdMismatch
    WePluginTestNoRegister
    WePluginTestFailRegister
    WePluginTestThrowRegister
    # T2a:宿主注册面正例 / 违约兜底负例 / 插件间 LookupExport。
    WePluginTestHostApi
    WePluginTestLeaky
    WePluginTestLookup
    # T2b:组件 schema 注册正例(含坏描述负例与注销导出)/ 违约兜底回收负例。
    WePluginTestComponent
    WePluginTestLeakyComponent
    # T2c:组件存储桥正例(Size/Alignment → 宿主合成 blob 存储,能挂到场景实体上)。
    WePluginTestComponentStorage
    # T3b:编辑器扩展注册正例(命令/面板/触发计数/注销幂等)/ 三条负例
    # (leaky 卸载回收、Register 抛异常回收、未接线宿主干净拒绝)。
    WePluginTestEditorUi
    WePluginTestLeakyEditorUi
    WePluginTestThrowEditorUi
    # T4:脚本函数库正例(坏描述拒绝 / 重复拒绝 / hello.ping+echo+boom / 按需注销幂等)/
    # 第二个插件(共享命名空间 + 跨插件归属保护 + 卸载兜底回收)/ Register 抛异常回滚。
    WePluginTestScriptLib
    WePluginTestScriptTwo
    WePluginTestThrowScriptLib
)
foreach(_we_plugin_target IN LISTS WE_PLUGIN_TEST_DLL_TARGETS)
    add_library(${_we_plugin_target} SHARED "plugins/${_we_plugin_target}.cpp")
    target_compile_features(${_we_plugin_target} PRIVATE cxx_std_17)
    target_include_directories(${_we_plugin_target} PRIVATE "${WLD_ENGINE_ROOT}/Engine/src")
    target_compile_definitions(${_we_plugin_target} PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
    )
    if(WIN32)
        target_compile_definitions(${_we_plugin_target} PRIVATE WLD_PLATFORM_WINDOWS)
    endif()
    if(MSVC)
        target_compile_options(${_we_plugin_target} PRIVATE /utf-8)
    endif()
endforeach()
add_dependencies(WorldPluginTests ${WE_PLUGIN_TEST_DLL_TARGETS})

# 插件 DLL 拷到测试 exe 旁(与文件末尾 WorldRuntime.dll 的 copy_if_different 同写法;
# 放在测试目标的 POST_BUILD 里,避免"插件目标 ↔ 测试目标"互相引用成环)。
foreach(_we_plugin_target IN LISTS WE_PLUGIN_TEST_DLL_TARGETS)
    add_custom_command(TARGET WorldPluginTests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${_we_plugin_target}>"
            "$<TARGET_FILE_DIR:WorldPluginTests>"
        COMMENT "Copying ${_we_plugin_target} next to WorldPluginTests.exe"
        VERBATIM
    )
endforeach()

# GameUI(M0):`.wui` 文档格式 + 版本门禁 + 锚点/缩放/安全区/命中 + 实例树布局。
add_executable(WorldUiDocumentTests World/UiDocumentTests.cpp)
target_link_libraries(WorldUiDocumentTests PRIVATE World)
target_compile_features(WorldUiDocumentTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiDocumentTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiDocumentTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiDocumentTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiDocument COMMAND WorldUiDocumentTests)
set_tests_properties(World.UiDocument PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M2):节点类型注册表 + `.wui` 绘制进 WuiContext + 无障碍节点登记(headless)。
add_executable(WorldUiPaintTests World/UiPaintTests.cpp)
target_link_libraries(WorldUiPaintTests PRIVATE World)
target_compile_features(WorldUiPaintTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiPaintTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiPaintTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiPaintTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiPaint COMMAND WorldUiPaintTests)
set_tests_properties(World.UiPaint PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M3):页面栈/模态/层序/转场 + 输入消费/焦点/手柄导航(headless)。
add_executable(WorldUiNavInputTests World/UiNavInputTests.cpp)
target_link_libraries(WorldUiNavInputTests PRIVATE World)
target_compile_features(WorldUiNavInputTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiNavInputTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiNavInputTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiNavInputTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiNavInput COMMAND WorldUiNavInputTests)
set_tests_properties(World.UiNavInput PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M6):数据绑定(协议解析/解析器注册/变更检测)+ 动效状态机/补间/曲线(headless)。
add_executable(WorldUiBindAnimTests World/UiBindAnimTests.cpp)
target_link_libraries(WorldUiBindAnimTests PRIVATE World)
target_compile_features(WorldUiBindAnimTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiBindAnimTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiBindAnimTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiBindAnimTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiBindAnim COMMAND WorldUiBindAnimTests)
set_tests_properties(World.UiBindAnim PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# World.UiWorld:世界空间 UI(M8)投影与锚定。
#
# 口径:`ApplyWorldAnchors` 只把"锚点输入"从父矩形换成投影点,产出的仍是设计空间矩形 ——
# 因此绘制与命中共用同一 `UiViewport` 映射(contract.ui-runtime §2)。
#
# WorldEngine(EN)

add_executable(WorldUiWorldTests World/UiWorldTests.cpp)
target_link_libraries(WorldUiWorldTests PRIVATE World)
target_compile_features(WorldUiWorldTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiWorldTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiWorldTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiWorldTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiWorld COMMAND WorldUiWorldTests)
set_tests_properties(World.UiWorld PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# World.UiScroll:滚动容器 + 商业控件面(M10)。
#
# 口径:`scrollable`/`overflow` 判定与 `UiInputRouter::IsScrollContainer` 同一份规则;
# 滚动偏移存 `UiScreen` 运行态(按稳定 Id),`UiPainter` 对滚动容器压/弹裁剪矩形,
# 溢出部分既不画也不命中;List/Grid 虚拟化(1000 行只画可见行)。
#
# WorldEngine(EN)

add_executable(WorldUiScrollTests World/UiScrollTests.cpp)
target_link_libraries(WorldUiScrollTests PRIVATE World)
target_compile_features(WorldUiScrollTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiScrollTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiScrollTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiScrollTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiScroll COMMAND WorldUiScrollTests)
set_tests_properties(World.UiScroll PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M9):UI 输入路由闭环(headless)—— GameHost 指针捕获语义 + UiHost::RouteInput 未启用零副作用。
add_executable(WorldUiPointerGateTests World/UiPointerGateTests.cpp)
target_link_libraries(WorldUiPointerGateTests PRIVATE World)
target_compile_features(WorldUiPointerGateTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiPointerGateTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiPointerGateTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiPointerGateTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiPointerGate COMMAND WorldUiPointerGateTests)
set_tests_properties(World.UiPointerGate PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M13):交互输入状态机(headless)—— 滑条拖拽 / 文本框编辑 / 滚轮滚动落地。
# 注:按派工单要求只追加目标与 add_test,不改动文件末尾的 WeCopyRuntimeDllForTests 白名单 foreach。
add_executable(WorldUiInputInteractTests World/UiInputInteractTests.cpp)
target_link_libraries(WorldUiInputInteractTests PRIVATE World)
target_compile_features(WorldUiInputInteractTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiInputInteractTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiInputInteractTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiInputInteractTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiInputInteract COMMAND WorldUiInputInteractTests)
set_tests_properties(World.UiInputInteract PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M14):主题令牌与换肤(headless)—— 令牌解析 / 多级引用 / 环 / 未定义 + 绘制颜色 + 无障碍文本。
# 注:按派工单要求只追加目标与 add_test,不改动文件末尾的 WeCopyRuntimeDllForTests 白名单 foreach。
add_executable(WorldUiThemeTests World/UiThemeTests.cpp)
target_link_libraries(WorldUiThemeTests PRIVATE World)
target_compile_features(WorldUiThemeTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiThemeTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiThemeTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiThemeTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiTheme COMMAND WorldUiThemeTests)
set_tests_properties(World.UiTheme PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M26):绑定解析器(ecs: 反射)/ 运行态属性覆盖 / 命令 router(EventBus)—— headless。
add_executable(WorldUiBindingSourcesTests World/UiBindingSourcesTests.cpp)
target_link_libraries(WorldUiBindingSourcesTests PRIVATE World)
target_compile_features(WorldUiBindingSourcesTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiBindingSourcesTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiBindingSourcesTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiBindingSourcesTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiBindingSources COMMAND WorldUiBindingSourcesTests)
set_tests_properties(World.UiBindingSources PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M30):控件属性覆盖度门禁(绘制读的属性 ⊆ 登记表声明的属性 + Kind/编码一致 +
# StateScoped 契约 + 本地化查询 API)—— headless。
add_executable(WorldUiPropertyCoverageTests World/UiPropertyCoverageTests.cpp)
target_link_libraries(WorldUiPropertyCoverageTests PRIVATE World)
target_compile_features(WorldUiPropertyCoverageTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiPropertyCoverageTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiPropertyCoverageTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiPropertyCoverageTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiPropertyCoverage COMMAND WorldUiPropertyCoverageTests)
set_tests_properties(World.UiPropertyCoverage PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# GameUI(M34):`.wui` 热重载(文件戳判脏 / 失败保留上一份可用版本 / 运行态按稳定 Id 迁移)—— headless。
add_executable(WorldUiHotReloadTests World/UiHotReloadTests.cpp)
target_link_libraries(WorldUiHotReloadTests PRIVATE World)
target_compile_features(WorldUiHotReloadTests PRIVATE cxx_std_17)
target_compile_definitions(WorldUiHotReloadTests PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
if(WIN32)
    target_compile_definitions(WorldUiHotReloadTests PRIVATE WLD_PLATFORM_WINDOWS)
endif()
if(MSVC)
    target_compile_options(WorldUiHotReloadTests PRIVATE /utf-8)
endif()

add_test(NAME World.UiHotReload COMMAND WorldUiHotReloadTests)
set_tests_properties(World.UiHotReload PROPERTIES
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    TIMEOUT 90
)

# WorldRuntime.dll -> 测试 exe 旁:**只拷一次**。
#
# 原先每个测试目标各挂一条 POST_BUILD `copy_if_different`,而本文件的所有测试 exe 都在同一目录
# (全文件无人设 RUNTIME_OUTPUT_DIRECTORY)⇒ ~49 个目标在 `--parallel` 下抢同一个目标文件,
# 偶发 `Permission denied`(MSB3073,单轮可达 50 条;串行重跑即过)。证据与根因见
# tools/agents/reports/MAT-UI6b-host-zoom.md 与 knowledge/traps/test-dll-copy-loop.md。
# 改成单一 custom target + add_dependencies:拷贝只发生一次,顺序由目标级依赖保证,
# `--parallel` 下不再存在第二个写者。
#
# 白名单仍然存在:**新增测试目标必须加进下面那个 foreach**,否则单独构建它时不会先跑
# 这次拷贝 —— 正是 traps/test-dll-copy-loop.md 记录的"测试加载旧 DLL"陷阱。
# $<TARGET_FILE:World> 是 target-based generator expression,CMake 会自动建立
# "先构建 World"的目标级依赖;目标目录用 ${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>,
# 即本目录下测试 exe 的默认输出目录(与每个 $<TARGET_FILE_DIR:...> 相同)。
add_custom_target(WeCopyRuntimeDllForTests
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:World>"
        "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    COMMENT "Copying WorldRuntime.dll next to test exes"
    VERBATIM
)

foreach(_we_test_target WorldFrameTimeTests WorldSchemaTests WorldWuiTests WorldWuiModalTests WorldWuiTextBufferTests WorldLuauHighlighterTests WorldLuauCompletionTests WorldWuiCompletionTests WorldVfsTests WorldAssetTests WorldRhiContractTests WorldVulkanDeviceTests WorldShaderPipelineTests WorldTextureImportTests WorldTextureRuntimeTests WorldRendererHotswapTests WorldMemoryTests WorldTaskTests WorldGameplayTests WorldLevelTests WorldMeshTests WorldMaterialTests WorldLightingTests WorldModelTests WorldAnimationTests WorldPhysics3DTests WorldPhysicsEventsTests WorldPhysicsJointTests WorldTransformInterpolationTests WorldHierarchyTests WorldQueryTests WorldLuauEcsTests WorldPureEcsExampleTests WorldEventTests WorldSystemTests WorldPrefabTests WorldInputTests WorldSaveTests WorldBehaviorTests WorldProjectSystemsHookTests WorldRuntimeContractTests WorldLuauVmTests WorldLuauBindingTests WorldLuauStubSchemaTests WorldLuauSandboxTests WorldScriptArtifactTests WorldScriptWorkflowTests WorldAssetTypeRegistryTests WorldStringPoolTests WorldAssetRegistryTests WorldAssetIdentityTests WorldAsyncLoaderTests WorldTextureAsyncTests WorldImportWorldSettingsTests WorldPluginTests WorldTelemetryTests WorldUiDocumentTests WorldUiPaintTests WorldUiNavInputTests WorldUiBindAnimTests WorldUiWorldTests WorldUiScrollTests WorldUiPointerGateTests WorldUiInputInteractTests WorldUiThemeTests WorldUiBindingSourcesTests WorldUiPropertyCoverageTests WorldUiHotReloadTests ${_we_optional_test_targets})
    add_dependencies(${_we_test_target} WeCopyRuntimeDllForTests)
endforeach()
