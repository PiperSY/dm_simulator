#pragma once

#include <deque>
#include <unordered_map>

#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "sim/config.hpp"
#include "sim/event.hpp"

namespace dm_sim {

class Scheduler;

/*********************************** 
 * MemoryNode class represents the memory component in the simulation. It handles events related to memory access, including 
 * forwarding requests to memory, starting memory service, and completing memory service. The MemoryNode maintains a queue of 
 * pending requests and interacts with the Scheduler to manage event timing and execution.
 ***********************************/
class MemoryNode {
public:
    MemoryNode(NodeId node_id,
               const SimulationConfig& config,
               Stats& stats,
               std::unordered_map<RequestId, Request>& request_table);

    void handle_event(const Event& event, Scheduler& scheduler);

private:
    void handle_forward_to_memory(const Event& event, Scheduler& scheduler);
    void handle_memory_service_start(const Event& event, Scheduler& scheduler);
    void handle_memory_service_complete(const Event& event, Scheduler& scheduler);
    [[nodiscard]] SimTime service_time_for(const Request& request) const noexcept;

    NodeId node_id_;
    const SimulationConfig& config_;
    Stats& stats_;
    std::unordered_map<RequestId, Request>& request_table_;
    std::deque<RequestId> queued_requests_;
    bool service_in_progress_ = false;
    bool service_start_scheduled_ = false;
};

}  // namespace dm_sim
