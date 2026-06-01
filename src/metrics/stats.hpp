#pragma once

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "model/request.hpp"
#include "sim/types.hpp"

namespace dm_sim {

/***********************************
 * ObjectContentionStats struct captures various metrics related to contention for a specific object during a particular epoch, including remote accesses, distinct requesters, 
 *   bytes served, service times, and queue wait times. This structure is used to analyze and understand contention patterns in the simulation.
 **********************************/
struct ObjectContentionStats {
    EpochId epoch_id = 0;
    ObjectId object_id = 0;
    MemoryChannelId memory_channel_id = 0;
    std::size_t remote_accesses = 0;            // Total number of remote accesses for this object during the epoch.
    std::size_t distinct_requesters = 0;        // Number of unique compute nodes that requested this object, indicating the breadth of contention across the system.
    std::uint64_t bytes_served = 0;             // Total bytes served for this object during the epoch, providing insight into the volume of data involved in the contention.   
    SimTime total_remote_service_time = 0;      // Total time spent servicing remote requests for this object, which can indicate the severity of contention and its impact on latency.
    SimTime total_queue_wait = 0;
    SimTime max_queue_wait = 0;
    std::size_t queue_wait_samples = 0;         // Number of samples taken for queue wait times, used to calculate average queue wait time for this object during the epoch.
    std::size_t max_observed_queue_depth = 0;   // Maximum observed depth of the memory request queue for this object during the epoch, which can indicate periods of high contention and potential bottlenecks.
    double average_queue_wait = 0.0;
};

struct ChannelContentionStats {
    EpochId epoch_id = 0;
    MemoryChannelId memory_channel_id = 0;
    std::size_t remote_accesses = 0;
    std::uint64_t bytes_served = 0;
    SimTime total_remote_service_time = 0;
    SimTime total_queue_wait = 0;
    SimTime max_queue_wait = 0;
    std::size_t queue_wait_samples = 0;
    std::size_t max_queue_depth = 0;
    double average_queue_wait = 0.0;
};

// Sorting key for top contention objects, allowing for different criteria to be used when ranking objects based on their contention statistics.
enum class ContentionSortKey {
    TotalQueueWait,
    TotalRemoteServiceTime,
    DistinctRequesters,
};

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
    void record_remote_access(const Request& request,
                              MemoryChannelId memory_channel_id,
                              std::size_t observed_queue_depth);
    void record_object_queue_wait(const Request& request,
                                  MemoryChannelId memory_channel_id,
                                  SimTime wait_time);
    void record_object_service(const Request& request,
                               MemoryChannelId memory_channel_id,
                               SimTime service_time);

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
    [[nodiscard]] std::size_t peak_memory_channel_queue_depth() const noexcept;
    [[nodiscard]] std::size_t local_cache_hits() const noexcept;
    [[nodiscard]] std::size_t local_cache_misses() const noexcept;
    [[nodiscard]] double local_cache_hit_rate() const noexcept;
    [[nodiscard]] std::size_t local_cache_hits(NodeId node_id) const noexcept;
    [[nodiscard]] std::size_t local_cache_misses(NodeId node_id) const noexcept;
    [[nodiscard]] double local_cache_hit_rate(NodeId node_id) const noexcept;
    // Methods for retrieving contention statistics for specific objects and epochs, as well as methods for retrieving sorted lists of contention statistics based on different criteria.
    [[nodiscard]] std::optional<ObjectContentionStats> object_contention(
        EpochId epoch_id,
        ObjectId object_id) const;
    [[nodiscard]] std::vector<ObjectContentionStats> contention_by_epoch(
        EpochId epoch_id) const;
    [[nodiscard]] std::optional<ObjectContentionStats> previous_epoch_object_contention(EpochId current_epoch,
        ObjectId object_id) const;
    [[nodiscard]] std::vector<ObjectContentionStats> previous_epoch_contention(
        EpochId current_epoch) const;
    [[nodiscard]] std::vector<ObjectContentionStats> all_contention_stats()
        const;
    [[nodiscard]] std::vector<ObjectContentionStats> top_contention_objects(
        ContentionSortKey sort_key,
        std::size_t limit) const;
    [[nodiscard]] std::optional<ChannelContentionStats> channel_contention(
        EpochId epoch_id,
        MemoryChannelId memory_channel_id) const;
    [[nodiscard]] std::vector<ChannelContentionStats> channel_contention_by_epoch(
        EpochId epoch_id) const;
    [[nodiscard]] std::vector<ChannelContentionStats> all_channel_contention_stats()
        const;

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
    // Internal structure for tracking contention statistics for each object in each epoch, including remote accesses, requesters, service times, and queue wait times.
    struct InternalObjectContentionStats {
        ObjectContentionStats stats;
        std::unordered_set<NodeId> requesters;
    };
    struct InternalChannelContentionStats {
        ChannelContentionStats stats;
    };
    // Helper method to retrieve the contention bucket for a given request, allowing for updates to contention statistics based on the request's epoch and object ID.
    [[nodiscard]] InternalObjectContentionStats& contention_bucket(
        const Request& request,
        MemoryChannelId memory_channel_id);
    [[nodiscard]] InternalChannelContentionStats& channel_bucket(
        EpochId epoch_id,
        MemoryChannelId memory_channel_id);
    // Helper method to create a snapshot of the contention statistics for an object, converting from the internal structure to the public ObjectContentionStats structure for reporting and analysis.
    [[nodiscard]] static ObjectContentionStats snapshot_contention(
        const InternalObjectContentionStats& internal_stats);
    [[nodiscard]] static ChannelContentionStats snapshot_channel_contention(
        const InternalChannelContentionStats& internal_stats);

    // Total counts and aggregates for requests, latency, memory waits, and cache performance.
    SimTime total_latency_ = 0;
    std::vector<SimTime> latencies_;
    std::unordered_map<NodeId, NodeLatencyStats> per_node_stats_;
    SimTime total_memory_wait_ = 0;
    SimTime max_memory_wait_ = 0;
    std::size_t memory_wait_samples_ = 0;
    std::size_t peak_memory_queue_depth_ = 0;
    std::size_t peak_memory_channel_queue_depth_ = 0;
    std::size_t local_cache_hits_ = 0;
    std::size_t local_cache_misses_ = 0;
    // Per-node cache statistics for hits and misses, allowing for analysis of cache performance on a per-node basis.
    std::unordered_map<NodeId, NodeCacheStats> per_node_cache_stats_;
    // Contention statistics organized by epoch and object ID.
    std::unordered_map<EpochId, std::unordered_map<ObjectId, InternalObjectContentionStats>>
        contention_by_epoch_;
    std::unordered_map<EpochId, std::unordered_map<MemoryChannelId, InternalChannelContentionStats>>
        channel_contention_by_epoch_;
};

}  // namespace dm_sim
