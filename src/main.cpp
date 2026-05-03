#include <iostream>

#include "sim/config.hpp"
#include "sim/simulator.hpp"
#include "workloads/workload.hpp"

int main() {
    dm_sim::SyntheticWorkloadConfig workload;
    workload.seed = 2026;
    workload.compute_node_ids = {1, 2};
    workload.object_count = 32;
    workload.object_size_bytes = 32;
    workload.requests_per_node_per_epoch = 4;
    workload.epoch_count = 2;
    workload.hot_set_size = 4;
    workload.hot_access_probability = 0.85;
    workload.hot_set_mode = dm_sim::HotSetMode::EpochShift;
    workload.cross_node_overlap = dm_sim::CrossNodeOverlap::High;

    dm_sim::SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 5;
    config.memory_base_latency = 20;
    config.memory_bandwidth_bytes_per_time = 16;
    config.local_cache = dm_sim::LocalCacheConfig{
        128,
        1,
        dm_sim::LocalCachePolicyType::Lru,
    };
    config.synthetic_workload = workload;

    dm_sim::Simulator simulator(config);
    simulator.run();

    std::cout << "Completed requests: "
              << simulator.stats().completed_requests() << "\n";
    std::cout << "Average latency: " << simulator.stats().average_latency() << "\n";
    std::cout << "Memory average wait: "
              << simulator.stats().average_memory_wait() << "\n";
    std::cout << "Local cache hit rate: "
              << simulator.stats().local_cache_hit_rate() << "\n";

    return 0;
}
