#include "nodes/compute_node.hpp"

#include <stdexcept>

#include "sim/scheduler.hpp"

namespace dm_sim {

ComputeNode::ComputeNode(NodeId node_id,
                         NodeId memory_node_id,
                         const SimulationConfig& config,
                         RequestId& next_request_id,
                         std::unordered_map<RequestId, Request>& request_table,
                         std::vector<Response>& responses,
                         Stats& stats)
    : node_id_(node_id),
      memory_node_id_(memory_node_id),
      config_(config),
      next_request_id_(next_request_id),
      request_table_(request_table),
      responses_(responses),
      stats_(stats) {}

void ComputeNode::handle_event(const Event& event, Scheduler& scheduler) {
    switch (event.type) {
    case EventType::GenerateRequest:
        handle_generate_request(event, scheduler);
        return;
    case EventType::LocalCacheLookup:
        handle_local_cache_lookup(event, scheduler);
        return;
    case EventType::ReturnResponse:
        handle_return_response(event, scheduler);
        return;
    case EventType::RequestComplete:
        handle_request_complete(event, scheduler);
        return;
    default:
        throw std::logic_error("ComputeNode received unsupported event type");
    }
}

std::size_t ComputeNode::outstanding_requests() const noexcept {
    return outstanding_requests_;
}

std::size_t ComputeNode::issued_requests() const noexcept {
    return next_request_index_;
}

void ComputeNode::handle_generate_request(const Event& event,
                                          Scheduler& scheduler) {
    if (next_request_index_ >= config_.requests.size()) {
        return;
    }

    const RequestSpec& spec = config_.requests[next_request_index_++];
    const RequestId request_id = next_request_id_++;

    Request request;
    request.request_id = request_id;
    request.source_node_id = node_id_;
    request.object_id = spec.object_id;
    request.operation_type = OperationType::Read;
    request.issue_time = event.time;
    request.size_bytes = spec.size_bytes;
    request.current_stage = RequestStage::Generated;

    request_table_[request_id] = request;
    ++outstanding_requests_;

    scheduler.schedule(
        Event(event.time, EventType::LocalCacheLookup, node_id_, request_id));
}

void ComputeNode::handle_local_cache_lookup(const Event& event,
                                            Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    request.current_stage = RequestStage::LocalLookup;
    request.current_stage = RequestStage::ForwardedToMemory;

    scheduler.schedule(Event(event.time + config_.one_way_link_latency,
                             EventType::ForwardToMemory,
                             memory_node_id_,
                             event.request_id));
}

void ComputeNode::handle_return_response(const Event& event,
                                         Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    request.current_stage = RequestStage::Completed;

    const SimTime latency = event.time - request.issue_time;

    Response response;
    response.request_id = request.request_id;
    response.object_id = request.object_id;
    response.served_from_tier = ServedFromTier::Memory;
    response.completion_time = event.time;
    response.total_latency = latency;
    response.bytes_transferred = request.size_bytes;

    responses_.push_back(response);
    stats_.record_latency(latency);

    if (outstanding_requests_ == 0) {
        throw std::logic_error("Outstanding request count underflow");
    }
    --outstanding_requests_;

    scheduler.schedule(
        Event(event.time, EventType::RequestComplete, node_id_, event.request_id));
}

void ComputeNode::handle_request_complete(const Event& event,
                                          Scheduler& scheduler) {
    (void)event;

    if (next_request_index_ < config_.requests.size()) {
        scheduler.schedule(
            Event(scheduler.now(), EventType::GenerateRequest, node_id_));
    }
}

}  // namespace dm_sim
