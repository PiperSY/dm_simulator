#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "sim/types.hpp"

namespace dm_sim {

/*********************************** 
 * Stats class manages and aggregates various simulation metrics, including latency, memory waits, and cache performance statistics.
 ***********************************/
class Stats {
public:
    // Records the latency of a completed request for the specified node ID, updating total latency and per-node statistics accordingly.    
    void record_latency(NodeId node_id, SimTime latency);
    // Records the wait time for a memory request, updating total memory wait time, maximum wait time, and sample count for calculating averages.
    void record_memory_wait(SimTime wait_time);
    // Observes the current depth of the memory request queue, monitoring peak memory queue depth.
    void observe_memory_queue_depth(std::size_t queue_depth);
    // Records a cache hit for the specified node ID, updating total hits and per-node cache statistics.
    void record_cache_hit(NodeId node_id);
    // Records a cache miss for the specified node ID, updating total misses and per-node cache statistics.
    void record_cache_miss(NodeId node_id);

    // Getters for various statistics, including total requests, completed requests, total and average latency, 
    //   memory wait times, and cache hit/miss counts and rates.
    [[nodiscard]] std::size_t completed_requests() const noexcept;
    [[nodiscard]] SimTime total_latency() const noexcept;
    [[nodiscard]] double average_latency() const noexcept;
    [[nodiscard]] std::size_t completed_requests(NodeId node_id) const noexcept;
    [[nodiscard]] SimTime total_latency(NodeId node_id) const noexcept;
    [[nodiscard]] double average_latency(NodeId node_id) const noexcept;
    [[nodiscard]] const std::vector<SimTime>& latencies() const noexcept;
    [[nodiscard]] const std::vector<SimTime>& latencies(NodeId node_id)
        const noexcept;
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
    // Internal structures for tracking per-node latency and cache statistics.
    struct NodeLatencyStats {
        SimTime total_latency = 0;
        std::vector<SimTime> latencies;
    };
    // Internal structure for tracking per-node cache hit/miss statistics.
    struct NodeCacheStats {
        std::size_t hits = 0;
        std::size_t misses = 0;
    };

    // Total counts and aggregates for requests, latency, memory waits, and cache performance.
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
