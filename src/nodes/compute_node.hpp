#pragma once

#include <cstddef>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

#include "cache/local_cache.hpp"
#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "model/response.hpp"
#include "sim/config.hpp"
#include "sim/event.hpp"
#include "workloads/workload.hpp"

namespace dm_sim {

class Scheduler;

/*********************************** 
 * ComputeNode class represents a compute node in the simulation. It handles events related to request generation, local cache lookups, 
 * and response handling. The ComputeNode maintains its own local cache and interacts with the Scheduler to manage event timing and execution.
 ***********************************/
class ComputeNode {
public:
    ComputeNode(NodeId node_id,
                NodeId memory_node_id,
                WorkloadCursor workload,
                WorkloadIssueMode issue_mode,
                SimTime one_way_link_latency,
                SimTime local_cache_hit_latency,
                LocalCache local_cache,
                const GlobalReplicaPlan* global_replica_plan,
                RequestId& next_request_id,
                std::unordered_map<RequestId, Request>& request_table,
                std::vector<Response>& responses,
                Stats& stats);

    // Handles an incoming event, dispatching it to the appropriate handler based on the event type. 
    // The ComputeNode processes events such as request generation, local cache lookups, and response handling, updating its state and interacting with the Scheduler as needed.
    void handle_event(const Event& event, Scheduler& scheduler);

    // Getters for the number of outstanding requests and issued requests. Tracks urrent load and activity of the compute node.
    [[nodiscard]] std::size_t outstanding_requests() const noexcept;
    /// Returns the total number of requests that have been issued by this compute node. Tracks the progress of the workload execution.
    [[nodiscard]] std::size_t issued_requests() const noexcept;
    [[nodiscard]] const LocalCache& local_cache() const noexcept;
    [[nodiscard]] std::optional<EpochId> next_request_epoch() const;
    [[nodiscard]] SimTime next_request_issue_offset() const;
    [[nodiscard]] std::vector<PolicyDecisionRecord> policy_diagnostics() const;
    // Copies local-cache lifecycle diagnostics so Simulator can aggregate all
    // compute nodes without exposing mutable cache internals.
    [[nodiscard]] std::vector<CacheAdmissionRecord>
    cache_admission_diagnostics() const;

private:
    // Event handler methods for different event types, including request generation, local cache lookups, cache hit completions, response handling, and request completion.
    void handle_generate_request(const Event& event, Scheduler& scheduler);
    void handle_local_cache_lookup(const Event& event, Scheduler& scheduler);
    void handle_local_cache_hit_complete(const Event& event, Scheduler& scheduler);
    void handle_return_response(const Event& event, Scheduler& scheduler);
    void handle_request_complete(const Event& event, Scheduler& scheduler);
    void start_epoch_if_needed(EpochId epoch_id, SimTime event_time);
    void schedule_next_bursty_request_if_ready(Scheduler& scheduler);

    // Compute node attributes, including its ID, associated memory node ID, workload cursor for generating requests, latencies for link and cache hits, 
    //   local cache instance, references to shared request table and responses vector, statistics collector, and counters for outstanding and issued requests.
    NodeId node_id_;
    NodeId memory_node_id_;
    WorkloadCursor workload_;
    WorkloadIssueMode issue_mode_ = WorkloadIssueMode::CompletionDriven;
    SimTime one_way_link_latency_ = 0;
    SimTime local_cache_hit_latency_ = 0;
    LocalCache local_cache_;
    const GlobalReplicaPlan* global_replica_plan_ = nullptr;
    RequestId& next_request_id_;
    std::unordered_map<RequestId, Request>& request_table_;
    std::vector<Response>& responses_;
    Stats& stats_;
    std::optional<EpochId> current_epoch_;
    SimTime current_epoch_start_time_ = 0;
    std::size_t outstanding_requests_ = 0;
};

}  // namespace dm_sim
