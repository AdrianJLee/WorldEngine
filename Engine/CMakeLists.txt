set(WE_THIRD_PARTY "${CMAKE_SOURCE_DIR}/third_party")

add_subdirectory(${WE_THIRD_PARTY}/GLFW ${CMAKE_BINARY_DIR}/third_party/GLFW)
add_subdirectory(${WE_THIRD_PARTY}/Glad ${CMAKE_BINARY_DIR}/third_party/Glad)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${WE_THIRD_PARTY}/yaml ${CMAKE_BINARY_DIR}/third_party/yaml)
add_subdirectory(${WE_THIRD_PARTY}/spdlog ${CMAKE_BINARY_DIR}/third_party/spdlog)
# Box2D 以 OBJECT 库并入 World（W4）：box2d 是带全局 id 池（b2_worldIdPool）
# 的 C 库，若以静态库形式被 World 和消费者各链接一份，跨 DLL 的
# b2Body_IsValid 等接口会查各自模块的 id 池而失效。以 OBJECT 形式并入
# World 后，符号经 WINDOWS_EXPORT_ALL_SYMBOLS 从 WorldRuntime.dll 导出，
# 保证进程内只有一份 box2d 状态。
set(WE_BOX2D_SOURCES
    ${WE_THIRD_PARTY}/box2d/src/aabb.c
    ${WE_THIRD_PARTY}/box2d/src/arena_allocator.c
    ${WE_THIRD_PARTY}/box2d/src/array.c
    ${WE_THIRD_PARTY}/box2d/src/bitset.c
    ${WE_THIRD_PARTY}/box2d/src/body.c
    ${WE_THIRD_PARTY}/box2d/src/broad_phase.c
    ${WE_THIRD_PARTY}/box2d/src/constraint_graph.c
    ${WE_THIRD_PARTY}/box2d/src/contact.c
    ${WE_THIRD_PARTY}/box2d/src/contact_solver.c
    ${WE_THIRD_PARTY}/box2d/src/core.c
    ${WE_THIRD_PARTY}/box2d/src/distance.c
    ${WE_THIRD_PARTY}/box2d/src/distance_joint.c
    ${WE_THIRD_PARTY}/box2d/src/dynamic_tree.c
    ${WE_THIRD_PARTY}/box2d/src/geometry.c
    ${WE_THIRD_PARTY}/box2d/src/hull.c
    ${WE_THIRD_PARTY}/box2d/src/id_pool.c
    ${WE_THIRD_PARTY}/box2d/src/island.c
    ${WE_THIRD_PARTY}/box2d/src/joint.c
    ${WE_THIRD_PARTY}/box2d/src/manifold.c
    ${WE_THIRD_PARTY}/box2d/src/math_functions.c
    ${WE_THIRD_PARTY}/box2d/src/motor_joint.c
    ${WE_THIRD_PARTY}/box2d/src/mover.c
    ${WE_THIRD_PARTY}/box2d/src/parallel_for.c
    ${WE_THIRD_PARTY}/box2d/src/physics_world.c
    ${WE_THIRD_PARTY}/box2d/src/prismatic_joint.c
    ${WE_THIRD_PARTY}/box2d/src/revolute_joint.c
    ${WE_THIRD_PARTY}/box2d/src/scheduler.c
    ${WE_THIRD_PARTY}/box2d/src/sensor.c
    ${WE_THIRD_PARTY}/box2d/src/shape.c
    ${WE_THIRD_PARTY}/box2d/src/solver.c
    ${WE_THIRD_PARTY}/box2d/src/solver_set.c
    ${WE_THIRD_PARTY}/box2d/src/table.c
    ${WE_THIRD_PARTY}/box2d/src/timer.c
    ${WE_THIRD_PARTY}/box2d/src/types.c
    ${WE_THIRD_PARTY}/box2d/src/weld_joint.c
    ${WE_THIRD_PARTY}/box2d/src/wheel_joint.c
)
add_library(WorldBox2d OBJECT ${WE_BOX2D_SOURCES})
target_include_directories(WorldBox2d
    PUBLIC "${CMAKE_SOURCE_DIR}/third_party/box2d/include"
    PRIVATE "${CMAKE_SOURCE_DIR}/third_party/box2d/src"
)
set_target_properties(WorldBox2d PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)
if(MSVC)
    target_compile_options(WorldBox2d PRIVATE /Wall /wd4820 /wd5045 /wd4061 /wd4711 /wd4710)
endif()

# P1b D5:glTF 2.0 导入(cgltf,锁定 v1.15)。单头库 + 本地包装源,与 stb_image 同款
# "普通 vendor 目录"做法(不进 submodule);include 只暴露给 World。
add_library(cgltf STATIC ${WE_THIRD_PARTY}/cgltf/cgltf.c)
target_include_directories(cgltf PUBLIC "${CMAKE_SOURCE_DIR}/third_party/cgltf")
set_target_properties(cgltf PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED YES
)

# P1b D6:3D 物理(Jolt,submodule 锁定 v5.6.0)。只编库,不编 samples/viewer/测试:
#  - Jolt 作为子目录引入时自己就会跳过 UnitTests/HelloWorld/PerformanceTest/Samples/JoltViewer
#    (这些 option 只在它是 root project 时才声明),这里仍显式钉住缓存值,防止上游改判定;
#  - INTERPROCEDURAL_OPTIMIZATION OFF:不让 Jolt 往本项目打开 LTO;
#  - USE_STATIC_MSVC_RUNTIME_LIBRARY OFF:Jolt 必须与本工程一致用 /MD(dll CRT),否则
#    Ref<>/Array/异常跨 CRT 堆分配释放会踩内存;
#  - JPH_USE_DX12/VK/MTL/CPU_COMPUTE OFF:D6 只做刚体物理,不需要 Jolt 的 compute 后端;
#    关掉后构建期不依赖 D3D12/Metal 的着色器工具链,也不会往 vendor 源码树写 .dxil/.spv。
set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
add_subdirectory(${WE_THIRD_PARTY}/Jolt/Build ${CMAKE_BINARY_DIR}/third_party/Jolt)

# P2 脚本前端:Luau(只编库,不编 CLI/tests/Web)。sol2 + PUC Lua 已在 W1b 移除。
set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
add_subdirectory(${WE_THIRD_PARTY}/luau ${CMAKE_BINARY_DIR}/third_party/luau)
add_library(World SHARED)
set_target_properties(World PROPERTIES
    OUTPUT_NAME "WorldRuntime"
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

# 预编译头
# W4 说明：World 改为 SHARED + WINDOWS_EXPORT_ALL_SYMBOLS 后，PCH 对象
# (cmake_pch.cxx.obj / cmake_pch.c.obj) 的 __@@_PchSym_ 符号会让链接器在
# 生成 exports.def 时误导出并报 LNK2001 "无法解析的外部符号 __"。暂时关闭
# PCH（wldpch.h 仍作为普通头文件被各源文件 include），后续可用显式 .def
# 方案恢复。
# target_precompile_headers(World PRIVATE src/wldpch.h)

# 用 /FI 强制包含替代 PCH：既保持各 TU 的传递头文件语义，又不产生
# cmake_pch.*.obj 参与导出表生成。
if(MSVC)
    target_compile_options(World PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/src/wldpch.h" "/FS")
endif()

if(WIN32)
    # 1. 宏定义 (defines)
    target_compile_definitions(World PRIVATE
        WLD_PLATFORM_WINDOWS
        WLD_BUILD_DLL
        GLFW_INCLUDE_NONE
    )

    # 2. 编译选项 (buildoptions)
    # /utf-8 仅适用于 MSVC 编译器，建议加上编译器判断
    if(MSVC)
        target_compile_options(World PRIVATE "/utf-8" "/bigobj")
    else()
    endif()

    # 3. 系统版本 (systemversion "latest")
    # CMake 默认会使用安装的最新的 Windows SDK。
    # 如果要显式指定，通常在命令行执行 cmake 时传入：
    # cmake -DCMAKE_SYSTEM_VERSION=10.0 ...
    # 或者在顶层 CMakeLists.txt 中设置：
    # set(CMAKE_SYSTEM_VERSION 10.0) 
endif()

# 宏定义
target_compile_definitions(World PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    GLFW_INCLUDE_VULKAN
    NOMINMAX
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/Engine/"
)
target_compile_definitions(World PUBLIC 
    WLD_CORE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
    WLD_ROOT_DIR="${CMAKE_SOURCE_DIR}/"
    WLD_INTERMEDIATE_DIR="${CMAKE_BINARY_DIR}/intermediate/"
)

# 头文件路径
target_include_directories(World PRIVATE 
    "${CMAKE_SOURCE_DIR}/third_party/GLFW/include"
    "${CMAKE_SOURCE_DIR}/third_party/Glad/include"
    "${CMAKE_SOURCE_DIR}/third_party/stb_image"
    "${CMAKE_SOURCE_DIR}/third_party/stb"
    "${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include"
    "${CMAKE_SOURCE_DIR}/third_party/volk"
    ${Vulkan_INCLUDE_DIRS} # 自动解析的 Vulkan 路径
)
target_include_directories(World PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/third_party/glm"
    "${CMAKE_SOURCE_DIR}/third_party/entt/include"
    "${CMAKE_SOURCE_DIR}/third_party/yaml/include"
    "${CMAKE_SOURCE_DIR}/third_party/spdlog/include"
    "${CMAKE_SOURCE_DIR}/third_party/box2d/src"
    "${CMAKE_SOURCE_DIR}/third_party/box2d/include"
)

# 递归获取源文件
file(GLOB_RECURSE WORLD_SOURCES 
    "src/*.h"
    "src/*.cpp"
    "${WE_THIRD_PARTY}/stb_image/*.h"
    "${WE_THIRD_PARTY}/stb_image/*.cpp"
    "${WE_THIRD_PARTY}/glm/glm/**.hpp"
	"${WE_THIRD_PARTY}/glm/glm/**.inl"
    "${WE_THIRD_PARTY}/volk/*.c"
    "${WE_THIRD_PARTY}/volk/*.h"

)

# 添加源文件到 target
target_sources(World PRIVATE ${WORLD_SOURCES} $<TARGET_OBJECTS:WorldBox2d>)

# volk 需要在启用 Win32 平台宏时编译,才会导出 VkWin32SurfaceCreateInfoKHR /
# vkCreateWin32SurfaceKHR 等平台符号(VulkanSwapchain 用它们创建表面)。
set_source_files_properties("${CMAKE_SOURCE_DIR}/third_party/volk/volk.c"
    PROPERTIES COMPILE_DEFINITIONS "VK_USE_PLATFORM_WIN32_KHR")

# 链接库 (Vulkan::Vulkan 解决了 vulkan-1.lib 路径问题)
target_link_libraries(World PRIVATE
    glfw    # 对应子目录生成的 target 名
    Glad
    opengl32
    dwmapi
    # WindowsWindow/WindowsPlatformUtils 的 IME 接口(ImmGetContext 等);
    # 此前由已移除的第三方 UI 依赖传递链接,现需显式声明。
    imm32
    cgltf
    Luau.VM
    Luau.Compiler
    yaml-cpp
    spdlog::spdlog
    # P1b D6:3D 物理(静态库,符号随 WorldRuntime.dll 导出;Jolt 类型不出 Physics3D.cpp)。
    Jolt
)

# P2 W3a:schema 生成器(纯 C++17 小工具,不参与产品二进制)。
# 生成物提交进仓库,产品构建不依赖本工具;它的用途是 --check 门禁(tests 里的
# World.SchemaDrift)——字段注解改了但没重跑生成器时直接红灯。
add_subdirectory(generators/schema-compiler)
