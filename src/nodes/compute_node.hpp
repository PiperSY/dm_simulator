#pragma once

#include <cstddef>
#include <utility>
#include <unordered_map>
#include <vector>

#include "cache/local_cache.hpp"
#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "model/response.hpp"
#include "sim/config.hpp"
#include "sim/event.hpp"

namespace dm_sim {

class Scheduler;

class ComputeNode {
public:
    ComputeNode(NodeId node_id,
                NodeId memory_node_id,
                const std::vector<RequestSpec>& requests,
                SimTime one_way_link_latency,
                SimTime local_cache_hit_latency,
                LocalCache local_cache,
                RequestId& next_request_id,
                std::unordered_map<RequestId, Request>& request_table,
                std::vector<Response>& responses,
                Stats& stats);

    void handle_event(const Event& event, Scheduler& scheduler);

    [[nodiscard]] std::size_t outstanding_requests() const noexcept;
    [[nodiscard]] std::size_t issued_requests() const noexcept;

private:
    void handle_generate_request(const Event& event, Scheduler& scheduler);
    void handle_local_cache_lookup(const Event& event, Scheduler& scheduler);
    void handle_local_cache_hit_complete(const Event& event, Scheduler& scheduler);
    void handle_return_response(const Event& event, Scheduler& scheduler);
    void handle_request_complete(const Event& event, Scheduler& scheduler);

    NodeId node_id_;
    NodeId memory_node_id_;
    const std::vector<RequestSpec>& requests_;
    SimTime one_way_link_latency_ = 0;
    SimTime local_cache_hit_latency_ = 0;
    LocalCache local_cache_;
    RequestId& next_request_id_;
    std::unordered_map<RequestId, Request>& request_table_;
    std::vector<Response>& responses_;
    Stats& stats_;
    std::size_t next_request_index_ = 0;
    std::size_t outstanding_requests_ = 0;
};

}  // namespace dm_sim
