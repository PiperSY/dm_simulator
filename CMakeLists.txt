cmake_minimum_required(VERSION 3.16)

project(dm_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

add_library(dm_sim_core
    src/sim/event.cpp
    src/sim/scheduler.cpp
    src/sim/simulator.cpp
    src/nodes/compute_node.cpp
    src/nodes/memory_node.cpp
    src/metrics/stats.cpp
)

target_include_directories(dm_sim_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

add_executable(dm_simulator
    src/main.cpp
)

target_link_libraries(dm_simulator
    PRIVATE
        dm_sim_core
)

add_executable(test_scheduler
    tests/test_scheduler.cpp
)

target_link_libraries(test_scheduler
    PRIVATE
        dm_sim_core
)

add_test(NAME scheduler COMMAND test_scheduler)

add_executable(test_request_path
    tests/test_request_path.cpp
)

target_link_libraries(test_request_path
    PRIVATE
        dm_sim_core
)

add_test(NAME request_path COMMAND test_request_path)
