#include "sim/config_loader.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace dm_sim {

namespace {

// Helper function to retrieve a required child node from a YAML node, throwing an exception if the child is missing.
YAML::Node required_child(const YAML::Node& node,
                          const std::string& key,
                          const std::string& context) {
    const YAML::Node child = node[key];
    if (!child) {
        throw std::invalid_argument("Missing required config field: " +
                                    context + "." + key);
    }

    return child;
}

// Template function to retrieve a required child node and convert it to the specified type, throwing an exception if the conversion fails.
template <typename T>
T required_as(const YAML::Node& node,
              const std::string& key,
              const std::string& context) {
    const YAML::Node child = required_child(node, key, context);
    try {
        return child.as<T>();
    } catch (const YAML::Exception& error) {
        throw std::invalid_argument("Invalid value for config field " +
                                    context + "." + key + ": " +
                                    error.what());
    }
}

// Parses the local cache policy type from a string value, throwing an exception if the value is invalid.
LocalCachePolicyType parse_local_cache_policy(const std::string& value) {
    if (value == "always_remote") {
        return LocalCachePolicyType::AlwaysRemote;
    }
    if (value == "lru") {
        return LocalCachePolicyType::Lru;
    }
    if (value == "hotness_only") {
        return LocalCachePolicyType::HotnessOnly;
    }
    if (value == "global_hottest_replication") {
        return LocalCachePolicyType::GlobalHottestReplication;
    }
    if (value == "contention_aware") {
        return LocalCachePolicyType::ContentionAware;
    }

    throw std::invalid_argument(
        "Invalid local_cache.policy: expected always_remote, lru, "
        "hotness_only, global_hottest_replication, or contention_aware");
}

HotnessHistoryMode parse_hotness_history_mode(const std::string& value) {
    if (value == "epoch") {
        return HotnessHistoryMode::Epoch;
    }
    if (value == "cumulative") {
        return HotnessHistoryMode::Cumulative;
    }
    if (value == "windowed") {
        return HotnessHistoryMode::Windowed;
    }

    throw std::invalid_argument(
        "Invalid local_cache.hotness.history_mode: expected epoch, "
        "cumulative, or windowed");
}

HotnessPolicyConfig parse_hotness_policy_config(
    const YAML::Node& local_cache_node) {
    HotnessPolicyConfig config;

    const YAML::Node hotness_node = local_cache_node["hotness"];
    if (!hotness_node) {
        return config;
    }

    if (const YAML::Node min_admit_count = hotness_node["min_admit_count"]) {
        config.min_admit_count = min_admit_count.as<std::uint64_t>();
    }

    if (hotness_node["reset_on_epoch_change"]) {
        throw std::invalid_argument(
            "Deprecated config field local_cache.hotness.reset_on_epoch_change: "
            "use local_cache.hotness.history_mode instead");
    }

    if (const YAML::Node history_mode = hotness_node["history_mode"]) {
        config.history_mode =
            parse_hotness_history_mode(history_mode.as<std::string>());
    }

    if (const YAML::Node history_window_epochs =
            hotness_node["history_window_epochs"]) {
        config.history_window_epochs =
            history_window_epochs.as<std::uint64_t>();
        if (config.history_window_epochs == 0) {
            throw std::invalid_argument(
                "Invalid local_cache.hotness.history_window_epochs: "
                "expected a positive integer");
        }
    }

    return config;
}

double optional_nonnegative_double(const YAML::Node& node,
                                   const std::string& key,
                                   const std::string& context,
                                   double current_value) {
    const YAML::Node child = node[key];
    if (!child) {
        return current_value;
    }

    const double value = child.as<double>();
    if (!std::isfinite(value) || value < 0.0) {
        throw std::invalid_argument("Invalid " + context + "." + key +
                                    ": expected a nonnegative finite number");
    }

    return value;
}

ContentionPolicyVariant parse_contention_policy_variant(
    const std::string& value) {
    // Variant strings are intentionally separate from local_cache.policy so
    // existing contention_aware configs keep their Phase 8 behavior.
    if (value == "v1") {
        return ContentionPolicyVariant::V1;
    }
    if (value == "smoothed") {
        return ContentionPolicyVariant::Smoothed;
    }
    if (value == "reuse_gated") {
        return ContentionPolicyVariant::ReuseGated;
    }
    if (value == "hysteresis") {
        return ContentionPolicyVariant::Hysteresis;
    }

    throw std::invalid_argument(
        "Invalid local_cache.contention.variant: expected v1, smoothed, "
        "reuse_gated, or hysteresis");
}

ContentionPolicyConfig parse_contention_policy_config(
    const YAML::Node& local_cache_node) {
    ContentionPolicyConfig config;

    const YAML::Node contention_node = local_cache_node["contention"];
    if (!contention_node) {
        return config;
    }

    if (const YAML::Node variant = contention_node["variant"]) {
        config.variant =
            parse_contention_policy_variant(variant.as<std::string>());
    }

    config.weights.local_hotness_weight = optional_nonnegative_double(
        contention_node,
        "local_hotness_weight",
        "local_cache.contention",
        config.weights.local_hotness_weight);
    config.weights.remote_access_weight = optional_nonnegative_double(
        contention_node,
        "remote_access_weight",
        "local_cache.contention",
        config.weights.remote_access_weight);
    config.weights.distinct_requester_weight = optional_nonnegative_double(
        contention_node,
        "distinct_requester_weight",
        "local_cache.contention",
        config.weights.distinct_requester_weight);
    config.weights.queue_wait_weight = optional_nonnegative_double(
        contention_node,
        "queue_wait_weight",
        "local_cache.contention",
        config.weights.queue_wait_weight);
    config.weights.remote_service_time_weight = optional_nonnegative_double(
        contention_node,
        "remote_service_time_weight",
        "local_cache.contention",
        config.weights.remote_service_time_weight);
    config.weights.size_penalty_weight = optional_nonnegative_double(
        contention_node,
        "size_penalty_weight",
        "local_cache.contention",
        config.weights.size_penalty_weight);
    config.weights.cost_density_weight = optional_nonnegative_double(
        contention_node,
        "cost_density_weight",
        "local_cache.contention",
        config.weights.cost_density_weight);
    config.min_admit_score = optional_nonnegative_double(
        contention_node,
        "min_admit_score",
        "local_cache.contention",
        config.min_admit_score);

    if (const YAML::Node threshold =
            contention_node["local_hotness_threshold"]) {
        config.local_hotness_threshold = threshold.as<std::uint64_t>();
        if (config.local_hotness_threshold == 0) {
            throw std::invalid_argument(
                "Invalid local_cache.contention.local_hotness_threshold: "
                "expected a positive integer");
        }
    }

    if (const YAML::Node reset_on_epoch_change =
            contention_node["reset_on_epoch_change"]) {
        config.reset_on_epoch_change = reset_on_epoch_change.as<bool>();
    }

    if (const YAML::Node history_epochs =
            contention_node["telemetry_history_epochs"]) {
        config.telemetry_history_epochs = history_epochs.as<std::uint64_t>();
        if (config.telemetry_history_epochs == 0) {
            throw std::invalid_argument(
                "Invalid local_cache.contention.telemetry_history_epochs: "
                "expected a positive integer");
        }
    }

    config.telemetry_decay = optional_nonnegative_double(
        contention_node,
        "telemetry_decay",
        "local_cache.contention",
        config.telemetry_decay);
    if (const YAML::Node reuse_gate =
            contention_node["local_reuse_gate_threshold"]) {
        config.local_reuse_gate_threshold = reuse_gate.as<std::uint64_t>();
        if (config.local_reuse_gate_threshold == 0) {
            throw std::invalid_argument(
                "Invalid local_cache.contention.local_reuse_gate_threshold: "
                "expected a positive integer");
        }
    }
    config.eviction_score_margin = optional_nonnegative_double(
        contention_node,
        "eviction_score_margin",
        "local_cache.contention",
        config.eviction_score_margin);

    return config;
}

// Parses the hot set mode from a string value, throwing an exception if the value is invalid.
HotSetMode parse_hot_set_mode(const std::string& value) {
    if (value == "static") {
        return HotSetMode::Static;
    }
    if (value == "epoch_shift") {
        return HotSetMode::EpochShift;
    }

    throw std::invalid_argument(
        "Invalid workload.hot_set_mode: expected static or epoch_shift");
}

// Parses the cross-node overlap level from a string value, throwing an exception if the value is invalid.
CrossNodeOverlap parse_cross_node_overlap(const std::string& value) {
    if (value == "low") {
        return CrossNodeOverlap::Low;
    }
    if (value == "medium") {
        return CrossNodeOverlap::Medium;
    }
    if (value == "high") {
        return CrossNodeOverlap::High;
    }

    throw std::invalid_argument(
        "Invalid workload.cross_node_overlap: expected low, medium, or high");
}

ObjectSizeMode parse_object_size_mode(const std::string& value) {
    if (value == "fixed") {
        return ObjectSizeMode::Fixed;
    }
    if (value == "bimodal") {
        return ObjectSizeMode::Bimodal;
    }

    throw std::invalid_argument(
        "Invalid workload.object_size_mode: expected fixed or bimodal");
}

WorkloadIssueMode parse_workload_issue_mode(const std::string& value) {
    if (value == "completion_driven") {
        return WorkloadIssueMode::CompletionDriven;
    }
    if (value == "scheduled_bursty") {
        return WorkloadIssueMode::ScheduledBursty;
    }

    throw std::invalid_argument(
        "Invalid workload.issue_mode: expected completion_driven or "
        "scheduled_bursty");
}

double optional_probability(const YAML::Node& node,
                            const std::string& key,
                            const std::string& context,
                            double current_value) {
    const YAML::Node child = node[key];
    if (!child) {
        return current_value;
    }

    const double value = child.as<double>();
    if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
        throw std::invalid_argument("Invalid " + context + "." + key +
                                    ": expected a finite number in [0, 1]");
    }

    return value;
}

std::uint64_t optional_positive_u64(const YAML::Node& node,
                                    const std::string& key,
                                    const std::string& context,
                                    std::uint64_t current_value) {
    const YAML::Node child = node[key];
    if (!child) {
        return current_value;
    }

    const std::uint64_t value = child.as<std::uint64_t>();
    if (value == 0) {
        throw std::invalid_argument("Invalid " + context + "." + key +
                                    ": expected a positive integer");
    }

    return value;
}

// Parses a list of compute node IDs from a YAML node, throwing an exception if the node is not a sequence or if any ID cannot be converted to the expected type.
std::vector<NodeId> parse_compute_node_ids(const YAML::Node& workload_node) {
    const YAML::Node ids_node =
        required_child(workload_node, "compute_node_ids", "workload");
    if (!ids_node.IsSequence()) {
        throw std::invalid_argument(
            "Invalid workload.compute_node_ids: expected a YAML sequence");
    }

    std::vector<NodeId> node_ids;
    node_ids.reserve(ids_node.size());
    for (const YAML::Node& id_node : ids_node) {
        node_ids.push_back(id_node.as<NodeId>());
    }

    return node_ids;
}

}  // namespace

// Loads the experiment configuration from a YAML file at the specified path, constructing an ExperimentConfig instance with the parsed values.
ExperimentConfig load_experiment_config(const std::string& path) {
    YAML::Node root;
    try {
        root = YAML::LoadFile(path);
    } catch (const YAML::Exception& error) {
        throw std::invalid_argument("Failed to load config file " + path +
                                    ": " + error.what());
    }

    const YAML::Node experiment_node =
        required_child(root, "experiment", "root");
    const YAML::Node memory_node = required_child(root, "memory", "root");
    const YAML::Node link_node = required_child(root, "link", "root");
    const YAML::Node local_cache_node =
        required_child(root, "local_cache", "root");
    const YAML::Node workload_node = required_child(root, "workload", "root");

    ExperimentConfig experiment;
    experiment.name =
        required_as<std::string>(experiment_node, "name", "experiment");
    experiment.output_dir =
        required_as<std::string>(experiment_node, "output_dir", "experiment");

    SimulationConfig simulation;
    simulation.memory_node_id =
        required_as<NodeId>(memory_node, "node_id", "memory");
    simulation.memory_base_latency =
        required_as<SimTime>(memory_node, "base_latency", "memory");
    simulation.memory_bandwidth_bytes_per_time =
        required_as<std::uint64_t>(
            memory_node, "bandwidth_bytes_per_time", "memory");
    simulation.memory_channel_count = optional_positive_u64(
        memory_node,
        "channel_count",
        "memory",
        simulation.memory_channel_count);
    simulation.one_way_link_latency =
        required_as<SimTime>(link_node, "one_way_latency", "link");
    simulation.local_cache = LocalCacheConfig{
        required_as<std::uint64_t>(
            local_cache_node, "capacity_bytes", "local_cache"),
        required_as<SimTime>(local_cache_node, "hit_latency", "local_cache"),
        parse_local_cache_policy(required_as<std::string>(
            local_cache_node, "policy", "local_cache")),
        parse_hotness_policy_config(local_cache_node),
        parse_contention_policy_config(local_cache_node),
    };

    SyntheticWorkloadConfig workload;
    workload.seed = required_as<std::uint64_t>(workload_node, "seed", "workload");
    workload.compute_node_ids = parse_compute_node_ids(workload_node);
    workload.object_count =
        required_as<std::uint64_t>(workload_node, "object_count", "workload");
    workload.memory_channel_count = simulation.memory_channel_count;
    workload.object_size_bytes = required_as<std::uint64_t>(
        workload_node, "object_size_bytes", "workload");
    if (workload.object_size_bytes == 0) {
        throw std::invalid_argument(
            "Invalid workload.object_size_bytes: expected a positive integer");
    }
    workload.hot_set_churn_fraction = optional_probability(
        workload_node,
        "hot_set_churn_fraction",
        "workload",
        workload.hot_set_churn_fraction);
    if (const YAML::Node hot_object_channel_count =
            workload_node["hot_object_channel_count"]) {
        workload.hot_object_channel_count =
            hot_object_channel_count.as<std::uint64_t>();
        if (workload.hot_object_channel_count >
            workload.memory_channel_count) {
            throw std::invalid_argument(
                "Invalid workload.hot_object_channel_count: expected a value "
                "no larger than memory.channel_count");
        }
    }
    if (const YAML::Node object_size_mode_node =
            workload_node["object_size_mode"]) {
        workload.object_size_mode =
            parse_object_size_mode(object_size_mode_node.as<std::string>());
    }
    workload.object_size_small_bytes = optional_positive_u64(
        workload_node,
        "object_size_small_bytes",
        "workload",
        workload.object_size_small_bytes);
    workload.object_size_large_bytes = optional_positive_u64(
        workload_node,
        "object_size_large_bytes",
        "workload",
        workload.object_size_large_bytes);
    if (workload.object_size_small_bytes > workload.object_size_large_bytes) {
        throw std::invalid_argument(
            "Invalid workload object size bounds: object_size_small_bytes "
            "must not exceed object_size_large_bytes");
    }
    workload.large_object_probability = optional_probability(
        workload_node,
        "large_object_probability",
        "workload",
        workload.large_object_probability);
    if (const YAML::Node issue_mode_node = workload_node["issue_mode"]) {
        workload.issue_mode =
            parse_workload_issue_mode(issue_mode_node.as<std::string>());
    }
    workload.burst_size = optional_positive_u64(workload_node,
                                                "burst_size",
                                                "workload",
                                                workload.burst_size);
    workload.burst_interval = optional_positive_u64(workload_node,
                                                    "burst_interval",
                                                    "workload",
                                                    workload.burst_interval);
    if (const YAML::Node intra_burst_gap_node =
            workload_node["intra_burst_gap"]) {
        workload.intra_burst_gap =
            intra_burst_gap_node.as<SimTime>();
    }
    if (const YAML::Node node_phase_jitter_node =
            workload_node["node_phase_jitter"]) {
        workload.node_phase_jitter =
            node_phase_jitter_node.as<SimTime>();
    }
    workload.requests_per_node_per_epoch = required_as<std::size_t>(
        workload_node, "requests_per_node_per_epoch", "workload");
    workload.epoch_count =
        required_as<EpochId>(workload_node, "epoch_count", "workload");
    workload.hot_set_size =
        required_as<std::size_t>(workload_node, "hot_set_size", "workload");
    workload.hot_access_probability = required_as<double>(
        workload_node, "hot_access_probability", "workload");
    workload.hot_set_mode = parse_hot_set_mode(
        required_as<std::string>(workload_node, "hot_set_mode", "workload"));
    workload.cross_node_overlap = parse_cross_node_overlap(
        required_as<std::string>(
            workload_node, "cross_node_overlap", "workload"));
    simulation.synthetic_workload = workload;

    experiment.simulation = simulation;
    return experiment;
}

SimulationConfig load_simulation_config(const std::string& path) {
    return load_experiment_config(path).simulation;
}

}  // namespace dm_sim
