#include "nodes/memory_node.hpp"

#include <stdexcept>

#include "sim/scheduler.hpp"

namespace dm_sim {

MemoryNode::MemoryNode(NodeId node_id,
                       const SimulationConfig& config,
                       Stats& stats,
                       std::unordered_map<RequestId, Request>& request_table)
    : node_id_(node_id),
      config_(config),
      stats_(stats),
      request_table_(request_table) {}

void MemoryNode::handle_event(const Event& event, Scheduler& scheduler) {
    switch (event.type) {
    case EventType::ForwardToMemory:
        handle_forward_to_memory(event, scheduler);
        return;
    case EventType::MemoryServiceStart:
        handle_memory_service_start(event, scheduler);
        return;
    case EventType::MemoryServiceComplete:
        handle_memory_service_complete(event, scheduler);
        return;
    default:
        throw std::logic_error("MemoryNode received unsupported event type");
    }
}

void MemoryNode::handle_forward_to_memory(const Event& event,
                                          Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    request.memory_enqueue_time = event.time;
    queued_requests_.push_back(event.request_id);
    stats_.observe_memory_queue_depth(queued_requests_.size());

    if (!service_in_progress_ && !service_start_scheduled_) {
        service_start_scheduled_ = true;
        scheduler.schedule(Event(event.time,
                                 EventType::MemoryServiceStart,
                                 node_id_,
                                 queued_requests_.front()));
    }
}

void MemoryNode::handle_memory_service_start(const Event& event,
                                             Scheduler& scheduler) {
    if (queued_requests_.empty()) {
        throw std::logic_error("Memory service started with empty queue");
    }

    if (queued_requests_.front() != event.request_id) {
        throw std::logic_error("Memory service started out of FIFO order");
    }

    Request& request = request_table_.at(event.request_id);
    queued_requests_.pop_front();
    service_start_scheduled_ = false;
    service_in_progress_ = true;
    request.current_stage = RequestStage::WaitingForResponse;

    const SimTime wait_time = event.time - request.memory_enqueue_time;
    stats_.record_memory_wait(wait_time);

    scheduler.schedule(Event(event.time + service_time_for(request),
                             EventType::MemoryServiceComplete,
                             node_id_,
                             request.request_id));
}

void MemoryNode::handle_memory_service_complete(const Event& event,
                                                Scheduler& scheduler) {
    if (!service_in_progress_) {
        throw std::logic_error("Memory service completed without active request");
    }

    service_in_progress_ = false;

    const Request& request = request_table_.at(event.request_id);
    scheduler.schedule(Event(event.time + config_.one_way_link_latency,
                             EventType::ReturnResponse,
                             request.source_node_id,
                             request.request_id));

    if (!queued_requests_.empty()) {
        service_start_scheduled_ = true;
        scheduler.schedule(Event(event.time,
                                 EventType::MemoryServiceStart,
                                 node_id_,
                                 queued_requests_.front()));
    }
}

SimTime MemoryNode::service_time_for(const Request& request) const noexcept {
    const std::uint64_t bandwidth = config_.memory_bandwidth_bytes_per_time;
    const SimTime serialization_delay =
        static_cast<SimTime>((request.size_bytes + bandwidth - 1) / bandwidth);

    return config_.memory_base_latency + serialization_delay;
}

}  // namespace dm_sim
