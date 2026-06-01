#include "workloads/generators.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace dm_sim {

namespace {

// Internal representation of a hot set split into the portion shared by every
// node and the per-node private portions. Keeping these separate lets partial
// churn preserve the configured cross-node overlap exactly.
struct HotSetSegments {
    std::vector<ObjectId> shared_hot_objects;
    std::vector<std::vector<ObjectId>> private_hot_objects_by_node;
};

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

// Static workloads intentionally ignore any configured churn value. Shifted
// workloads use the configured fraction, defaulting to the historical full
// shift behavior.
double effective_hot_set_churn_fraction(
    const SyntheticWorkloadConfig& config) {
    return config.hot_set_mode == HotSetMode::Static
               ? 0.0
               : config.hot_set_churn_fraction;
}

// Integer hot-set replacement counts cannot represent every fractional value
// exactly, so any nonzero fraction replaces at least one object in a nonempty
// segment.
std::size_t replacement_count(std::size_t segment_size, double churn_fraction) {
    if (segment_size == 0 || churn_fraction <= 0.0) {
        return 0;
    }
    if (churn_fraction >= 1.0) {
        return segment_size;
    }

    return static_cast<std::size_t>(
        std::ceil(static_cast<double>(segment_size) * churn_fraction));
}

// Computes how many fresh object IDs must be available to move from one epoch
// to the next under partial churn.
std::size_t total_replacements_per_epoch(
    const SyntheticWorkloadConfig& config,
    double churn_fraction) {
    const std::size_t shared_count = shared_hot_object_count(config);
    const std::size_t private_count = config.hot_set_size - shared_count;
    return replacement_count(shared_count, churn_fraction) +
           replacement_count(private_count, churn_fraction) *
               config.compute_node_ids.size();
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

std::uint64_t effective_hot_object_channel_count(
    const SyntheticWorkloadConfig& config) {
    return config.hot_object_channel_count == 0
               ? config.memory_channel_count
               : config.hot_object_channel_count;
}

std::size_t eligible_hot_object_count(
    const SyntheticWorkloadConfig& config) {
    const std::uint64_t channel_limit =
        effective_hot_object_channel_count(config);
    std::size_t count = 0;
    for (ObjectId object_id = 1; object_id <= config.object_count; ++object_id) {
        if (memory_channel_for_object(object_id,
                                      config.memory_channel_count) <
            channel_limit) {
            ++count;
        }
    }
    return count;
}

// Hotspot experiments restrict only hot-set allocation. Cold accesses still use
// the full object universe, so this knob creates channel-local pressure without
// changing object placement or cache policy behavior.
std::vector<ObjectId> eligible_hot_objects(
    const SyntheticWorkloadConfig& config,
    const std::vector<ObjectId>& shuffled_objects) {
    const std::uint64_t channel_limit =
        effective_hot_object_channel_count(config);
    if (channel_limit == config.memory_channel_count) {
        return shuffled_objects;
    }

    std::vector<ObjectId> eligible;
    eligible.reserve(eligible_hot_object_count(config));
    for (ObjectId object_id : shuffled_objects) {
        if (memory_channel_for_object(object_id,
                                      config.memory_channel_count) <
            channel_limit) {
            eligible.push_back(object_id);
        }
    }
    return eligible;
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

// Finds the next object that can safely be introduced into the current epoch's
// hot-set layout. `used_objects` prevents duplicates within the new epoch,
// while `disallowed_objects` prevents partial churn from reusing objects that
// were hot in the immediately previous epoch.
ObjectId next_unused_object(const std::vector<ObjectId>& objects,
                            std::size_t& cursor,
                            const std::unordered_set<ObjectId>& used_objects,
                            const std::unordered_set<ObjectId>& disallowed_objects) {
    for (std::size_t attempts = 0; attempts < objects.size(); ++attempts) {
        const ObjectId candidate = object_at(objects, cursor);
        ++cursor;
        if (used_objects.find(candidate) == used_objects.end() &&
            disallowed_objects.find(candidate) == disallowed_objects.end()) {
            return candidate;
        }
    }

    throw std::invalid_argument(
        "Synthetic workload object_count is too small for requested churn");
}

// Keeps the stable prefix of a hot-set segment and drops the suffix that should
// be replaced. This makes churn deterministic and easy to test.
std::vector<ObjectId> keep_prefix_for_churn(const std::vector<ObjectId>& segment,
                                            double churn_fraction) {
    const std::size_t replacements =
        replacement_count(segment.size(), churn_fraction);
    return std::vector<ObjectId>(segment.begin(),
                                 segment.end() - replacements);
}

// Fills a partially churned segment back to its target size with fresh objects.
// The shared segment is filled first, then private segments, which preserves the
// exact overlap shape across all nodes.
void append_replacements(std::vector<ObjectId>& segment,
                         std::size_t final_size,
                         const std::vector<ObjectId>& objects,
                         std::size_t& cursor,
                         std::unordered_set<ObjectId>& used_objects,
                         const std::unordered_set<ObjectId>& disallowed_objects) {
    while (segment.size() < final_size) {
        const ObjectId replacement =
            next_unused_object(objects, cursor, used_objects, disallowed_objects);
        segment.push_back(replacement);
        used_objects.insert(replacement);
    }
}

// Adds one hot-set segment into a lookup set used for duplicate checks.
void insert_segment(std::unordered_set<ObjectId>& objects,
                    const std::vector<ObjectId>& segment) {
    objects.insert(segment.begin(), segment.end());
}

// Flattens the previous epoch's shared and private segments so partial churn can
// avoid immediately reintroducing stale hot objects.
std::unordered_set<ObjectId> all_hot_objects(const HotSetSegments& segments) {
    std::unordered_set<ObjectId> objects;
    insert_segment(objects, segments.shared_hot_objects);
    for (const std::vector<ObjectId>& private_segment :
         segments.private_hot_objects_by_node) {
        insert_segment(objects, private_segment);
    }

    return objects;
}

// Allocates a full hot-set layout from the shuffled object universe. The caller
// owns the cursor so static, full-shift, and partial-churn paths can all share
// the same deterministic object stream.
HotSetSegments allocate_hot_set_segments(
    const SyntheticWorkloadConfig& config,
    const std::vector<ObjectId>& objects,
    std::size_t& cursor) {
    const std::size_t shared_count = shared_hot_object_count(config);
    const std::size_t private_count = config.hot_set_size - shared_count;

    HotSetSegments segments;
    segments.shared_hot_objects = next_objects(objects, cursor, shared_count);
    segments.private_hot_objects_by_node.reserve(config.compute_node_ids.size());
    for (std::size_t i = 0; i < config.compute_node_ids.size(); ++i) {
        segments.private_hot_objects_by_node.push_back(
            next_objects(objects, cursor, private_count));
    }

    return segments;
}

// Builds the next epoch's hot-set segments from the previous epoch by keeping a
// stable prefix in each segment and replacing the churned suffix. Shared and
// private segments are churned independently so overlap remains low/medium/high
// as configured instead of drifting accidentally over time.
HotSetSegments churn_hot_set_segments(const HotSetSegments& previous,
                                      const std::vector<ObjectId>& objects,
                                      std::size_t& cursor,
                                      double churn_fraction) {
    HotSetSegments next;
    // First copy the surviving portion of each segment. At this point the
    // layout is undersized but already contains every object that should remain
    // hot from the previous epoch.
    next.shared_hot_objects =
        keep_prefix_for_churn(previous.shared_hot_objects, churn_fraction);
    next.private_hot_objects_by_node.reserve(
        previous.private_hot_objects_by_node.size());
    for (const std::vector<ObjectId>& private_segment :
         previous.private_hot_objects_by_node) {
        next.private_hot_objects_by_node.push_back(
            keep_prefix_for_churn(private_segment, churn_fraction));
    }

    std::unordered_set<ObjectId> used_objects;
    insert_segment(used_objects, next.shared_hot_objects);
    for (const std::vector<ObjectId>& private_segment :
         next.private_hot_objects_by_node) {
        insert_segment(used_objects, private_segment);
    }

    // Partial churn should introduce genuinely new hot objects rather than
    // reshuffling last epoch's hot objects into different slots.
    const std::unordered_set<ObjectId> previous_hot_objects =
        all_hot_objects(previous);

    // Fill shared replacements before private replacements. This matters
    // because shared replacements must be visible to all nodes, while private
    // replacements must stay node-specific.
    append_replacements(next.shared_hot_objects,
                        previous.shared_hot_objects.size(),
                        objects,
                        cursor,
                        used_objects,
                        previous_hot_objects);
    for (std::size_t i = 0; i < next.private_hot_objects_by_node.size(); ++i) {
        append_replacements(next.private_hot_objects_by_node[i],
                            previous.private_hot_objects_by_node[i].size(),
                            objects,
                            cursor,
                            used_objects,
                            previous_hot_objects);
    }

    return next;
}

// Reconstructs the full hot set for one node by combining the epoch's shared
// segment with that node's private segment.
std::vector<ObjectId> hot_set_for_node(const HotSetSegments& segments,
                                       std::size_t node_index) {
    std::vector<ObjectId> hot_set = segments.shared_hot_objects;
    const std::vector<ObjectId>& private_hot_objects =
        segments.private_hot_objects_by_node[node_index];
    hot_set.insert(hot_set.end(),
                   private_hot_objects.begin(),
                   private_hot_objects.end());
    return hot_set;
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

// Builds a one-based lookup table from object ID to stable object size. Bimodal
// sizes are assigned once per object, not per request, so repeated reads of an
// object have consistent cache-capacity and service-time behavior.
std::vector<std::uint64_t> object_sizes_by_id(
    const SyntheticWorkloadConfig& config) {
    std::vector<std::uint64_t> sizes(
        static_cast<std::size_t>(config.object_count) + 1, 0);

    if (config.object_size_mode == ObjectSizeMode::Fixed) {
        std::fill(sizes.begin(), sizes.end(), config.object_size_bytes);
        return sizes;
    }

    // Use a separate deterministic RNG stream for size assignment. This keeps
    // object access order unchanged when comparing fixed-size and bimodal runs.
    std::mt19937_64 size_rng(config.seed ^ 0x9e3779b97f4a7c15ULL);
    std::bernoulli_distribution choose_large(
        config.large_object_probability);
    for (ObjectId object_id = 1; object_id <= config.object_count; ++object_id) {
        sizes[static_cast<std::size_t>(object_id)] =
            choose_large(size_rng) ? config.object_size_large_bytes
                                   : config.object_size_small_bytes;
    }

    return sizes;
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

    if (config.memory_channel_count == 0) {
        throw std::invalid_argument(
            "Synthetic workload memory_channel_count must be positive");
    }

    if (config.hot_object_channel_count > config.memory_channel_count) {
        throw std::invalid_argument(
            "Synthetic workload hot_object_channel_count must not exceed "
            "memory_channel_count");
    }

    if (config.object_size_bytes == 0) {
        throw std::invalid_argument(
            "Synthetic workload object_size_bytes must be positive");
    }

    if (config.object_size_small_bytes == 0 ||
        config.object_size_large_bytes == 0) {
        throw std::invalid_argument(
            "Synthetic workload bimodal object sizes must be positive");
    }

    if (config.object_size_small_bytes > config.object_size_large_bytes) {
        throw std::invalid_argument(
            "Synthetic workload object_size_small_bytes must not exceed "
            "object_size_large_bytes");
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

    if (config.hot_set_churn_fraction < 0.0 ||
        config.hot_set_churn_fraction > 1.0 ||
        std::isnan(config.hot_set_churn_fraction)) {
        throw std::invalid_argument(
            "Synthetic workload hot_set_churn_fraction must be in [0, 1]");
    }

    if (config.large_object_probability < 0.0 ||
        config.large_object_probability > 1.0 ||
        std::isnan(config.large_object_probability)) {
        throw std::invalid_argument(
            "Synthetic workload large_object_probability must be in [0, 1]");
    }

    const std::size_t eligible_count = eligible_hot_object_count(config);
    if (required_hot_objects_per_epoch(config) > eligible_count) {
        throw std::invalid_argument(
            "Synthetic workload has too few hot-channel-eligible objects for "
            "requested overlap");
    }

    // Full churn can cycle through the object universe, but partial churn
    // temporarily needs both the retained old objects and the newly introduced
    // replacements to be distinct in the same epoch transition.
    const double churn_fraction = effective_hot_set_churn_fraction(config);
    if (churn_fraction > 0.0 && churn_fraction < 1.0) {
        const std::size_t required_objects =
            required_hot_objects_per_epoch(config) +
            total_replacements_per_epoch(config, churn_fraction);
        if (required_objects > eligible_count) {
            throw std::invalid_argument(
                "Synthetic workload has too few hot-channel-eligible objects "
                "for requested partial hot-set churn");
        }
    }

    switch (config.object_size_mode) {
    case ObjectSizeMode::Fixed:
    case ObjectSizeMode::Bimodal:
        break;
    default:
        throw std::invalid_argument("Unknown synthetic workload object size mode");
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
    const std::vector<ObjectId> hot_object_candidates =
        eligible_hot_objects(config, objects);
    const std::vector<std::uint64_t> object_sizes =
        object_sizes_by_id(config);

    GeneratedWorkload workload;
    workload.node_workloads.reserve(config.compute_node_ids.size());
    for (NodeId node_id : config.compute_node_ids) {
        workload.node_workloads.push_back(NodeWorkload{node_id, {}});
        workload.node_workloads.back().requests.reserve(
            config.requests_per_node_per_epoch *
            static_cast<std::size_t>(config.epoch_count));
    }

    const std::size_t epoch_stride = required_hot_objects_per_epoch(config);
    const double churn_fraction = effective_hot_set_churn_fraction(config);
    std::size_t object_cursor = 0;
    HotSetSegments hot_set_segments;

    for (EpochId epoch_id = 0; epoch_id < config.epoch_count; ++epoch_id) {
        // Epoch 0 establishes the initial layout. Later epochs either fully
        // shift to the next layout, partially churn the previous layout, or keep
        // the layout unchanged when effective churn is zero.
        if (epoch_id == 0) {
            hot_set_segments =
                allocate_hot_set_segments(config,
                                          hot_object_candidates,
                                          object_cursor);
        } else if (churn_fraction >= 1.0) {
            // Preserve the original full-shift behavior exactly: each epoch
            // starts at the next stride in the shuffled object universe.
            object_cursor =
                (static_cast<std::size_t>(epoch_id) * epoch_stride) %
                hot_object_candidates.size();
            hot_set_segments =
                allocate_hot_set_segments(config,
                                          hot_object_candidates,
                                          object_cursor);
        } else if (churn_fraction > 0.0) {
            hot_set_segments = churn_hot_set_segments(hot_set_segments,
                                                      hot_object_candidates,
                                                      object_cursor,
                                                      churn_fraction);
        }

        EpochHotSetMetadata epoch_metadata;
        epoch_metadata.epoch_id = epoch_id;

        for (std::size_t node_index = 0;
             node_index < workload.node_workloads.size();
             ++node_index) {
            NodeWorkload& node_workload = workload.node_workloads[node_index];
            const std::vector<ObjectId> hot_set =
                hot_set_for_node(hot_set_segments, node_index);
            epoch_metadata.hot_sets_by_node[node_workload.node_id] = hot_set;

            // Cold objects are computed per node because each node can have a
            // different private hot segment even within the same epoch.
            const std::vector<ObjectId> cold_set =
                cold_objects_for_node(objects, hot_set);
            for (std::size_t i = 0; i < config.requests_per_node_per_epoch; ++i) {
                // Request order is generated directly into the node stream so
                // consumers can replay it with WorkloadCursor.
                const ObjectId object_id = choose_object(hot_set,
                                                         cold_set,
                                                         config.hot_access_probability,
                                                         rng);
                node_workload.requests.push_back(RequestSpec{
                    object_id,
                    object_sizes[static_cast<std::size_t>(object_id)],
                    epoch_id,
                });
            }
        }

        workload.epochs.push_back(std::move(epoch_metadata));
    }

    return workload;
}

}  // namespace dm_sim
