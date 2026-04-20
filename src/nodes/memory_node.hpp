#pragma once

#include <unordered_map>

#include "model/request.hpp"
#include "sim/config.hpp"
#include "sim/event.hpp"

namespace dm_sim {

class Scheduler;

class MemoryNode {
public:
    MemoryNode(NodeId node_id,
               const SimulationConfig& config,
               std::unordered_map<RequestId, Request>& request_table);

    void handle_event(const Event& event, Scheduler& scheduler);

private:
    [[nodiscard]] SimTime service_time_for(const Request& request) const noexcept;

    NodeId node_id_;
    const SimulationConfig& config_;
    std::unordered_map<RequestId, Request>& request_table_;
};

}  // namespace dm_sim
