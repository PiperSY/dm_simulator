#include "experiments/runner.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "metrics/histogram.hpp"

namespace dm_sim {

namespace {

// Escapes characters that need special handling before writing a string into
// JSON output.
std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (char character : value) {
        switch (character) {
        case '"':
            escaped << "\\\"";
            break;
        case '\\':
            escaped << "\\\\";
            break;
        case '\n':
            escaped << "\\n";
            break;
        case '\r':
            escaped << "\\r";
            break;
        case '\t':
            escaped << "\\t";
            break;
        default:
            escaped << character;
            break;
        }
    }

    return escaped.str();
}

// Converts a response source tier into the stable label used in CSV output.
std::string served_from_tier_to_string(ServedFromTier tier) {
    switch (tier) {
    case ServedFromTier::LocalCache:
        return "local_cache";
    case ServedFromTier::Memory:
        return "memory";
    }

    throw std::invalid_argument("Unknown served-from tier");
}

// Writes one object-level contention summary as a compact JSON object.
void write_contention_object_json(std::ofstream& output,
                                  const ObjectContentionStats& stats) {
    output << "{"
           << "\"epoch_id\": " << stats.epoch_id << ", "
           << "\"object_id\": " << stats.object_id << ", "
           << "\"remote_accesses\": " << stats.remote_accesses << ", "
           << "\"distinct_requesters\": " << stats.distinct_requesters << ", "
           << "\"bytes_served\": " << stats.bytes_served << ", "
           << "\"total_remote_service_time\": "
           << stats.total_remote_service_time << ", "
           << "\"total_queue_wait\": " << stats.total_queue_wait << ", "
           << "\"average_queue_wait\": " << stats.average_queue_wait << "}";
}

// Writes a JSON array of object-level contention summaries.
void write_contention_array_json(
    std::ofstream& output,
    const std::vector<ObjectContentionStats>& objects) {
    output << "[";
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (i > 0) {
            output << ", ";
        }
        write_contention_object_json(output, objects[i]);
    }
    output << "]";
}

void write_policy_decision_json(std::ofstream& output,
                                const PolicyDecisionRecord& decision) {
    output << "{"
           << "\"node_id\": " << decision.node_id << ", "
           << "\"epoch_id\": " << decision.epoch_id << ", "
           << "\"request_id\": " << decision.request_id << ", "
           << "\"object_id\": " << decision.object_id << ", "
           << "\"admitted\": " << (decision.admitted ? "true" : "false")
           << ", "
           << "\"reason\": \"" << json_escape(decision.reason) << "\", "
           << "\"total_score\": " << decision.score.total_score << "}";
}

void write_policy_decision_array_json(
    std::ofstream& output,
    const std::vector<PolicyDecisionRecord>& decisions) {
    output << "[";
    for (std::size_t i = 0; i < decisions.size(); ++i) {
        if (i > 0) {
            output << ", ";
        }
        write_policy_decision_json(output, decisions[i]);
    }
    output << "]";
}

std::string join_evicted_objects(const std::vector<ObjectId>& objects) {
    std::ostringstream joined;
    for (std::size_t i = 0; i < objects.size(); ++i) {
        if (i > 0) {
            joined << "|";
        }
        joined << objects[i];
    }

    return joined.str();
}

// Writes the main experiment summary report, including aggregate, contention,
// and per-node metrics.
void write_summary_json(const std::filesystem::path& path,
                        const MetricsSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open summary output: " +
                                 path.string());
    }

    output << std::fixed << std::setprecision(6);
    output << "{\n";
    // Keep the top-level sections aligned with the summary fields consumed by
    // reports and tests.
    output << "  \"experiment_name\": \""
           << json_escape(summary.experiment_name) << "\",\n";
    output << "  \"completed_requests\": "
           << summary.completed_requests << ",\n";
    output << "  \"latency\": {\n";
    output << "    \"mean\": " << summary.mean_latency << ",\n";
    output << "    \"median\": " << summary.median_latency << ",\n";
    output << "    \"p95\": " << summary.p95_latency << ",\n";
    output << "    \"p99\": " << summary.p99_latency << "\n";
    output << "  },\n";
    output << "  \"cache\": {\n";
    output << "    \"local_hits\": " << summary.local_cache_hits << ",\n";
    output << "    \"local_misses\": " << summary.local_cache_misses << ",\n";
    output << "    \"local_hit_rate\": "
           << summary.local_cache_hit_rate << "\n";
    output << "  },\n";
    output << "  \"memory\": {\n";
    output << "    \"average_wait\": " << summary.memory_average_wait << ",\n";
    output << "    \"max_wait\": " << summary.memory_max_wait << ",\n";
    output << "    \"peak_queue_depth\": "
           << summary.memory_peak_queue_depth << "\n";
    output << "  },\n";
    output << "  \"contention\": {\n";
    output << "    \"top_by_queue_wait\": ";
    write_contention_array_json(output, summary.top_by_queue_wait);
    output << ",\n";
    output << "    \"top_by_service_time\": ";
    write_contention_array_json(output, summary.top_by_service_time);
    output << "\n";
    output << "  },\n";
    output << "  \"policy\": {\n";
    output << "    \"admitted\": " << summary.policy_admitted << ",\n";
    output << "    \"rejected\": " << summary.policy_rejected << ",\n";
    output << "    \"top_decisions_by_score\": ";
    write_policy_decision_array_json(output, summary.top_policy_decisions);
    output << "\n";
    output << "  },\n";
    // Per-node rows are emitted as an array so the JSON mirrors per_node.csv.
    output << "  \"per_node\": [\n";
    for (std::size_t i = 0; i < summary.per_node.size(); ++i) {
        const PerNodeMetricsSummary& node = summary.per_node[i];
        output << "    {"
               << "\"node_id\": " << node.node_id << ", "
               << "\"completed_requests\": " << node.completed_requests << ", "
               << "\"mean_latency\": " << node.mean_latency << ", "
               << "\"p99_latency\": " << node.p99_latency << ", "
               << "\"local_cache_hits\": " << node.local_cache_hits << ", "
               << "\"local_cache_misses\": " << node.local_cache_misses << ", "
               << "\"local_cache_hit_rate\": "
               << node.local_cache_hit_rate << "}";
        if (i + 1 < summary.per_node.size()) {
            output << ",";
        }
        output << "\n";
    }
    output << "  ]\n";
    output << "}\n";
}

// Writes a CSV report containing one metrics summary row per compute node.
void write_per_node_csv(const std::filesystem::path& path,
                        const MetricsSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open per-node output: " +
                                 path.string());
    }

    output << "node_id,completed_requests,mean_latency,p99_latency,"
              "local_cache_hits,local_cache_misses,local_cache_hit_rate\n";
    output << std::fixed << std::setprecision(6);
    for (const PerNodeMetricsSummary& node : summary.per_node) {
        output << node.node_id << ","
               << node.completed_requests << ","
               << node.mean_latency << ","
               << node.p99_latency << ","
               << node.local_cache_hits << ","
               << node.local_cache_misses << ","
               << node.local_cache_hit_rate << "\n";
    }
}

// Writes one CSV row per completed response, joined with request metadata.
void write_latencies_csv(const std::filesystem::path& path,
                         const Simulator& simulator) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open latency output: " +
                                 path.string());
    }

    output << "request_id,source_node_id,object_id,epoch_id,served_from_tier,"
              "completion_time,total_latency,bytes_transferred\n";
    for (const Response& response : simulator.responses()) {
        const Request& request = simulator.requests().at(response.request_id);
        output << response.request_id << ","
               << request.source_node_id << ","
               << response.object_id << ","
               << request.epoch_id << ","
               << served_from_tier_to_string(response.served_from_tier) << ","
               << response.completion_time << ","
               << response.total_latency << ","
               << response.bytes_transferred << "\n";
    }
}

// Writes all recorded object-contention metrics as a CSV report.
void write_contention_by_object_csv(const std::filesystem::path& path,
                                    const Simulator& simulator) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open contention output: " +
                                 path.string());
    }

    output << "epoch_id,object_id,remote_accesses,distinct_requesters,"
              "bytes_served,total_remote_service_time,total_queue_wait,"
              "max_queue_wait,queue_wait_samples,max_observed_queue_depth,"
              "average_queue_wait\n";
    output << std::fixed << std::setprecision(6);
    for (const ObjectContentionStats& stats :
         simulator.stats().all_contention_stats()) {
        output << stats.epoch_id << ","
               << stats.object_id << ","
               << stats.remote_accesses << ","
               << stats.distinct_requesters << ","
               << stats.bytes_served << ","
               << stats.total_remote_service_time << ","
               << stats.total_queue_wait << ","
               << stats.max_queue_wait << ","
               << stats.queue_wait_samples << ","
               << stats.max_observed_queue_depth << ","
               << stats.average_queue_wait << "\n";
    }
}

void write_policy_diagnostics_csv(const std::filesystem::path& path,
                                  const Simulator& simulator) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open policy diagnostics output: " +
                                 path.string());
    }

    output << "node_id,epoch_id,request_id,object_id,admitted,reason,"
              "total_score,local_hotness,remote_accesses,distinct_requesters,"
              "queue_wait,remote_service_time,size_penalty,evicted_objects\n";
    output << std::fixed << std::setprecision(6);
    for (const PolicyDecisionRecord& decision :
         simulator.policy_diagnostics()) {
        output << decision.node_id << ","
               << decision.epoch_id << ","
               << decision.request_id << ","
               << decision.object_id << ","
               << (decision.admitted ? "true" : "false") << ","
               << decision.reason << ","
               << decision.score.total_score << ","
               << decision.score.local_hotness << ","
               << decision.score.remote_accesses << ","
               << decision.score.distinct_requesters << ","
               << decision.score.queue_wait << ","
               << decision.score.remote_service_time << ","
               << decision.score.size_penalty << ","
               << join_evicted_objects(decision.evicted_objects) << "\n";
    }
}

}  // namespace

// Builds a report-friendly metrics summary from the simulator's recorded
// statistics.
MetricsSummary summarize_metrics(const std::string& experiment_name,
                                 const Simulator& simulator) {
    const Stats& stats = simulator.stats();

    MetricsSummary summary;
    summary.experiment_name = experiment_name;
    summary.completed_requests = stats.completed_requests();
    summary.mean_latency = stats.average_latency();
    summary.median_latency = median_latency(stats.latencies());
    summary.p95_latency = dm_sim::p95_latency(stats.latencies());
    summary.p99_latency = dm_sim::p99_latency(stats.latencies());
    summary.local_cache_hits = stats.local_cache_hits();
    summary.local_cache_misses = stats.local_cache_misses();
    summary.local_cache_hit_rate = stats.local_cache_hit_rate();
    summary.memory_average_wait = stats.average_memory_wait();
    summary.memory_max_wait = stats.max_memory_wait();
    summary.memory_peak_queue_depth = stats.peak_memory_queue_depth();
    // Keep the most contended objects by the two metrics surfaced in the JSON
    // summary.
    summary.top_by_queue_wait =
        stats.top_contention_objects(ContentionSortKey::TotalQueueWait, 5);
    summary.top_by_service_time =
        stats.top_contention_objects(ContentionSortKey::TotalRemoteServiceTime, 5);

    std::vector<PolicyDecisionRecord> policy_diagnostics =
        simulator.policy_diagnostics();
    for (const PolicyDecisionRecord& decision : policy_diagnostics) {
        if (decision.admitted) {
            ++summary.policy_admitted;
        } else {
            ++summary.policy_rejected;
        }
    }

    std::sort(policy_diagnostics.begin(),
              policy_diagnostics.end(),
              [](const PolicyDecisionRecord& lhs,
                 const PolicyDecisionRecord& rhs) {
                  if (lhs.score.total_score != rhs.score.total_score) {
                      return lhs.score.total_score > rhs.score.total_score;
                  }
                  if (lhs.epoch_id != rhs.epoch_id) {
                      return lhs.epoch_id < rhs.epoch_id;
                  }
                  return lhs.object_id < rhs.object_id;
              });
    if (policy_diagnostics.size() > 5) {
        policy_diagnostics.resize(5);
    }
    summary.top_policy_decisions = std::move(policy_diagnostics);

    // Preserve the compute-node order from the experiment configuration.
    for (const ComputeNodeConfig& node_config : simulator.config().compute_nodes) {
        summary.per_node.push_back(PerNodeMetricsSummary{
            node_config.node_id,
            stats.completed_requests(node_config.node_id),
            stats.average_latency(node_config.node_id),
            dm_sim::p99_latency(stats.latencies(node_config.node_id)),
            stats.local_cache_hits(node_config.node_id),
            stats.local_cache_misses(node_config.node_id),
            stats.local_cache_hit_rate(node_config.node_id),
        });
    }

    return summary;
}

// Loads an experiment configuration, runs the simulator, writes report files,
// and returns the full experiment result.
ExperimentResult ExperimentRunner::run_config(
    const std::string& config_path,
    const std::optional<std::string>& output_dir_override) const {
    ExperimentConfig config = load_experiment_config(config_path);
    // Allow callers to redirect output without changing the experiment config.
    const std::string output_dir =
        output_dir_override.has_value() ? *output_dir_override : config.output_dir;

    Simulator simulator(config.simulation);
    simulator.run();

    MetricsSummary summary = summarize_metrics(config.name, simulator);

    // Create the output directory before writing all generated report files.
    const std::filesystem::path output_path(output_dir);
    std::filesystem::create_directories(output_path);
    write_summary_json(output_path / "summary.json", summary);
    write_per_node_csv(output_path / "per_node.csv", summary);
    write_latencies_csv(output_path / "latencies.csv", simulator);
    write_contention_by_object_csv(output_path / "contention_by_object.csv",
                                   simulator);
    write_policy_diagnostics_csv(output_path / "policy_diagnostics.csv",
                                 simulator);

    return ExperimentResult{config, summary, output_dir};
}

}  // namespace dm_sim
