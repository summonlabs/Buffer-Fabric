# Buffer Fabric test suite.
#
# Every test executable is registered with CTest without any timeout property:
# a hang is a defect to diagnose, not something to terminate.

add_executable(bf_tests
    test_main.cpp
    test_support.cpp
    test_core.cpp
    test_fabric.cpp
    test_pressure_reclaim.cpp
    test_persistence.cpp
    test_property.cpp
    test_concurrency.cpp
    test_adversarial.cpp
)
target_link_libraries(bf_tests PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)
target_include_directories(bf_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME bf_tests COMMAND bf_tests)

if(BUFFER_FABRIC_BUILD_TOOLS)
    add_executable(bf_mp_tests
        test_main.cpp
        test_support.cpp
        test_multiprocess.cpp
    )
    target_link_libraries(bf_mp_tests PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)
    target_include_directories(bf_mp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    # The multiprocess suite launches the real coordinator and worker binaries.
    target_compile_definitions(bf_mp_tests PRIVATE
        BF_COORDINATOR_EXE="$<TARGET_FILE:bf_coordinator>"
        BF_WORKER_EXE="$<TARGET_FILE:bf_worker>")
    add_test(NAME bf_mp_tests COMMAND bf_mp_tests)
endif()
