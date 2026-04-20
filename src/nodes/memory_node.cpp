#include "nodes/memory_node.hpp"

#include <stdexcept>

#include "sim/scheduler.hpp"

namespace dm_sim {

MemoryNode::MemoryNode(NodeId node_id,
                       const SimulationConfig& config,
                       std::unordered_map<RequestId, Request>& request_table)
    : node_id_(node_id), config_(config), request_table_(request_table) {}

void MemoryNode::handle_event(const Event& event, Scheduler& scheduler) {
    if (event.type != EventType::ForwardToMemory) {
        throw std::logic_error("MemoryNode received unsupported event type");
    }

    Request& request = request_table_.at(event.request_id);
    request.current_stage = RequestStage::WaitingForResponse;

    const SimTime service_time = service_time_for(request);
    const SimTime response_time =
        event.time + service_time + config_.one_way_link_latency;

    scheduler.schedule(Event(response_time,
                             EventType::ReturnResponse,
                             request.source_node_id,
                             request.request_id));
}

SimTime MemoryNode::service_time_for(const Request& request) const noexcept {
    const std::uint64_t bandwidth = config_.memory_bandwidth_bytes_per_time;
    const SimTime serialization_delay =
        static_cast<SimTime>((request.size_bytes + bandwidth - 1) / bandwidth);

    return config_.memory_base_latency + serialization_delay;
}

}  // namespace dm_sim
