#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "sim/types.hpp"

namespace dm_sim {

class Stats {
public:
    void record_latency(NodeId node_id, SimTime latency);
    void record_memory_wait(SimTime wait_time);
    void observe_memory_queue_depth(std::size_t queue_depth);
    void record_cache_hit(NodeId node_id);
    void record_cache_miss(NodeId node_id);

    [[nodiscard]] std::size_t completed_requests() const noexcept;
    [[nodiscard]] SimTime total_latency() const noexcept;
    [[nodiscard]] double average_latency() const noexcept;
    [[nodiscard]] std::size_t completed_requests(NodeId node_id) const noexcept;
    [[nodiscard]] SimTime total_latency(NodeId node_id) const noexcept;
    [[nodiscard]] double average_latency(NodeId node_id) const noexcept;
    [[nodiscard]] const std::vector<SimTime>& latencies() const noexcept;
    [[nodiscard]] SimTime total_memory_wait() const noexcept;
    [[nodiscard]] double average_memory_wait() const noexcept;
    [[nodiscard]] SimTime max_memory_wait() const noexcept;
    [[nodiscard]] std::size_t peak_memory_queue_depth() const noexcept;
    [[nodiscard]] std::size_t local_cache_hits() const noexcept;
    [[nodiscard]] std::size_t local_cache_misses() const noexcept;
    [[nodiscard]] double local_cache_hit_rate() const noexcept;
    [[nodiscard]] std::size_t local_cache_hits(NodeId node_id) const noexcept;
    [[nodiscard]] std::size_t local_cache_misses(NodeId node_id) const noexcept;
    [[nodiscard]] double local_cache_hit_rate(NodeId node_id) const noexcept;

private:
    struct NodeLatencyStats {
        SimTime total_latency = 0;
        std::vector<SimTime> latencies;
    };

    struct NodeCacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
    };

    SimTime total_latency_ = 0;
    std::vector<SimTime> latencies_;
    std::unordered_map<NodeId, NodeLatencyStats> per_node_stats_;
    SimTime total_memory_wait_ = 0;
    SimTime max_memory_wait_ = 0;
    std::size_t memory_wait_samples_ = 0;
    std::size_t peak_memory_queue_depth_ = 0;
    std::size_t local_cache_hits_ = 0;
    std::size_t local_cache_misses_ = 0;
    std::unordered_map<NodeId, NodeCacheStats> per_node_cache_stats_;
};

}  // namespace dm_sim
