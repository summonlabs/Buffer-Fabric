# Buffer Fabric command line tools.

add_executable(bfctl bfctl.cpp)
target_link_libraries(bfctl PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)

add_executable(bfbench bfbench.cpp)
target_link_libraries(bfbench PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)

add_executable(bf_coordinator bf_coordinator.cpp)
target_link_libraries(bf_coordinator PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)

add_executable(bf_worker bf_worker.cpp)
target_link_libraries(bf_worker PRIVATE buffer_fabric $<BUILD_INTERFACE:buffer_fabric_warnings>)
