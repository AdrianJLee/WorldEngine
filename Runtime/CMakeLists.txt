project(Runtime)

add_executable(Runtime)

if(WIN32)
    target_compile_definitions(Runtime PRIVATE
        WLD_PLATFORM_WINDOWS
    )

    if(MSVC)
        target_compile_options(Runtime PRIVATE "/utf-8")
    endif()

endif()


target_compile_definitions(Runtime PRIVATE 
    _CRT_SECURE_NO_WARNINGS
    YAML_CPP_STATIC_DEFINE
    WLD_CURRENT_DIR="${CMAKE_SOURCE_DIR}/Runtime/"
)



file(GLOB_RECURSE Runtime_SOURCES 
    "src/*.h"
    "src/*.cpp"

)
target_sources(Runtime PRIVATE ${Runtime_SOURCES})

target_link_libraries(Runtime PRIVATE 
    World
)

