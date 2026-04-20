#pragma once

#include <cstdint>
#include <vector>

#include "model/request.hpp"
#include "sim/types.hpp"

namespace dm_sim {

struct SimulationConfig {
    NodeId compute_node_id = 1;
    NodeId memory_node_id = 2;
    SimTime one_way_link_latency = 5;
    SimTime memory_base_latency = 20;
    std::uint64_t memory_bandwidth_bytes_per_time = 16;
    std::vector<RequestSpec> requests;
};

}  // namespace dm_sim
