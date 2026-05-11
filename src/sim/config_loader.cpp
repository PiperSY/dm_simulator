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

    if (const YAML::Node reset_on_epoch_change =
            hotness_node["reset_on_epoch_change"]) {
        config.reset_on_epoch_change = reset_on_epoch_change.as<bool>();
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

ContentionPolicyConfig parse_contention_policy_config(
    const YAML::Node& local_cache_node) {
    ContentionPolicyConfig config;

    const YAML::Node contention_node = local_cache_node["contention"];
    if (!contention_node) {
        return config;
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
    workload.object_size_bytes = required_as<std::uint64_t>(
        workload_node, "object_size_bytes", "workload");
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
