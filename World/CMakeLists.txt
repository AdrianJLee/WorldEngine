add_subdirectory(vendor/GLFW)
add_subdirectory(vendor/Glad)
add_subdirectory(vendor/imgui)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/yaml)
add_subdirectory(vendor/spdlog)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/SPIRV-Cross)
add_subdirectory(vendor/box2d)
add_library(World STATIC)

# 预编译头
target_precompile_headers(World PRIVATE src/wldpch.h)
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
        target_compile_options(World PRIVATE "/utf-8")
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
)
# 添加源文件到 target
target_sources(World PRIVATE ${WORLD_SOURCES})


# 链接库 (Vulkan::Vulkan 解决了 vulkan-1.lib 路径问题)
target_link_libraries(World PRIVATE 
    glfw    # 对应子目录生成的 target 名
    Glad
    opengl32
    Vulkan::Vulkan
    box2d
)
target_link_libraries(World PUBLIC 
    yaml-cpp
    ImGui
    spdlog::spdlog
)