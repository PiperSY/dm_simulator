#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

#include "experiments/runner.hpp"
#include "metrics/histogram.hpp"
#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"

namespace {

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 1e-9;
}

void test_percentiles_handle_common_shapes() {
    assert(near(dm_sim::median_latency({10, 20, 30}), 20.0));
    assert(near(dm_sim::median_latency({10, 20, 30, 40}), 25.0));
    assert(near(dm_sim::p95_latency({1, 2, 3, 4, 5}), 4.8));
    assert(near(dm_sim::p99_latency({1, 2, 3, 4, 5}), 4.96));
    assert(near(dm_sim::median_latency({42}), 42.0));
    assert(near(dm_sim::p95_latency({}), 0.0));
}

void test_metrics_summary_includes_per_node_values() {
    dm_sim::SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = dm_sim::LocalCacheConfig{
        64,
        1,
        dm_sim::LocalCachePolicyType::Lru,
    };
    config.compute_nodes = {
        dm_sim::ComputeNodeConfig{
            1,
            {
                dm_sim::RequestSpec{101, 8, 0},
                dm_sim::RequestSpec{101, 8, 0},
            },
        },
        dm_sim::ComputeNodeConfig{
            2,
            {
                dm_sim::RequestSpec{202, 8, 0},
                dm_sim::RequestSpec{202, 8, 0},
            },
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    const dm_sim::MetricsSummary summary =
        dm_sim::summarize_metrics("metrics_test", simulator);

    assert(summary.experiment_name == "metrics_test");
    assert(summary.completed_requests == 4);
    assert(summary.mean_latency > 0.0);
    assert(summary.median_latency > 0.0);
    assert(summary.p95_latency >= summary.median_latency);
    assert(summary.local_cache_hits == 2);
    assert(summary.local_cache_misses == 2);
    assert(near(summary.local_cache_hit_rate, 0.5));
    assert(summary.memory_peak_queue_depth == 2);
    assert(summary.per_node.size() == 2);

    for (const dm_sim::PerNodeMetricsSummary& node : summary.per_node) {
        assert(node.completed_requests == 2);
        assert(node.mean_latency > 0.0);
        assert(node.p99_latency > 0.0);
        assert(node.local_cache_hits == 1);
        assert(node.local_cache_misses == 1);
        assert(near(node.local_cache_hit_rate, 0.5));
    }
}

dm_sim::Request make_request(dm_sim::RequestId request_id,
                             dm_sim::NodeId source_node_id,
                             dm_sim::ObjectId object_id,
                             dm_sim::EpochId epoch_id,
                             std::uint64_t size_bytes) {
    dm_sim::Request request;
    request.request_id = request_id;
    request.source_node_id = source_node_id;
    request.object_id = object_id;
    request.epoch_id = epoch_id;
    request.size_bytes = size_bytes;
    return request;
}

void test_contention_aggregation_records_remote_signals() {
    dm_sim::Stats stats;

    const dm_sim::Request first = make_request(1, 1, 9001, 0, 16);
    stats.record_remote_access(first, 1);
    stats.record_object_queue_wait(first, 0);
    stats.record_object_service(first, 7);

    const dm_sim::Request second = make_request(2, 2, 9001, 0, 16);
    stats.record_remote_access(second, 2);
    stats.record_object_queue_wait(second, 5);
    stats.record_object_service(second, 7);

    const std::optional<dm_sim::ObjectContentionStats> object_stats =
        stats.object_contention(0, 9001);
    assert(object_stats.has_value());
    assert(object_stats->remote_accesses == 2);
    assert(object_stats->distinct_requesters == 2);
    assert(object_stats->bytes_served == 32);
    assert(object_stats->total_remote_service_time == 14);
    assert(object_stats->total_queue_wait == 5);
    assert(object_stats->max_queue_wait == 5);
    assert(object_stats->queue_wait_samples == 2);
    assert(object_stats->max_observed_queue_depth == 2);
    assert(near(object_stats->average_queue_wait, 2.5));

    const dm_sim::Request next_epoch = make_request(3, 1, 9001, 1, 16);
    stats.record_remote_access(next_epoch, 1);
    stats.record_object_queue_wait(next_epoch, 0);
    stats.record_object_service(next_epoch, 7);

    assert(stats.contention_by_epoch(0).size() == 1);
    assert(stats.contention_by_epoch(1).size() == 1);
    assert(stats.previous_epoch_contention(0).empty());
    assert(stats.previous_epoch_contention(1).size() == 1);
    assert(stats.previous_epoch_object_contention(1, 9001).has_value());
    assert(stats.all_contention_stats().size() == 2);

    const std::vector<dm_sim::ObjectContentionStats> top_by_wait =
        stats.top_contention_objects(dm_sim::ContentionSortKey::TotalQueueWait, 1);
    assert(top_by_wait.size() == 1);
    assert(top_by_wait.front().epoch_id == 0);
    assert(top_by_wait.front().object_id == 9001);
}

void test_cache_hits_do_not_increment_remote_contention() {
    dm_sim::SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = dm_sim::LocalCacheConfig{
        64,
        1,
        dm_sim::LocalCachePolicyType::Lru,
    };
    config.compute_nodes = {
        dm_sim::ComputeNodeConfig{
            1,
            {
                dm_sim::RequestSpec{9101, 8, 0},
                dm_sim::RequestSpec{9101, 8, 0},
                dm_sim::RequestSpec{9101, 8, 0},
            },
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().local_cache_misses() == 1);
    assert(simulator.stats().local_cache_hits() == 2);

    const std::optional<dm_sim::ObjectContentionStats> object_stats =
        simulator.stats().object_contention(0, 9101);
    assert(object_stats.has_value());
    assert(object_stats->remote_accesses == 1);
    assert(object_stats->bytes_served == 8);
    assert(simulator.stats().all_contention_stats().size() == 1);
}

void test_viability_metrics_track_reuse_relief_and_overlap() {
    dm_sim::SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = dm_sim::LocalCacheConfig{
        8,
        1,
        dm_sim::LocalCachePolicyType::Lru,
    };
    config.compute_nodes = {
        dm_sim::ComputeNodeConfig{
            1,
            {
                dm_sim::RequestSpec{9201, 8, 0},
                dm_sim::RequestSpec{9201, 8, 0},
                dm_sim::RequestSpec{9201, 8, 1},
                dm_sim::RequestSpec{9201, 8, 1},
            },
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    const dm_sim::MetricsSummary summary =
        dm_sim::summarize_metrics("viability_reuse", simulator);
    const dm_sim::ViabilityMetricsSummary& viability = summary.viability;

    assert(viability.successful_placements == 1);
    assert(viability.rejected_admissions == 0);
    assert(viability.total_future_hits == 3);
    assert(near(viability.admission_yield, 3.0));
    assert(near(viability.reuse_after_admit_rate, 1.0));
    assert(near(viability.average_top_object_overlap, 1.0));
    assert(near(viability.stale_telemetry_rate, 0.0));
    assert(viability.estimated_avoided_remote_accesses == 3);
    assert(viability.estimated_avoided_remote_service_time > 0.0);
    assert(near(viability.jain_inverse_latency_fairness, 1.0));
}

void test_viability_metrics_detect_eviction_regret() {
    dm_sim::SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = dm_sim::LocalCacheConfig{
        8,
        1,
        dm_sim::LocalCachePolicyType::Lru,
    };
    config.compute_nodes = {
        dm_sim::ComputeNodeConfig{
            1,
            {
                dm_sim::RequestSpec{9301, 8, 0},
                dm_sim::RequestSpec{9302, 8, 0},
                dm_sim::RequestSpec{9301, 8, 0},
            },
        },
    };

    dm_sim::Simulator simulator(config);
    simulator.run();

    const dm_sim::MetricsSummary summary =
        dm_sim::summarize_metrics("viability_regret", simulator);

    assert(summary.viability.eviction_regret_count == 1);
    assert(summary.viability.remote_eviction_regret_count == 1);
}

}  // namespace

int main() {
    test_percentiles_handle_common_shapes();
    test_metrics_summary_includes_per_node_values();
    test_contention_aggregation_records_remote_signals();
    test_cache_hits_do_not_increment_remote_contention();
    test_viability_metrics_track_reuse_relief_and_overlap();
    test_viability_metrics_detect_eviction_regret();
    return 0;
}
