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

enum class HotnessHistoryMode {
    Epoch,
    Cumulative,
    Windowed,
};

struct HotnessPolicyConfig {
    std::uint64_t min_admit_count = 2;
    HotnessHistoryMode history_mode = HotnessHistoryMode::Epoch;
    std::uint64_t history_window_epochs = 4;
};

struct ContentionPolicyWeights {
    double local_hotness_weight = 1.0;
    double remote_access_weight = 1.0;
    double distinct_requester_weight = 1.5;
    double queue_wait_weight = 2.0;
    double remote_service_time_weight = 1.0;
    double size_penalty_weight = 0.5;
    double cost_density_weight = 0.0;
};

enum class ContentionPolicyVariant {
    // V1 is the original Phase 8 policy and remains the default baseline.
    V1,
    // Blends several completed prior epochs instead of using only epoch N-1.
    Smoothed,
    // Requires local reuse evidence before admitting a globally contended item.
    ReuseGated,
    // Requires an incoming object to beat a resident by a configurable margin.
    Hysteresis,
};

struct ContentionPolicyConfig {
    ContentionPolicyWeights weights;
    ContentionPolicyVariant variant = ContentionPolicyVariant::V1;
    double min_admit_score = 1.0;
    std::uint64_t local_hotness_threshold = 2;
    bool reset_on_epoch_change = true;
    // Variant knobs are ignored by v1 unless their matching variant is active.
    std::uint64_t telemetry_history_epochs = 1;
    double telemetry_decay = 1.0;
    std::uint64_t local_reuse_gate_threshold = 2;
    double eviction_score_margin = 0.0;
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
    std::uint64_t memory_channel_count = 1;
};

}  // namespace dm_sim
