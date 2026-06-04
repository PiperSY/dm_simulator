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

struct EpochDiagnosticSummary {
    EpochId epoch_id = 0;
    std::size_t previous_top_count = 0;
    std::size_t current_top_count = 0;
    std::size_t overlap_count = 0;
    double top_object_overlap = 0.0;
    double stale_telemetry_rate = 0.0;
    std::vector<ObjectId> previous_top_contended;
    std::vector<ObjectId> current_top_requested;
};

// Run-level diagnostics intended to explain policy viability, not just raw
// latency. Most fields are derived after the simulation from cache lifecycle,
// request, response, and contention telemetry.
struct ViabilityMetricsSummary {
    std::size_t telemetry_comparison_object_count = 5;
    std::size_t admission_attempts = 0;
    std::size_t successful_placements = 0;
    std::size_t rejected_admissions = 0;
    std::size_t total_future_hits = 0;
    double admission_yield = 0.0;
    std::size_t placements_with_reuse = 0;
    double reuse_after_admit_rate = 0.0;
    double stale_telemetry_rate = 0.0;
    double average_top_object_overlap = 0.0;
    std::size_t estimated_avoided_remote_accesses = 0;
    double estimated_avoided_queue_wait = 0.0;
    double estimated_avoided_remote_service_time = 0.0;
    std::size_t eviction_regret_count = 0;
    std::size_t remote_eviction_regret_count = 0;
    double per_node_mean_latency_spread = 0.0;
    double per_node_p99_latency_spread = 0.0;
    double jain_inverse_latency_fairness = 0.0;
    std::vector<EpochDiagnosticSummary> epoch_diagnostics;
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
    std::uint64_t memory_channel_count = 1;
    std::size_t memory_peak_channel_queue_depth = 0;
    SimTime max_channel_total_queue_wait = 0;
    double channel_queue_imbalance = 0.0;
    std::vector<ObjectContentionStats> top_by_queue_wait;
    std::vector<ObjectContentionStats> top_by_service_time;
    std::size_t policy_admitted = 0;
    std::size_t policy_rejected = 0;
    std::vector<PolicyDecisionRecord> top_policy_decisions;
    std::vector<PerNodeMetricsSummary> per_node;
    ViabilityMetricsSummary viability;
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
[[nodiscard]] ViabilityMetricsSummary summarize_viability_metrics(
    const Simulator& simulator,
    const std::vector<PerNodeMetricsSummary>& per_node);

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
