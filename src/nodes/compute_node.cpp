#include "nodes/compute_node.hpp"

#include <stdexcept>
#include <utility>

#include "sim/scheduler.hpp"

namespace dm_sim {

ComputeNode::ComputeNode(NodeId node_id,
                         NodeId memory_node_id,
                         WorkloadCursor workload,
                         SimTime one_way_link_latency,
                         SimTime local_cache_hit_latency,
                         LocalCache local_cache,
                         const GlobalReplicaPlan* global_replica_plan,
                         RequestId& next_request_id,
                         std::unordered_map<RequestId, Request>& request_table,
                         std::vector<Response>& responses,
                         Stats& stats)
    : node_id_(node_id),
      memory_node_id_(memory_node_id),
      workload_(std::move(workload)),
      one_way_link_latency_(one_way_link_latency),
      local_cache_hit_latency_(local_cache_hit_latency),
      local_cache_(std::move(local_cache)),
      global_replica_plan_(global_replica_plan),
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
    case EventType::LocalCacheHitComplete:
        handle_local_cache_hit_complete(event, scheduler);
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
    return workload_.issued_count();
}

const LocalCache& ComputeNode::local_cache() const noexcept {
    return local_cache_;
}

std::optional<EpochId> ComputeNode::next_request_epoch() const {
    if (!workload_.has_next()) {
        return std::nullopt;
    }

    return workload_.peek_next().epoch_id;
}

std::vector<PolicyDecisionRecord> ComputeNode::policy_diagnostics() const {
    return local_cache_.policy_diagnostics();
}

std::vector<CacheAdmissionRecord>
ComputeNode::cache_admission_diagnostics() const {
    const std::vector<CacheAdmissionRecord>& records =
        local_cache_.cache_admission_diagnostics();
    return {records.begin(), records.end()};
}

void ComputeNode::handle_generate_request(const Event& event,
                                          Scheduler& scheduler) {
    if (!workload_.has_next()) {
        return;
    }

    const RequestSpec spec = workload_.next();
    const RequestId request_id = next_request_id_++;

    Request request;
    request.request_id = request_id;
    request.source_node_id = node_id_;
    request.object_id = spec.object_id;
    request.operation_type = OperationType::Read;
    request.issue_time = event.time;
    request.size_bytes = spec.size_bytes;
    request.epoch_id = spec.epoch_id;
    request.current_stage = RequestStage::Generated;

    start_epoch_if_needed(request.epoch_id, event.time);

    request_table_[request_id] = request;
    ++outstanding_requests_;

    scheduler.schedule(
        Event(event.time, EventType::LocalCacheLookup, node_id_, request_id));
}

void ComputeNode::handle_local_cache_lookup(const Event& event,
                                            Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    request.current_stage = RequestStage::LocalLookup;

    if (local_cache_.lookup(request, event.time)) {
        stats_.record_cache_hit(node_id_);
        scheduler.schedule(Event(event.time + local_cache_hit_latency_,
                                 EventType::LocalCacheHitComplete,
                                 node_id_,
                                 event.request_id));
        return;
    }

    stats_.record_cache_miss(node_id_);
    request.current_stage = RequestStage::ForwardedToMemory;

    scheduler.schedule(Event(event.time + one_way_link_latency_,
                             EventType::ForwardToMemory,
                             memory_node_id_,
                             event.request_id));
}

void ComputeNode::handle_local_cache_hit_complete(const Event& event,
                                                  Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    request.current_stage = RequestStage::Completed;

    const SimTime latency = event.time - request.issue_time;

    Response response;
    response.request_id = request.request_id;
    response.object_id = request.object_id;
    response.served_from_tier = ServedFromTier::LocalCache;
    response.completion_time = event.time;
    response.total_latency = latency;
    response.bytes_transferred = 0;

    responses_.push_back(response);
    stats_.record_latency(node_id_, latency);

    if (outstanding_requests_ == 0) {
        throw std::logic_error("Outstanding request count underflow");
    }
    --outstanding_requests_;

    scheduler.schedule(
        Event(event.time, EventType::RequestComplete, node_id_, event.request_id));
}

void ComputeNode::handle_return_response(const Event& event,
                                         Scheduler& scheduler) {
    Request& request = request_table_.at(event.request_id);
    const SimTime latency = event.time - request.issue_time;

    Response response;
    response.request_id = request.request_id;
    response.object_id = request.object_id;
    response.served_from_tier = ServedFromTier::Memory;
    response.completion_time = event.time;
    response.total_latency = latency;
    response.bytes_transferred = request.size_bytes;

    responses_.push_back(response);
    stats_.record_latency(node_id_, latency);
    const bool admitted = local_cache_.admit(request, response, event.time);
    (void)admitted;
    request.current_stage = RequestStage::Completed;

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

    if (workload_.has_next() && current_epoch_.has_value() &&
        workload_.peek_next().epoch_id == *current_epoch_) {
        scheduler.schedule(
            Event(scheduler.now(), EventType::GenerateRequest, node_id_));
    }
}

void ComputeNode::start_epoch_if_needed(EpochId epoch_id, SimTime event_time) {
    if (current_epoch_.has_value() && *current_epoch_ == epoch_id) {
        return;
    }

    current_epoch_ = epoch_id;
    local_cache_.on_epoch_start(epoch_id);

    if (global_replica_plan_ == nullptr) {
        return;
    }

    const auto it = global_replica_plan_->find(epoch_id);
    if (it == global_replica_plan_->end()) {
        local_cache_.install_replicas({}, event_time, node_id_, epoch_id);
        return;
    }

    local_cache_.install_replicas(it->second, event_time, node_id_, epoch_id);
}

}  // namespace dm_sim
