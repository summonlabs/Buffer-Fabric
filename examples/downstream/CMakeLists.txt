cmake_minimum_required(VERSION 3.20)

# An independent downstream consumer of the installed Buffer Fabric package.
#
# This project deliberately shares no build tree with Buffer Fabric: it is
# configured against an installed prefix and consumes only the exported CMake
# package and the public headers.
project(BufferFabricDownstream
    VERSION 1.0.0
    DESCRIPTION "Independent find_package consumer of Buffer Fabric"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(BufferFabric 1.0 REQUIRED CONFIG)

message(STATUS "BufferFabric found: version ${BufferFabric_VERSION}")

add_executable(downstream main.cpp)
target_link_libraries(downstream PRIVATE BufferFabric::buffer_fabric)

if(MSVC)
    target_compile_options(downstream PRIVATE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(downstream PRIVATE -Wall -Wextra -Werror)
endif()
