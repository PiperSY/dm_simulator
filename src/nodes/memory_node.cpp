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
      request_table_(request_table),
      channels_(static_cast<std::size_t>(config.memory_channel_count)) {}

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
    const MemoryChannelId channel_id = channel_for(request);
    ChannelState& channel = channel_state(channel_id);

    request.memory_enqueue_time = event.time;
    channel.queued_requests.push_back(event.request_id);
    stats_.observe_memory_queue_depth(channel.queued_requests.size());
    stats_.record_remote_access(request,
                                channel_id,
                                channel.queued_requests.size());

    // Channels are independent FIFO servers. This models memory-resource
    // parallelism without pretending to simulate a full fabric or bank model.
    if (!channel.service_in_progress && !channel.service_start_scheduled) {
        channel.service_start_scheduled = true;
        scheduler.schedule(Event(event.time,
                                 EventType::MemoryServiceStart,
                                 node_id_,
                                 channel.queued_requests.front()));
    }
}

void MemoryNode::handle_memory_service_start(const Event& event,
                                             Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    const MemoryChannelId channel_id = channel_for(request);
    ChannelState& channel = channel_state(channel_id);

    if (channel.queued_requests.empty()) {
        throw std::logic_error("Memory service started with empty queue");
    }

    if (channel.queued_requests.front() != event.request_id) {
        throw std::logic_error("Memory service started out of FIFO order");
    }

    channel.queued_requests.pop_front();
    channel.service_start_scheduled = false;
    channel.service_in_progress = true;
    request.current_stage = RequestStage::WaitingForResponse;

    const SimTime wait_time = event.time - request.memory_enqueue_time;
    stats_.record_memory_wait(wait_time);
    stats_.record_object_queue_wait(request, channel_id, wait_time);

    scheduler.schedule(Event(event.time + service_time_for(request),
                             EventType::MemoryServiceComplete,
                             node_id_,
                             request.request_id));
}

void MemoryNode::handle_memory_service_complete(const Event& event,
                                                Scheduler& scheduler) {
    const Request& request = request_table_.at(event.request_id);
    const MemoryChannelId channel_id = channel_for(request);
    ChannelState& channel = channel_state(channel_id);

    if (!channel.service_in_progress) {
        throw std::logic_error("Memory service completed without active request");
    }

    channel.service_in_progress = false;

    stats_.record_object_service(request, channel_id, service_time_for(request));

    scheduler.schedule(Event(event.time + config_.one_way_link_latency,
                             EventType::ReturnResponse,
                             request.source_node_id,
                             request.request_id));

    if (!channel.queued_requests.empty()) {
        channel.service_start_scheduled = true;
        scheduler.schedule(Event(event.time,
                                 EventType::MemoryServiceStart,
                                 node_id_,
                                 channel.queued_requests.front()));
    }
}

SimTime MemoryNode::service_time_for(const Request& request) const noexcept {
    const std::uint64_t bandwidth = config_.memory_bandwidth_bytes_per_time;
    const SimTime serialization_delay =
        static_cast<SimTime>((request.size_bytes + bandwidth - 1) / bandwidth);

    return config_.memory_base_latency + serialization_delay;
}

MemoryChannelId MemoryNode::channel_for(const Request& request) const noexcept {
    return memory_channel_for_object(request.object_id,
                                     config_.memory_channel_count);
}

MemoryNode::ChannelState& MemoryNode::channel_state(
    MemoryChannelId channel_id) {
    return channels_.at(static_cast<std::size_t>(channel_id));
}

const MemoryNode::ChannelState& MemoryNode::channel_state(
    MemoryChannelId channel_id) const {
    return channels_.at(static_cast<std::size_t>(channel_id));
}

}  // namespace dm_sim
