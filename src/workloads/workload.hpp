#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "model/request.hpp"
#include "sim/types.hpp"

namespace dm_sim {

// Controls whether each epoch reuses the same hot objects or rotates to a new
// region of the object universe.
enum class HotSetMode {
    Static,
    EpochShift,
};

// Controls how much of each node's hot set is shared with other compute nodes.
enum class CrossNodeOverlap {
    Low,
    Medium,
    High,
};

// Parameters used to synthesize per-node request streams.
struct SyntheticWorkloadConfig {
    // Seed for deterministic object shuffling and request selection.
    std::uint64_t seed = 1;
    // Compute nodes that receive generated requests.
    std::vector<NodeId> compute_node_ids;
    // Number of distinct objects available to the workload.
    std::uint64_t object_count = 0;
    // Size attached to every generated request.
    std::uint64_t object_size_bytes = 64;
    // Number of requests generated for each node during each epoch.
    std::size_t requests_per_node_per_epoch = 0;
    // Number of epochs to generate.
    EpochId epoch_count = 1;
    // Number of objects considered hot for each node in an epoch.
    std::size_t hot_set_size = 0;
    // Probability that a request chooses from the node's hot set.
    double hot_access_probability = 0.8;
    HotSetMode hot_set_mode = HotSetMode::Static;
    CrossNodeOverlap cross_node_overlap = CrossNodeOverlap::High;
};

// Request stream assigned to a single compute node.
struct NodeWorkload {
    NodeId node_id = 0;
    std::vector<RequestSpec> requests;
};

// Records the hot objects used by each node in one epoch. This metadata is
// useful for validating placement and migration behavior during simulation.
struct EpochHotSetMetadata {
    EpochId epoch_id = 0;
    std::unordered_map<NodeId, std::vector<ObjectId>> hot_sets_by_node;
};

// Complete output of a generated workload: request streams plus epoch metadata.
struct GeneratedWorkload {
    std::vector<NodeWorkload> node_workloads;
    std::vector<EpochHotSetMetadata> epochs;
};

// Sequential reader for a precomputed request stream.
class WorkloadCursor {
public:
    WorkloadCursor() = default;
    // Takes ownership of requests so the cursor can issue them without copying.
    explicit WorkloadCursor(std::vector<RequestSpec> requests);

    // Returns true while next() can still produce a request.
    [[nodiscard]] bool has_next() const noexcept;
    // Number of requests already returned by next().
    [[nodiscard]] std::size_t issued_count() const noexcept;
    // Number of requests still available.
    [[nodiscard]] std::size_t remaining_count() const noexcept;
    // Total number of requests originally loaded into the cursor.
    [[nodiscard]] std::size_t total_count() const noexcept;

    // Returns the next request and advances the cursor.
    RequestSpec next();

private:
    std::vector<RequestSpec> requests_;
    std::size_t next_index_ = 0;
};

}  // namespace dm_sim
