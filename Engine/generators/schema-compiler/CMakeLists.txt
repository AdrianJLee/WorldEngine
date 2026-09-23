cmake_minimum_required(VERSION 3.16)

project(WorldEngineSchemaCompiler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(schema-compiler main.cpp)

if(MSVC)
    target_compile_options(schema-compiler PRIVATE "/utf-8")
endif()
