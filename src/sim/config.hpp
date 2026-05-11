#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "model/request.hpp"
#include "sim/types.hpp"
#include "workloads/workload.hpp"

namespace dm_sim {

/*********************************** 
 * Configuration for the local cache policy of compute nodes
 * - AlwaysRemote: Compute nodes always fetch data from memory, never hitting the local cache.
 * - LRU: Compute nodes use a Least Recently Used (LRU) policy for the local chache.
 ***********************************/
enum class LocalCachePolicyType {
    AlwaysRemote,
    Lru,
    HotnessOnly,
    GlobalHottestReplication,
    ContentionAware,
};

struct HotnessPolicyConfig {
    std::uint64_t min_admit_count = 2;
    bool reset_on_epoch_change = true;
};

struct ContentionPolicyWeights {
    double local_hotness_weight = 1.0;
    double remote_access_weight = 1.0;
    double distinct_requester_weight = 1.5;
    double queue_wait_weight = 2.0;
    double remote_service_time_weight = 1.0;
    double size_penalty_weight = 0.5;
};

struct ContentionPolicyConfig {
    ContentionPolicyWeights weights;
    double min_admit_score = 1.0;
    std::uint64_t local_hotness_threshold = 2;
    bool reset_on_epoch_change = true;
};

/*********************************** 
 * Configuration for the local cache of compute nodes.
 * - capacity_bytes: Total capacity of the local cache in bytes.
 * - hit_latency: Latency (in simulation time units) for a cache hit.
 * - policy_type: The cache replacement policy to use when the cache is full.
 ***********************************/
struct LocalCacheConfig {
    std::uint64_t capacity_bytes = 0;
    SimTime hit_latency = 1;
    LocalCachePolicyType policy_type = LocalCachePolicyType::AlwaysRemote;
    HotnessPolicyConfig hotness;
    ContentionPolicyConfig contention;
};

/*********************************** 
 * Configuration for a compute node.
 * - node_id: Unique identifier for the compute node.
 * - requests: List of requests that this compute node will generate during the simulation.
 ***********************************/
struct ComputeNodeConfig {
    NodeId node_id = 0;
    std::vector<RequestSpec> requests;
};

/*********************************** 
 * Configuration for the simulation.
 * - compute_nodes: List of compute nodes in the simulation as defined by their configurations.
 * - memory_node_id: Unique identifier for the memory node.
 * - one_way_link_latency: Latency for a one-way communication link between nodes.
 * - memory_base_latency: Base latency for memory access (not including queuing delays).
 * - memory_bandwidth_bytes_per_time: Bandwidth of the memory in bytes per simulation time unit.
 * - local_cache: Configuration for the local cache used by compute nodes.
 ***********************************/
struct SimulationConfig {
    std::vector<ComputeNodeConfig> compute_nodes;
    NodeId memory_node_id = 2;
    SimTime one_way_link_latency = 5;
    SimTime memory_base_latency = 20;
    std::uint64_t memory_bandwidth_bytes_per_time = 16;
    LocalCacheConfig local_cache;
    std::optional<SyntheticWorkloadConfig> synthetic_workload;
};

}  // namespace dm_sim
