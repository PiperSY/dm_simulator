#include "experiments/runner.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
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

std::vector<ObjectId> top_contended_objects(const Stats& stats,
                                            EpochId epoch_id,
                                            std::size_t limit) {
    std::vector<ObjectContentionStats> objects =
        stats.contention_by_epoch(epoch_id);
    std::sort(objects.begin(),
              objects.end(),
              [](const ObjectContentionStats& lhs,
                 const ObjectContentionStats& rhs) {
                  // Rank by "pain at the memory bottleneck" first, then by
                  // service demand. This mirrors the stale-telemetry question:
                  // did last epoch's painful objects stay important?
                  if (lhs.total_queue_wait != rhs.total_queue_wait) {
                      return lhs.total_queue_wait > rhs.total_queue_wait;
                  }
                  if (lhs.total_remote_service_time !=
                      rhs.total_remote_service_time) {
                      return lhs.total_remote_service_time >
                             rhs.total_remote_service_time;
                  }
                  if (lhs.remote_accesses != rhs.remote_accesses) {
                      return lhs.remote_accesses > rhs.remote_accesses;
                  }
                  return lhs.object_id < rhs.object_id;
              });

    std::vector<ObjectId> top_objects;
    const std::size_t count = std::min(limit, objects.size());
    top_objects.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        top_objects.push_back(objects[i].object_id);
    }
    return top_objects;
}

std::vector<ObjectId> top_requested_objects(
    const std::unordered_map<RequestId, Request>& requests,
    EpochId epoch_id,
    std::size_t limit) {
    std::unordered_map<ObjectId, std::size_t> request_counts;
    for (const auto& request_entry : requests) {
        const Request& request = request_entry.second;
        if (request.epoch_id == epoch_id) {
            // Use all issued requests, not just remote misses. A local hit is
            // still true demand and should count when judging whether previous
            // contention telemetry predicted the next epoch's hot objects.
            ++request_counts[request.object_id];
        }
    }

    std::vector<std::pair<ObjectId, std::size_t>> ranked_requests{
        request_counts.begin(),
        request_counts.end(),
    };
    std::sort(ranked_requests.begin(),
              ranked_requests.end(),
              [](const auto& lhs, const auto& rhs) {
                  if (lhs.second != rhs.second) {
                      return lhs.second > rhs.second;
                  }
                  return lhs.first < rhs.first;
              });

    std::vector<ObjectId> top_objects;
    const std::size_t count = std::min(limit, ranked_requests.size());
    top_objects.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        top_objects.push_back(ranked_requests[i].first);
    }
    return top_objects;
}

std::size_t overlap_count(const std::vector<ObjectId>& lhs,
                          const std::vector<ObjectId>& rhs) {
    std::unordered_set<ObjectId> rhs_objects{rhs.begin(), rhs.end()};
    std::size_t count = 0;
    for (ObjectId object_id : lhs) {
        if (rhs_objects.find(object_id) != rhs_objects.end()) {
            ++count;
        }
    }
    return count;
}

std::vector<EpochId> request_epochs(
    const std::unordered_map<RequestId, Request>& requests) {
    std::unordered_set<EpochId> unique_epochs;
    for (const auto& request_entry : requests) {
        unique_epochs.insert(request_entry.second.epoch_id);
    }

    std::vector<EpochId> epochs{unique_epochs.begin(), unique_epochs.end()};
    std::sort(epochs.begin(), epochs.end());
    return epochs;
}

double value_spread(const std::vector<double>& values) {
    if (values.empty()) {
        return 0.0;
    }

    const auto [minimum, maximum] =
        std::minmax_element(values.begin(), values.end());
    return *maximum - *minimum;
}

double jain_inverse_latency_fairness(
    const std::vector<PerNodeMetricsSummary>& per_node) {
    double sum = 0.0;
    double sum_squares = 0.0;
    std::size_t samples = 0;
    for (const PerNodeMetricsSummary& node : per_node) {
        if (node.mean_latency <= 0.0) {
            continue;
        }

        // Jain's index is larger when benefit is evenly distributed. Inverse
        // latency turns lower-latency nodes into larger "benefit" values.
        const double inverse_latency = 1.0 / node.mean_latency;
        sum += inverse_latency;
        sum_squares += inverse_latency * inverse_latency;
        ++samples;
    }

    if (samples == 0 || sum_squares == 0.0) {
        return 0.0;
    }

    return (sum * sum) / (static_cast<double>(samples) * sum_squares);
}

std::unordered_map<RequestId, ServedFromTier> served_tiers_by_request(
    const std::vector<Response>& responses) {
    std::unordered_map<RequestId, ServedFromTier> served_tiers;
    for (const Response& response : responses) {
        served_tiers[response.request_id] = response.served_from_tier;
    }
    return served_tiers;
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
    output << "  \"viability\": {\n";
    output << "    \"top_k\": " << summary.viability.top_k << ",\n";
    output << "    \"admission_attempts\": "
           << summary.viability.admission_attempts << ",\n";
    output << "    \"successful_placements\": "
           << summary.viability.successful_placements << ",\n";
    output << "    \"rejected_admissions\": "
           << summary.viability.rejected_admissions << ",\n";
    output << "    \"admission_yield\": "
           << summary.viability.admission_yield << ",\n";
    output << "    \"reuse_after_admit_rate\": "
           << summary.viability.reuse_after_admit_rate << ",\n";
    output << "    \"stale_telemetry_rate\": "
           << summary.viability.stale_telemetry_rate << ",\n";
    output << "    \"average_top_object_overlap\": "
           << summary.viability.average_top_object_overlap << ",\n";
    output << "    \"estimated_avoided_remote_accesses\": "
           << summary.viability.estimated_avoided_remote_accesses << ",\n";
    output << "    \"estimated_avoided_queue_wait\": "
           << summary.viability.estimated_avoided_queue_wait << ",\n";
    output << "    \"estimated_avoided_remote_service_time\": "
           << summary.viability.estimated_avoided_remote_service_time << ",\n";
    output << "    \"eviction_regret_count\": "
           << summary.viability.eviction_regret_count << ",\n";
    output << "    \"remote_eviction_regret_count\": "
           << summary.viability.remote_eviction_regret_count << ",\n";
    output << "    \"per_node_mean_latency_spread\": "
           << summary.viability.per_node_mean_latency_spread << ",\n";
    output << "    \"per_node_p99_latency_spread\": "
           << summary.viability.per_node_p99_latency_spread << ",\n";
    output << "    \"jain_inverse_latency_fairness\": "
           << summary.viability.jain_inverse_latency_fairness << "\n";
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

void write_cache_admissions_csv(const std::filesystem::path& path,
                                const Simulator& simulator) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open cache admissions output: " +
                                 path.string());
    }

    // This file is intentionally policy-neutral: LRU, hotness, oracle
    // replication, and contention-aware all emit comparable lifecycle rows.
    output << "node_id,epoch_id,request_id,object_id,time,admitted,reason,"
              "placement_source,size_bytes,future_hit_count,"
              "reused_after_admit,evicted,eviction_time,evicted_objects\n";
    for (const CacheAdmissionRecord& record :
         simulator.cache_admission_diagnostics()) {
        output << record.node_id << ","
               << record.epoch_id << ","
               << record.request_id << ","
               << record.object_id << ","
               << record.time << ","
               << (record.admitted ? "true" : "false") << ","
               << record.reason << ","
               << record.placement_source << ","
               << record.size_bytes << ","
               << record.future_hit_count << ","
               << (record.reused_after_admit ? "true" : "false") << ","
               << (record.evicted ? "true" : "false") << ",";
        if (record.evicted) {
            output << record.eviction_time;
        }
        output << "," << join_evicted_objects(record.evicted_objects) << "\n";
    }
}

void write_epoch_diagnostics_csv(const std::filesystem::path& path,
                                 const MetricsSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open epoch diagnostics output: " +
                                 path.string());
    }

    // Each row compares previous-epoch contention against current-epoch demand.
    // Low overlap means prior telemetry is stale for that epoch transition.
    output << "epoch_id,previous_top_count,current_top_count,overlap_count,"
              "top_object_overlap,stale_telemetry_rate,"
              "previous_top_contended,current_top_requested\n";
    output << std::fixed << std::setprecision(6);
    for (const EpochDiagnosticSummary& epoch :
         summary.viability.epoch_diagnostics) {
        output << epoch.epoch_id << ","
               << epoch.previous_top_count << ","
               << epoch.current_top_count << ","
               << epoch.overlap_count << ","
               << epoch.top_object_overlap << ","
               << epoch.stale_telemetry_rate << ","
               << join_evicted_objects(epoch.previous_top_contended) << ","
               << join_evicted_objects(epoch.current_top_requested) << "\n";
    }
}

void write_viability_metrics_csv(const std::filesystem::path& path,
                                 const MetricsSummary& summary) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error("Failed to open viability metrics output: " +
                                 path.string());
    }

    output << "top_k,admission_attempts,successful_placements,"
              "rejected_admissions,total_future_hits,admission_yield,"
              "placements_with_reuse,reuse_after_admit_rate,"
              "stale_telemetry_rate,average_top_object_overlap,"
              "estimated_avoided_remote_accesses,"
              "estimated_avoided_queue_wait,"
              "estimated_avoided_remote_service_time,"
              "eviction_regret_count,remote_eviction_regret_count,"
              "per_node_mean_latency_spread,per_node_p99_latency_spread,"
              "jain_inverse_latency_fairness\n";

    const ViabilityMetricsSummary& viability = summary.viability;
    output << std::fixed << std::setprecision(6);
    output << viability.top_k << ","
           << viability.admission_attempts << ","
           << viability.successful_placements << ","
           << viability.rejected_admissions << ","
           << viability.total_future_hits << ","
           << viability.admission_yield << ","
           << viability.placements_with_reuse << ","
           << viability.reuse_after_admit_rate << ","
           << viability.stale_telemetry_rate << ","
           << viability.average_top_object_overlap << ","
           << viability.estimated_avoided_remote_accesses << ","
           << viability.estimated_avoided_queue_wait << ","
           << viability.estimated_avoided_remote_service_time << ","
           << viability.eviction_regret_count << ","
           << viability.remote_eviction_regret_count << ","
           << viability.per_node_mean_latency_spread << ","
           << viability.per_node_p99_latency_spread << ","
           << viability.jain_inverse_latency_fairness << "\n";
}

}  // namespace

ViabilityMetricsSummary summarize_viability_metrics(
    const Simulator& simulator,
    const std::vector<PerNodeMetricsSummary>& per_node) {
    constexpr std::size_t kTopK = 5;

    ViabilityMetricsSummary summary;
    summary.top_k = kTopK;

    const std::vector<CacheAdmissionRecord> admission_records =
        simulator.cache_admission_diagnostics();
    summary.admission_attempts = admission_records.size();
    // Admission yield is measured after the fact: every successful placement
    // records how many later local hits it produced before eviction/end.
    for (const CacheAdmissionRecord& record : admission_records) {
        if (!record.admitted) {
            ++summary.rejected_admissions;
            continue;
        }

        ++summary.successful_placements;
        summary.total_future_hits += record.future_hit_count;
        if (record.reused_after_admit) {
            ++summary.placements_with_reuse;
        }
    }

    if (summary.successful_placements > 0) {
        summary.admission_yield =
            static_cast<double>(summary.total_future_hits) /
            static_cast<double>(summary.successful_placements);
        summary.reuse_after_admit_rate =
            static_cast<double>(summary.placements_with_reuse) /
            static_cast<double>(summary.successful_placements);
    }

    double overlap_sum = 0.0;
    double stale_sum = 0.0;
    std::size_t comparable_epochs = 0;
    // Staleness is computed at epoch boundaries using previous contention and
    // current request demand. Epoch 0 has no prior telemetry by design.
    for (EpochId epoch_id : request_epochs(simulator.requests())) {
        if (epoch_id == 0) {
            continue;
        }

        EpochDiagnosticSummary epoch;
        epoch.epoch_id = epoch_id;
        epoch.previous_top_contended =
            top_contended_objects(simulator.stats(), epoch_id - 1, kTopK);
        epoch.current_top_requested =
            top_requested_objects(simulator.requests(), epoch_id, kTopK);
        epoch.previous_top_count = epoch.previous_top_contended.size();
        epoch.current_top_count = epoch.current_top_requested.size();
        epoch.overlap_count = overlap_count(epoch.previous_top_contended,
                                            epoch.current_top_requested);
        if (epoch.previous_top_count > 0) {
            epoch.top_object_overlap =
                static_cast<double>(epoch.overlap_count) /
                static_cast<double>(epoch.previous_top_count);
            epoch.stale_telemetry_rate = 1.0 - epoch.top_object_overlap;
            overlap_sum += epoch.top_object_overlap;
            stale_sum += epoch.stale_telemetry_rate;
            ++comparable_epochs;
        }

        summary.epoch_diagnostics.push_back(std::move(epoch));
    }

    if (comparable_epochs > 0) {
        summary.average_top_object_overlap =
            overlap_sum / static_cast<double>(comparable_epochs);
        summary.stale_telemetry_rate =
            stale_sum / static_cast<double>(comparable_epochs);
    }

    for (const Response& response : simulator.responses()) {
        if (response.served_from_tier != ServedFromTier::LocalCache) {
            continue;
        }

        ++summary.estimated_avoided_remote_accesses;
        const Request& request = simulator.requests().at(response.request_id);
        const std::optional<ObjectContentionStats> previous_stats =
            simulator.stats().previous_epoch_object_contention(
                request.epoch_id,
                request.object_id);
        if (!previous_stats.has_value() ||
            previous_stats->remote_accesses == 0) {
            continue;
        }

        // Relief is an estimate, not a counterfactual rerun. We value each
        // local hit using the object's average remote cost from the previous
        // epoch to stay consistent with the policy's prior-telemetry model.
        const double remote_accesses =
            static_cast<double>(previous_stats->remote_accesses);
        summary.estimated_avoided_queue_wait +=
            static_cast<double>(previous_stats->total_queue_wait) /
            remote_accesses;
        summary.estimated_avoided_remote_service_time +=
            static_cast<double>(previous_stats->total_remote_service_time) /
            remote_accesses;
    }

    const std::unordered_map<RequestId, ServedFromTier> served_tiers =
        served_tiers_by_request(simulator.responses());
    // Eviction regret asks whether a removed object was requested again by the
    // same node. Remote-regret is the stricter subset where that later request
    // had to go back to memory.
    for (const CacheAdmissionRecord& record : admission_records) {
        if (!record.admitted || !record.evicted) {
            continue;
        }

        for (const auto& request_entry : simulator.requests()) {
            const Request& request = request_entry.second;
            if (request.source_node_id != record.node_id ||
                request.object_id != record.object_id ||
                request.issue_time < record.eviction_time) {
                continue;
            }

            ++summary.eviction_regret_count;
            const auto served_it = served_tiers.find(request.request_id);
            if (served_it != served_tiers.end() &&
                served_it->second == ServedFromTier::Memory) {
                ++summary.remote_eviction_regret_count;
            }
        }
    }

    std::vector<double> mean_latencies;
    std::vector<double> p99_latencies;
    mean_latencies.reserve(per_node.size());
    p99_latencies.reserve(per_node.size());
    for (const PerNodeMetricsSummary& node : per_node) {
        mean_latencies.push_back(node.mean_latency);
        p99_latencies.push_back(node.p99_latency);
    }
    summary.per_node_mean_latency_spread = value_spread(mean_latencies);
    summary.per_node_p99_latency_spread = value_spread(p99_latencies);
    summary.jain_inverse_latency_fairness =
        jain_inverse_latency_fairness(per_node);

    return summary;
}

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

    summary.viability = summarize_viability_metrics(simulator, summary.per_node);

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
    write_cache_admissions_csv(output_path / "cache_admissions.csv",
                               simulator);
    write_epoch_diagnostics_csv(output_path / "epoch_diagnostics.csv",
                                summary);
    write_viability_metrics_csv(output_path / "viability_metrics.csv",
                                summary);

    return ExperimentResult{config, summary, output_dir};
}

}  // namespace dm_sim
