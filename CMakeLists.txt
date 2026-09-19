cmake_minimum_required(VERSION 3.20)

project(BufferFabric
    VERSION 1.0.0
    DESCRIPTION "Vendor-neutral C++20 runtime for generation-bound governance of shared packet-buffer capacity"
    HOMEPAGE_URL "https://github.com/summonlabs/Buffer-Fabric"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    set(BUFFER_FABRIC_IS_TOP_LEVEL ON)
else()
    set(BUFFER_FABRIC_IS_TOP_LEVEL OFF)
endif()

option(BUFFER_FABRIC_BUILD_TOOLS  "Build bfctl / bfbench / bf_coordinator / bf_worker" ${BUFFER_FABRIC_IS_TOP_LEVEL})
option(BUFFER_FABRIC_BUILD_TESTS  "Build the Buffer Fabric test suite"                  ${BUFFER_FABRIC_IS_TOP_LEVEL})
option(BUFFER_FABRIC_INSTALL      "Generate install and CMake package export rules"     ON)
option(BUFFER_FABRIC_STRICT       "Treat compiler warnings as errors"                   ON)
option(BUFFER_FABRIC_WARNINGS     "Enable a strict warning set"                         ON)
set(BUFFER_FABRIC_SANITIZE "" CACHE STRING
    "Sanitizer to enable (empty, address). Only 'address' is supported by MSVC.")
set_property(CACHE BUFFER_FABRIC_SANITIZE PROPERTY STRINGS "" "address")
option(BUFFER_FABRIC_ANALYZE      "Run the platform static analyzer on first-party sources" OFF)

# ---------------------------------------------------------------------------
# Global language settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Warning / hardening interface
# ---------------------------------------------------------------------------
add_library(buffer_fabric_warnings INTERFACE)
add_library(BufferFabric::warnings ALIAS buffer_fabric_warnings)

if(MSVC)
    target_compile_options(buffer_fabric_warnings INTERFACE
        /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor /MP
        /wd4127  # conditional expression is constant (used deliberately in static_assert helpers)
    )
    if(BUFFER_FABRIC_WARNINGS)
        target_compile_options(buffer_fabric_warnings INTERFACE
            /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546 /w14547
            /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928)
    endif()
    if(BUFFER_FABRIC_STRICT AND NOT BUFFER_FABRIC_ANALYZE)
        target_compile_options(buffer_fabric_warnings INTERFACE /WX)
    endif()
    target_compile_definitions(buffer_fabric_warnings INTERFACE
        _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(buffer_fabric_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
        -Wunused -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
        -Wformat=2 -Wimplicit-fallthrough)
    if(BUFFER_FABRIC_STRICT)
        target_compile_options(buffer_fabric_warnings INTERFACE -Werror)
    endif()
endif()

if(BUFFER_FABRIC_SANITIZE STREQUAL "address")
    if(MSVC)
        target_compile_options(buffer_fabric_warnings INTERFACE /fsanitize=address /Zi)
        target_link_options(buffer_fabric_warnings INTERFACE /INCREMENTAL:NO)
    else()
        target_compile_options(buffer_fabric_warnings INTERFACE -fsanitize=address -fno-omit-frame-pointer -g)
        target_link_options(buffer_fabric_warnings INTERFACE -fsanitize=address)
    endif()
elseif(NOT BUFFER_FABRIC_SANITIZE STREQUAL "")
    message(FATAL_ERROR "Unsupported BUFFER_FABRIC_SANITIZE value: '${BUFFER_FABRIC_SANITIZE}'")
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(buffer_fabric
    src/fs.cpp
    src/status.cpp
    src/time.cpp
    src/json.cpp
    src/codec.cpp
    src/policy.cpp
    src/topology.cpp
    src/accounting.cpp
    src/authority.cpp
    src/decision.cpp
    src/backend.cpp
    src/observation.cpp
    src/explain.cpp
    src/persistence.cpp
    src/fabric.cpp
    src/fabric_topology.cpp
    src/fabric_alloc.cpp
    src/fabric_query.cpp
    src/transport.cpp
    src/protocol.cpp
    src/process.cpp
    src/coordinator.cpp
    src/worker.cpp
)
add_library(BufferFabric::buffer_fabric ALIAS buffer_fabric)

target_include_directories(buffer_fabric
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The warning interface is a build-time-only dependency: it must not leak into
# the exported package, because consumers compile with their own warning set.
target_link_libraries(buffer_fabric
    PUBLIC Threads::Threads
    PRIVATE $<BUILD_INTERFACE:buffer_fabric_warnings>)

target_compile_definitions(buffer_fabric PRIVATE BUFFER_FABRIC_BUILDING_LIBRARY=1)

if(WIN32)
    target_link_libraries(buffer_fabric PRIVATE ws2_32)
endif()

set_target_properties(buffer_fabric PROPERTIES
    OUTPUT_NAME buffer_fabric
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(BUFFER_FABRIC_ANALYZE AND MSVC)
    target_compile_options(buffer_fabric PRIVATE /analyze /analyze:external- /wd6001 /wd6011)
endif()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(BUFFER_FABRIC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUFFER_FABRIC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
if(BUFFER_FABRIC_INSTALL)
    include(CMakePackageConfigHelpers)
    include(GNUInstallDirs)

    install(TARGETS buffer_fabric
        EXPORT BufferFabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    if(BUFFER_FABRIC_BUILD_TOOLS)
        install(TARGETS bfctl bfbench bf_coordinator bf_worker
            EXPORT BufferFabricTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    install(EXPORT BufferFabricTargets
        FILE BufferFabricTargets.cmake
        NAMESPACE BufferFabric::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BufferFabric)

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BufferFabricConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/BufferFabricConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BufferFabric)

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/BufferFabricConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/BufferFabricConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/BufferFabricConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/BufferFabric)

    install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DATADIR}/BufferFabric)
endif()

message(STATUS "BufferFabric ${PROJECT_VERSION} | build type=${CMAKE_BUILD_TYPE} | tests=${BUFFER_FABRIC_BUILD_TESTS} | tools=${BUFFER_FABRIC_BUILD_TOOLS} | sanitize='${BUFFER_FABRIC_SANITIZE}'")
