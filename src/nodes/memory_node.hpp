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

    // Handle incoming events directed to the MemoryNode, dispatching them to the appropriate handler based on the event type.
    void handle_event(const Event& event, Scheduler& scheduler);

private:
    // Handlers for specific event types related to memory access, including forwarding requests to memory, starting memory service, and completing memory service.
    void handle_forward_to_memory(const Event& event, Scheduler& scheduler);
    void handle_memory_service_start(const Event& event, Scheduler& scheduler);
    void handle_memory_service_complete(const Event& event, Scheduler& scheduler);
    [[nodiscard]] SimTime service_time_for(const Request& request) const noexcept;

    NodeId node_id_;
    const SimulationConfig& config_;                            // Reference to the simulation configuration, allowing the MemoryNode to access parameters such as memory latency and bandwidth.
    Stats& stats_;                                              // Reference to the Stats object for recording metrics related to memory access and contention.
    std::unordered_map<RequestId, Request>& request_table_;     // Reference to the global request table, allowing the MemoryNode to access and update request information during event handling.
    std::deque<RequestId> queued_requests_;                     // Queue of pending requests waiting for memory service, maintained in FIFO order to ensure fair servicing of requests.
    bool service_in_progress_ = false;                          // Flags to track whether a memory service start event has been scheduled or is in progress, 
    bool service_start_scheduled_ = false;                      //   preventing multiple concurrent service start events from being scheduled when the queue is not empty.    
};

}  // namespace dm_sim
