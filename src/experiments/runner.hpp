#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "sim/config_loader.hpp"
#include "sim/simulator.hpp"
#include "sim/types.hpp"

namespace dm_sim {

// Summary of metrics for a single node.
struct PerNodeMetricsSummary {
    NodeId node_id = 0;
    std::size_t completed_requests = 0;
    double mean_latency = 0.0;
    double p99_latency = 0.0;
    std::size_t local_cache_hits = 0;
    std::size_t local_cache_misses = 0;
    double local_cache_hit_rate = 0.0;
};

// Summary of metrics for a single experiment, including overall statistics and per-node breakdowns.
struct MetricsSummary {
    std::string experiment_name;
    std::size_t completed_requests = 0;
    double mean_latency = 0.0;
    double median_latency = 0.0;
    double p95_latency = 0.0;
    double p99_latency = 0.0;
    std::size_t local_cache_hits = 0;
    std::size_t local_cache_misses = 0;
    double local_cache_hit_rate = 0.0;
    double memory_average_wait = 0.0;
    SimTime memory_max_wait = 0;
    std::size_t memory_peak_queue_depth = 0;
    std::vector<ObjectContentionStats> top_by_queue_wait;
    std::vector<ObjectContentionStats> top_by_service_time;
    std::vector<PerNodeMetricsSummary> per_node;
};

// Result of running an experiment, including the configuration, summarized metrics, and output directory for results.
struct ExperimentResult {
    ExperimentConfig config;
    MetricsSummary summary;
    std::string output_dir;
};

// Helper function to summarize metrics from a Simulator instance into a MetricsSummary structure for reporting and analysis.
[[nodiscard]] MetricsSummary summarize_metrics(
    const std::string& experiment_name,
    const Simulator& simulator);

// ExperimentRunner class responsible for running experiments based on configuration files, executing simulations, and summarizing results.
class ExperimentRunner {
public:
    // Runs an experiment based on the provided configuration file path and an optional output directory override. 
    // It loads the experiment configuration, initializes and runs the simulator, summarizes the metrics, and returns the results.
    [[nodiscard]] ExperimentResult run_config(
        const std::string& config_path,
        const std::optional<std::string>& output_dir_override = std::nullopt)
        const;
};

}  // namespace dm_sim
