cmake_minimum_required(VERSION 3.16)

project(dm_simulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
find_package(yaml-cpp REQUIRED)

if(TARGET yaml-cpp::yaml-cpp)
    set(DM_SIM_YAML_TARGET yaml-cpp::yaml-cpp)
else()
    set(DM_SIM_YAML_TARGET yaml-cpp)
endif()

add_library(dm_sim_core
    src/sim/event.cpp
    src/sim/scheduler.cpp
    src/sim/simulator.cpp
    src/sim/config_loader.cpp
    src/cache/local_cache.cpp
    src/cache/lru_policy.cpp
    src/cache/hotness_policy.cpp
    src/cache/contention_policy.cpp
    src/nodes/compute_node.cpp
    src/nodes/memory_node.cpp
    src/metrics/stats.cpp
    src/metrics/histogram.cpp
    src/workloads/workload.cpp
    src/workloads/generators.cpp
    src/experiments/runner.cpp
)

target_include_directories(dm_sim_core
    PUBLIC
        ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(dm_sim_core
    PUBLIC
        ${DM_SIM_YAML_TARGET}
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

add_executable(test_cache
    tests/test_cache.cpp
)

target_link_libraries(test_cache
    PRIVATE
        dm_sim_core
)

add_test(NAME cache COMMAND test_cache)

add_executable(test_workload
    tests/test_workload.cpp
)

target_link_libraries(test_workload
    PRIVATE
        dm_sim_core
)

add_test(NAME workload COMMAND test_workload)

add_executable(test_config_loader
    tests/test_config_loader.cpp
)

target_link_libraries(test_config_loader
    PRIVATE
        dm_sim_core
)

add_test(NAME config_loader COMMAND test_config_loader)

add_executable(test_metrics
    tests/test_metrics.cpp
)

target_link_libraries(test_metrics
    PRIVATE
        dm_sim_core
)

add_test(NAME metrics COMMAND test_metrics)

add_executable(test_experiment_runner
    tests/test_experiment_runner.cpp
)

target_link_libraries(test_experiment_runner
    PRIVATE
        dm_sim_core
)

add_test(NAME experiment_runner COMMAND test_experiment_runner)

add_test(
    NAME experiment_matrix_dry_run
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/scripts/run_matrix.py
        --dry-run
        --results-dir
        ${PROJECT_BINARY_DIR}/matrix_dry_run
)

add_test(
    NAME experiment_matrix_phase_b_dry_run
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/scripts/run_matrix.py
        --preset
        phase_b
        --policies
        lru
        --dry-run
        --expect-runs
        120
        --results-dir
        ${PROJECT_BINARY_DIR}/matrix_phase_b_dry_run
)

add_test(
    NAME experiment_matrix_phase_c_dry_run
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/scripts/run_matrix.py
        --preset
        phase_c
        --policies
        lru
        --dry-run
        --expect-runs
        12
        --results-dir
        ${PROJECT_BINARY_DIR}/matrix_phase_c_dry_run
)
