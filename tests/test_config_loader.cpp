#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sim/config_loader.hpp"

namespace {

using dm_sim::CrossNodeOverlap;
using dm_sim::ExperimentConfig;
using dm_sim::HotSetMode;
using dm_sim::LocalCachePolicyType;
using dm_sim::ObjectSizeMode;
using dm_sim::SimulationConfig;

std::filesystem::path write_temp_config(const std::string& name,
                                        const std::string& contents) {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() / name;
    std::ofstream output(path);
    output << contents;
    return path;
}

std::string valid_config_text() {
    return R"(experiment:
  name: loader_test
  output_dir: /tmp/dm_sim_loader_test_results

memory:
  node_id: 99
  base_latency: 20
  bandwidth_bytes_per_time: 16

link:
  one_way_latency: 5

local_cache:
  capacity_bytes: 128
  hit_latency: 1
  policy: lru

workload:
  seed: 123
  compute_node_ids: [1, 2]
  object_count: 32
  object_size_bytes: 16
  requests_per_node_per_epoch: 4
  epoch_count: 2
  hot_set_size: 4
  hot_access_probability: 0.75
  hot_set_mode: epoch_shift
  cross_node_overlap: medium
)";
}

void test_valid_yaml_loads_experiment_and_simulation_config() {
    const std::filesystem::path path =
        write_temp_config("dm_sim_valid_config.yaml", valid_config_text());

    const ExperimentConfig experiment =
        dm_sim::load_experiment_config(path.string());
    assert(experiment.name == "loader_test");
    assert(experiment.output_dir == "/tmp/dm_sim_loader_test_results");

    const SimulationConfig& simulation = experiment.simulation;
    assert(simulation.memory_node_id == 99);
    assert(simulation.memory_base_latency == 20);
    assert(simulation.memory_bandwidth_bytes_per_time == 16);
    assert(simulation.one_way_link_latency == 5);
    assert(simulation.local_cache.capacity_bytes == 128);
    assert(simulation.local_cache.hit_latency == 1);
    assert(simulation.local_cache.policy_type == LocalCachePolicyType::Lru);
    assert(simulation.synthetic_workload.has_value());

    const auto& workload = *simulation.synthetic_workload;
    assert(workload.seed == 123);
    assert((workload.compute_node_ids == std::vector<dm_sim::NodeId>{1, 2}));
    assert(workload.object_count == 32);
    assert(workload.object_size_bytes == 16);
    assert(workload.hot_set_churn_fraction == 1.0);
    assert(workload.object_size_mode == ObjectSizeMode::Fixed);
    assert(workload.requests_per_node_per_epoch == 4);
    assert(workload.epoch_count == 2);
    assert(workload.hot_set_size == 4);
    assert(workload.hot_access_probability == 0.75);
    assert(workload.hot_set_mode == HotSetMode::EpochShift);
    assert(workload.cross_node_overlap == CrossNodeOverlap::Medium);

    const SimulationConfig loaded_simulation =
        dm_sim::load_simulation_config(path.string());
    assert(loaded_simulation.memory_node_id == 99);
}

void test_phase_b_workload_fields_parse() {
    std::string config = valid_config_text();
    config.replace(config.find("object_size_bytes: 16"),
                   std::string("object_size_bytes: 16").size(),
                   "object_size_bytes: 16\n"
                   "  hot_set_churn_fraction: 0.5\n"
                   "  object_size_mode: bimodal\n"
                   "  object_size_small_bytes: 8\n"
                   "  object_size_large_bytes: 64\n"
                   "  large_object_probability: 0.25");

    const std::filesystem::path path =
        write_temp_config("dm_sim_phase_b_config.yaml", config);
    const ExperimentConfig experiment =
        dm_sim::load_experiment_config(path.string());

    const auto& workload = *experiment.simulation.synthetic_workload;
    assert(workload.hot_set_churn_fraction == 0.5);
    assert(workload.object_size_mode == ObjectSizeMode::Bimodal);
    assert(workload.object_size_small_bytes == 8);
    assert(workload.object_size_large_bytes == 64);
    assert(workload.large_object_probability == 0.25);
}

void test_enum_strings_parse() {
    std::string config = valid_config_text();
    const std::string from = "policy: lru";
    const std::string to = "policy: always_remote";
    config.replace(config.find(from), from.size(), to);
    config.replace(config.find("hot_set_mode: epoch_shift"),
                   std::string("hot_set_mode: epoch_shift").size(),
                   "hot_set_mode: static");
    config.replace(config.find("cross_node_overlap: medium"),
                   std::string("cross_node_overlap: medium").size(),
                   "cross_node_overlap: high");

    const std::filesystem::path path =
        write_temp_config("dm_sim_enum_config.yaml", config);
    const ExperimentConfig experiment =
        dm_sim::load_experiment_config(path.string());

    assert(experiment.simulation.local_cache.policy_type ==
           LocalCachePolicyType::AlwaysRemote);
    assert(experiment.simulation.synthetic_workload->hot_set_mode ==
           HotSetMode::Static);
    assert(experiment.simulation.synthetic_workload->cross_node_overlap ==
           CrossNodeOverlap::High);
}

void test_phase6_policy_strings_parse() {
    std::string config = valid_config_text();
    config.replace(config.find("policy: lru"),
                   std::string("policy: lru").size(),
                   "policy: hotness_only\n"
                   "  hotness:\n"
                   "    min_admit_count: 3\n"
                   "    reset_on_epoch_change: false");

    const std::filesystem::path hotness_path =
        write_temp_config("dm_sim_hotness_config.yaml", config);
    const ExperimentConfig hotness_experiment =
        dm_sim::load_experiment_config(hotness_path.string());

    assert(hotness_experiment.simulation.local_cache.policy_type ==
           LocalCachePolicyType::HotnessOnly);
    assert(hotness_experiment.simulation.local_cache.hotness.min_admit_count == 3);
    assert(!hotness_experiment.simulation.local_cache.hotness.reset_on_epoch_change);

    config = valid_config_text();
    config.replace(config.find("policy: lru"),
                   std::string("policy: lru").size(),
                   "policy: global_hottest_replication");

    const std::filesystem::path global_path =
        write_temp_config("dm_sim_global_replication_config.yaml", config);
    const ExperimentConfig global_experiment =
        dm_sim::load_experiment_config(global_path.string());

    assert(global_experiment.simulation.local_cache.policy_type ==
           LocalCachePolicyType::GlobalHottestReplication);
}

void test_phase8_contention_policy_config_parses() {
    std::string config = valid_config_text();
    config.replace(config.find("policy: lru"),
                   std::string("policy: lru").size(),
                   "policy: contention_aware\n"
                   "  contention:\n"
                   "    local_hotness_weight: 1.2\n"
                   "    remote_access_weight: 1.3\n"
                   "    distinct_requester_weight: 1.4\n"
                   "    queue_wait_weight: 2.5\n"
                   "    remote_service_time_weight: 1.6\n"
                   "    size_penalty_weight: 0.7\n"
                   "    min_admit_score: 1.8\n"
                   "    local_hotness_threshold: 3\n"
                   "    reset_on_epoch_change: false");

    const std::filesystem::path path =
        write_temp_config("dm_sim_contention_config.yaml", config);
    const ExperimentConfig experiment =
        dm_sim::load_experiment_config(path.string());

    const auto& local_cache = experiment.simulation.local_cache;
    assert(local_cache.policy_type == LocalCachePolicyType::ContentionAware);
    assert(local_cache.contention.weights.local_hotness_weight == 1.2);
    assert(local_cache.contention.weights.remote_access_weight == 1.3);
    assert(local_cache.contention.weights.distinct_requester_weight == 1.4);
    assert(local_cache.contention.weights.queue_wait_weight == 2.5);
    assert(local_cache.contention.weights.remote_service_time_weight == 1.6);
    assert(local_cache.contention.weights.size_penalty_weight == 0.7);
    assert(local_cache.contention.min_admit_score == 1.8);
    assert(local_cache.contention.local_hotness_threshold == 3);
    assert(!local_cache.contention.reset_on_epoch_change);
}

void test_invalid_contention_policy_config_fails() {
    std::string config = valid_config_text();
    config.replace(config.find("policy: lru"),
                   std::string("policy: lru").size(),
                   "policy: contention_aware\n"
                   "  contention:\n"
                   "    queue_wait_weight: -1.0");
    const std::filesystem::path path =
        write_temp_config("dm_sim_invalid_contention_config.yaml", config);

    try {
        (void)dm_sim::load_experiment_config(path.string());
        assert(false);
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        assert(message.find("local_cache.contention.queue_wait_weight") !=
               std::string::npos);
    }
}

void test_invalid_phase_b_workload_fields_fail() {
    {
        std::string config = valid_config_text();
        config.replace(config.find("object_size_bytes: 16"),
                       std::string("object_size_bytes: 16").size(),
                       "object_size_bytes: 16\n"
                       "  hot_set_churn_fraction: 1.5");
        const std::filesystem::path path =
            write_temp_config("dm_sim_invalid_churn_config.yaml", config);

        try {
            (void)dm_sim::load_experiment_config(path.string());
            assert(false);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            assert(message.find("workload.hot_set_churn_fraction") !=
                   std::string::npos);
        }
    }

    {
        std::string config = valid_config_text();
        config.replace(config.find("object_size_bytes: 16"),
                       std::string("object_size_bytes: 16").size(),
                       "object_size_bytes: 16\n"
                       "  object_size_mode: triangular");
        const std::filesystem::path path =
            write_temp_config("dm_sim_invalid_size_mode_config.yaml", config);

        try {
            (void)dm_sim::load_experiment_config(path.string());
            assert(false);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            assert(message.find("workload.object_size_mode") !=
                   std::string::npos);
        }
    }

    {
        std::string config = valid_config_text();
        config.replace(config.find("object_size_bytes: 16"),
                       std::string("object_size_bytes: 16").size(),
                       "object_size_bytes: 16\n"
                       "  object_size_mode: bimodal\n"
                       "  object_size_small_bytes: 0");
        const std::filesystem::path path =
            write_temp_config("dm_sim_invalid_size_config.yaml", config);

        try {
            (void)dm_sim::load_experiment_config(path.string());
            assert(false);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            assert(message.find("workload.object_size_small_bytes") !=
                   std::string::npos);
        }
    }

    {
        std::string config = valid_config_text();
        config.replace(config.find("object_size_bytes: 16"),
                       std::string("object_size_bytes: 16").size(),
                       "object_size_bytes: 16\n"
                       "  large_object_probability: -0.25");
        const std::filesystem::path path =
            write_temp_config("dm_sim_invalid_large_probability.yaml", config);

        try {
            (void)dm_sim::load_experiment_config(path.string());
            assert(false);
        } catch (const std::invalid_argument& error) {
            const std::string message = error.what();
            assert(message.find("workload.large_object_probability") !=
                   std::string::npos);
        }
    }
}

void test_missing_required_field_fails() {
    const std::filesystem::path path =
        write_temp_config("dm_sim_missing_config.yaml", R"(experiment:
  name: missing_test
  output_dir: /tmp/missing
)");

    try {
        (void)dm_sim::load_experiment_config(path.string());
        assert(false);
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        assert(message.find("Missing required config field") != std::string::npos);
    }
}

void test_invalid_enum_value_fails() {
    std::string config = valid_config_text();
    config.replace(config.find("policy: lru"),
                   std::string("policy: lru").size(),
                   "policy: not_a_policy");
    const std::filesystem::path path =
        write_temp_config("dm_sim_invalid_enum.yaml", config);

    try {
        (void)dm_sim::load_experiment_config(path.string());
        assert(false);
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        assert(message.find("local_cache.policy") != std::string::npos);
    }
}

}  // namespace

int main() {
    test_valid_yaml_loads_experiment_and_simulation_config();
    test_phase_b_workload_fields_parse();
    test_enum_strings_parse();
    test_phase6_policy_strings_parse();
    test_phase8_contention_policy_config_parses();
    test_invalid_contention_policy_config_fails();
    test_invalid_phase_b_workload_fields_fail();
    test_missing_required_field_fails();
    test_invalid_enum_value_fails();
    return 0;
}
