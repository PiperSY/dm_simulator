#include "experiments/runner.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include "metrics/histogram.hpp"

namespace dm_sim {

namespace {

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

std::string served_from_tier_to_string(ServedFromTier tier) {
    switch (tier) {
    case ServedFromTier::LocalCache:
        return "local_cache";
    case ServedFromTier::Memory:
        return "memory";
    }

    throw std::invalid_argument("Unknown served-from tier");
}

void write_summary_json(const std::filesystem::path& path,
                        const MetricsSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open summary output: " +
                                 path.string());
    }

    output << std::fixed << std::setprecision(6);
    output << "{\n";
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

}  // namespace

MetricsSummary summarize_metrics(const std::string& experiment_name,
                                 const Simulator& simulator) {
    const Stats& stats = simulator.stats();

    MetricsSummary summary;
    summary.experiment_name = experiment_name;
    summary.completed_requests = stats.completed_requests();
    summary.mean_latency = stats.average_latency();
    summary.median_latency = dm_sim::median_latency(stats.latencies());
    summary.p95_latency = dm_sim::p95_latency(stats.latencies());
    summary.p99_latency = dm_sim::p99_latency(stats.latencies());
    summary.local_cache_hits = stats.local_cache_hits();
    summary.local_cache_misses = stats.local_cache_misses();
    summary.local_cache_hit_rate = stats.local_cache_hit_rate();
    summary.memory_average_wait = stats.average_memory_wait();
    summary.memory_max_wait = stats.max_memory_wait();
    summary.memory_peak_queue_depth = stats.peak_memory_queue_depth();

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

ExperimentResult ExperimentRunner::run_config(
    const std::string& config_path,
    const std::optional<std::string>& output_dir_override) const {
    ExperimentConfig config = load_experiment_config(config_path);
    const std::string output_dir =
        output_dir_override.has_value() ? *output_dir_override : config.output_dir;

    Simulator simulator(config.simulation);
    simulator.run();

    MetricsSummary summary = summarize_metrics(config.name, simulator);

    const std::filesystem::path output_path(output_dir);
    std::filesystem::create_directories(output_path);
    write_summary_json(output_path / "summary.json", summary);
    write_per_node_csv(output_path / "per_node.csv", summary);
    write_latencies_csv(output_path / "latencies.csv", simulator);

    return ExperimentResult{config, summary, output_dir};
}

}  // namespace dm_sim
