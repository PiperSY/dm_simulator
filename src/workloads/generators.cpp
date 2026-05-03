#include "workloads/generators.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dm_sim {

namespace {

// Converts an overlap mode into the number of hot objects shared by all nodes.
std::size_t shared_hot_object_count(const SyntheticWorkloadConfig& config) {
    switch (config.cross_node_overlap) {
    case CrossNodeOverlap::Low:
        return 0;
    case CrossNodeOverlap::Medium:
        return config.hot_set_size / 2;
    case CrossNodeOverlap::High:
        return config.hot_set_size;
    }

    throw std::invalid_argument("Unknown cross-node overlap mode");
}

// Computes the number of distinct hot objects needed before an epoch's private
// per-node hot sets would start reusing objects.
std::size_t required_hot_objects_per_epoch(const SyntheticWorkloadConfig& config) {
    const std::size_t shared_count = shared_hot_object_count(config);
    const std::size_t private_count = config.hot_set_size - shared_count;
    return shared_count + private_count * config.compute_node_ids.size();
}

// Creates object IDs in the simulation's one-based object ID space.
std::vector<ObjectId> make_object_universe(std::uint64_t object_count) {
    std::vector<ObjectId> objects;
    objects.reserve(static_cast<std::size_t>(object_count));

    for (ObjectId object_id = 1; object_id <= object_count; ++object_id) {
        objects.push_back(object_id);
    }

    return objects;
}

// Wraps object selection so shifted epochs can cycle through the universe.
ObjectId object_at(const std::vector<ObjectId>& objects, std::size_t index) {
    return objects[index % objects.size()];
}

// Selects a contiguous run from the shuffled object list and advances the
// caller-owned cursor for the next hot-set allocation.
std::vector<ObjectId> next_objects(const std::vector<ObjectId>& objects,
                                   std::size_t& cursor,
                                   std::size_t count) {
    std::vector<ObjectId> selected;
    selected.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        selected.push_back(object_at(objects, cursor + i));
    }

    cursor += count;
    return selected;
}

// Builds the complementary cold set for a node after its hot set is chosen.
std::vector<ObjectId> cold_objects_for_node(
    const std::vector<ObjectId>& objects,
    const std::vector<ObjectId>& hot_set) {
    std::unordered_set<ObjectId> hot_lookup(hot_set.begin(), hot_set.end());
    std::vector<ObjectId> cold;
    cold.reserve(objects.size() - hot_set.size());

    for (ObjectId object_id : objects) {
        if (hot_lookup.find(object_id) == hot_lookup.end()) {
            cold.push_back(object_id);
        }
    }

    return cold;
}

// Chooses one request target, biasing toward hot objects while still allowing
// cold accesses when the configured probability permits them.
ObjectId choose_object(const std::vector<ObjectId>& hot_set,
                       const std::vector<ObjectId>& cold_set,
                       double hot_access_probability,
                       std::mt19937_64& rng) {
    const bool choose_hot =
        std::bernoulli_distribution(hot_access_probability)(rng) || cold_set.empty();
    const std::vector<ObjectId>& candidates = choose_hot ? hot_set : cold_set;

    std::uniform_int_distribution<std::size_t> distribution(
        0, candidates.size() - 1);
    return candidates[distribution(rng)];
}

// Rejects configurations that would produce invalid distributions or impossible
// hot-set layouts.
void validate_synthetic_workload_config(const SyntheticWorkloadConfig& config) {
    if (config.compute_node_ids.empty()) {
        throw std::invalid_argument("Synthetic workload requires compute nodes");
    }

    std::unordered_set<NodeId> seen_node_ids;
    for (NodeId node_id : config.compute_node_ids) {
        const auto [_, inserted] = seen_node_ids.insert(node_id);
        if (!inserted) {
            throw std::invalid_argument(
                "Synthetic workload compute node IDs must be unique");
        }
    }

    if (config.object_count == 0) {
        throw std::invalid_argument("Synthetic workload object_count must be positive");
    }

    if (config.object_size_bytes == 0) {
        throw std::invalid_argument(
            "Synthetic workload object_size_bytes must be positive");
    }

    if (config.requests_per_node_per_epoch == 0) {
        throw std::invalid_argument(
            "Synthetic workload requests_per_node_per_epoch must be positive");
    }

    if (config.epoch_count == 0) {
        throw std::invalid_argument("Synthetic workload epoch_count must be positive");
    }

    if (config.hot_set_size == 0) {
        throw std::invalid_argument("Synthetic workload hot_set_size must be positive");
    }

    if (config.hot_set_size > config.object_count) {
        throw std::invalid_argument(
            "Synthetic workload hot_set_size must not exceed object_count");
    }

    if (config.hot_access_probability < 0.0 ||
        config.hot_access_probability > 1.0 ||
        std::isnan(config.hot_access_probability)) {
        throw std::invalid_argument(
            "Synthetic workload hot_access_probability must be in [0, 1]");
    }

    if (required_hot_objects_per_epoch(config) > config.object_count) {
        throw std::invalid_argument(
            "Synthetic workload object_count is too small for requested overlap");
    }
}

}  // namespace

GeneratedWorkload generate_synthetic_workload(
    const SyntheticWorkloadConfig& config) {
    validate_synthetic_workload_config(config);

    // Shuffle once so deterministic seeds still produce varied object layouts.
    std::mt19937_64 rng(config.seed);
    std::vector<ObjectId> objects = make_object_universe(config.object_count);
    std::shuffle(objects.begin(), objects.end(), rng);

    GeneratedWorkload workload;
    workload.node_workloads.reserve(config.compute_node_ids.size());
    for (NodeId node_id : config.compute_node_ids) {
        workload.node_workloads.push_back(NodeWorkload{node_id, {}});
        workload.node_workloads.back().requests.reserve(
            config.requests_per_node_per_epoch *
            static_cast<std::size_t>(config.epoch_count));
    }

    const std::size_t shared_count = shared_hot_object_count(config);
    const std::size_t private_count = config.hot_set_size - shared_count;
    const std::size_t epoch_stride = required_hot_objects_per_epoch(config);

    for (EpochId epoch_id = 0; epoch_id < config.epoch_count; ++epoch_id) {
        // Static workloads reuse the same shuffled prefix. EpochShift advances
        // by the number of hot objects needed for one full epoch.
        const std::size_t epoch_offset =
            config.hot_set_mode == HotSetMode::Static
                ? 0
                : (static_cast<std::size_t>(epoch_id) * epoch_stride) %
                      objects.size();
        std::size_t object_cursor = epoch_offset;

        EpochHotSetMetadata epoch_metadata;
        epoch_metadata.epoch_id = epoch_id;

        // Shared hot objects are allocated once per epoch, then copied into
        // every node's hot set before private objects are appended.
        const std::vector<ObjectId> shared_hot_objects =
            next_objects(objects, object_cursor, shared_count);

        for (NodeWorkload& node_workload : workload.node_workloads) {
            std::vector<ObjectId> hot_set = shared_hot_objects;
            std::vector<ObjectId> private_hot_objects =
                next_objects(objects, object_cursor, private_count);
            hot_set.insert(hot_set.end(),
                           private_hot_objects.begin(),
                           private_hot_objects.end());

            epoch_metadata.hot_sets_by_node[node_workload.node_id] = hot_set;

            const std::vector<ObjectId> cold_set =
                cold_objects_for_node(objects, hot_set);
            for (std::size_t i = 0; i < config.requests_per_node_per_epoch; ++i) {
                // Request order is generated directly into the node stream so
                // consumers can replay it with WorkloadCursor.
                node_workload.requests.push_back(RequestSpec{
                    choose_object(hot_set,
                                  cold_set,
                                  config.hot_access_probability,
                                  rng),
                    config.object_size_bytes,
                    epoch_id,
                });
            }
        }

        workload.epochs.push_back(std::move(epoch_metadata));
    }

    return workload;
}

}  // namespace dm_sim
