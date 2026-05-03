#include <cassert>
#include <cmath>

#include "experiments/runner.hpp"
#include "metrics/histogram.hpp"
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

}  // namespace

int main() {
    test_percentiles_handle_common_shapes();
    test_metrics_summary_includes_per_node_values();
    return 0;
}
