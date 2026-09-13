add_subdirectory(vendor/GLFW)
add_subdirectory(vendor/Glad)
add_subdirectory(vendor/imgui)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/yaml)
add_subdirectory(vendor/spdlog)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SPIRV-Cross)
# Box2D 以 OBJECT 库并入 World（W4）：box2d 是带全局 id 池（b2_worldIdPool）
# 的 C 库，若以静态库形式被 World 和消费者各链接一份，跨 DLL 的
# b2Body_IsValid 等接口会查各自模块的 id 池而失效。以 OBJECT 形式并入
# World 后，符号经 WINDOWS_EXPORT_ALL_SYMBOLS 从 WorldRuntime.dll 导出，
# 保证进程内只有一份 box2d 状态。
set(WE_BOX2D_SOURCES
    vendor/box2d/src/aabb.c
    vendor/box2d/src/arena_allocator.c
    vendor/box2d/src/array.c
    vendor/box2d/src/bitset.c
    vendor/box2d/src/body.c
    vendor/box2d/src/broad_phase.c
    vendor/box2d/src/constraint_graph.c
    vendor/box2d/src/contact.c
    vendor/box2d/src/contact_solver.c
    vendor/box2d/src/core.c
    vendor/box2d/src/distance.c
    vendor/box2d/src/distance_joint.c
    vendor/box2d/src/dynamic_tree.c
    vendor/box2d/src/geometry.c
    vendor/box2d/src/hull.c
    vendor/box2d/src/id_pool.c
    vendor/box2d/src/island.c
    vendor/box2d/src/joint.c
    vendor/box2d/src/manifold.c
    vendor/box2d/src/math_functions.c
    vendor/box2d/src/motor_joint.c
    vendor/box2d/src/mover.c
    vendor/box2d/src/parallel_for.c
    vendor/box2d/src/physics_world.c
    vendor/box2d/src/prismatic_joint.c
    vendor/box2d/src/revolute_joint.c
    vendor/box2d/src/scheduler.c
    vendor/box2d/src/sensor.c
    vendor/box2d/src/shape.c
    vendor/box2d/src/solver.c
    vendor/box2d/src/solver_set.c
    vendor/box2d/src/table.c
    vendor/box2d/src/timer.c
    vendor/box2d/src/types.c
    vendor/box2d/src/weld_joint.c
    vendor/box2d/src/wheel_joint.c
)
add_library(WorldBox2d OBJECT ${WE_BOX2D_SOURCES})
target_include_directories(WorldBox2d
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/vendor/box2d/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/box2d/src"
)
set_target_properties(WorldBox2d PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS YES
)
if(MSVC)
    target_compile_options(WorldBox2d PRIVATE /Wall /wd4820 /wd5045 /wd4061 /wd4711 /wd4710)
endif()

add_subdirectory(vendor/sol2)
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
# 针对 ImGuizmo 禁用 PCH (类似你的 Premake filter)
set_source_files_properties(vendor/ImGuizmo/ImGuizmo.cpp PROPERTIES SKIP_PRECOMPILED_HEADERS ON)

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
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/World/"
)
target_compile_definitions(World PUBLIC 
    WLD_CORE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/"
    WLD_ROOT_DIR="${CMAKE_SOURCE_DIR}/"
    WLD_INTERMEDIATE_DIR="${CMAKE_BINARY_DIR}/intermediate/"
)

# 头文件路径
target_include_directories(World PRIVATE 
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/GLFW/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/Glad/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb_image"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/VMA/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/vulkan-headers/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/volk"
    ${Vulkan_INCLUDE_DIRS} # 自动解析的 Vulkan 路径
)
target_include_directories(World PUBLIC 
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/glm"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/entt/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/yaml/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/ImGuizmo"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SPIRV-Cross"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/SPIRV-Cross/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/spdlog/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/box2d/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/box2d/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/sol2/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/lua/src"
)

# 递归获取源文件
file(GLOB_RECURSE WORLD_SOURCES 
    "src/*.h"
    "src/*.cpp"
    "vendor/stb_image/*.h"
    "vendor/stb_image/*.cpp"
    "vendor/glm/glm/**.hpp"
	"vendor/glm/glm/**.inl"
    "vendor/ImGuizmo/ImGuizmo.h"
    "vendor/ImGuizmo/ImGuizmo.cpp"
    "vendor/sol2/include/**.hpp"
    "vendor/lua/src/**.hpp"
    "vendor/lua/src/**.c"
    "vendor/volk/*.c"
    "vendor/volk/*.h"

)

# 添加源文件到 target
target_sources(World PRIVATE ${WORLD_SOURCES} $<TARGET_OBJECTS:WorldBox2d>)

# 链接库 (Vulkan::Vulkan 解决了 vulkan-1.lib 路径问题)
target_link_libraries(World PRIVATE
    glfw    # 对应子目录生成的 target 名
    Glad
    opengl32
    sol2
    yaml-cpp
    ImGui
    spdlog::spdlog
)
