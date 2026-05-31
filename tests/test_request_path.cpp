#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <vector>

#include "model/request.hpp"
#include "model/response.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"
#include "workloads/workload.hpp"

namespace {

using dm_sim::ComputeNodeConfig;
using dm_sim::ContentionPolicyConfig;
using dm_sim::EventRecord;
using dm_sim::EventType;
using dm_sim::LocalCacheConfig;
using dm_sim::LocalCachePolicyType;
using dm_sim::HotnessPolicyConfig;
using dm_sim::Request;
using dm_sim::RequestSpec;
using dm_sim::RequestStage;
using dm_sim::Response;
using dm_sim::ServedFromTier;
using dm_sim::SimulationConfig;
using dm_sim::Simulator;
using dm_sim::SimTime;
using dm_sim::CrossNodeOverlap;
using dm_sim::HotSetMode;
using dm_sim::HotnessHistoryMode;
using dm_sim::SyntheticWorkloadConfig;

HotnessPolicyConfig hotness_config(
    std::uint64_t min_admit_count = 2,
    HotnessHistoryMode history_mode = HotnessHistoryMode::Epoch,
    std::uint64_t history_window_epochs = 4) {
    HotnessPolicyConfig config;
    config.min_admit_count = min_admit_count;
    config.history_mode = history_mode;
    config.history_window_epochs = history_window_epochs;
    return config;
}

ContentionPolicyConfig contention_config(double min_admit_score = 1.0) {
    ContentionPolicyConfig config;
    config.weights.local_hotness_weight = 0.0;
    config.weights.remote_access_weight = 1.0;
    config.weights.distinct_requester_weight = 1.0;
    config.weights.queue_wait_weight = 1.0;
    config.weights.remote_service_time_weight = 1.0;
    config.weights.size_penalty_weight = 0.0;
    config.min_admit_score = min_admit_score;
    config.local_hotness_threshold = 2;
    return config;
}

std::size_t count_event_type(const std::vector<EventRecord>& event_log,
                             EventType type) {
    std::size_t count = 0;
    for (const EventRecord& event : event_log) {
        if (event.type == type) {
            ++count;
        }
    }

    return count;
}

SimulationConfig make_single_request_config(
    LocalCachePolicyType policy_type = LocalCachePolicyType::AlwaysRemote) {
    return SimulationConfig{
        {
            ComputeNodeConfig{
                1,
                {
                    RequestSpec{1001, 64},
                },
            },
        },
        99,
        5,
        20,
        16,
        LocalCacheConfig{
            64,
            2,
            policy_type,
        },
    };
}

void test_single_request_path_records_latency_under_always_remote() {
    Simulator simulator(make_single_request_config());
    simulator.run();

    assert(simulator.stats().completed_requests() == 1);
    assert(simulator.responses().size() == 1);
    assert(simulator.compute_node(1).outstanding_requests() == 0);

    const Response& response = simulator.responses().front();
    const SimTime expected_service_time = 20 + ((64 + 16 - 1) / 16);
    const SimTime expected_latency = 5 + expected_service_time + 5;

    assert(response.request_id == 1);
    assert(response.object_id == 1001);
    assert(response.served_from_tier == ServedFromTier::Memory);
    assert(response.total_latency == expected_latency);
    assert(response.completion_time == expected_latency);
    assert(response.bytes_transferred == 64);

    assert(simulator.stats().total_latency() == expected_latency);
    assert(simulator.stats().average_latency() == static_cast<double>(expected_latency));
    assert(simulator.stats().completed_requests(1) == 1);
    assert(simulator.stats().average_latency(1) == static_cast<double>(expected_latency));
    assert(simulator.stats().total_memory_wait() == 0);
    assert(simulator.stats().average_memory_wait() == 0.0);
    assert(simulator.stats().max_memory_wait() == 0);
    assert(simulator.stats().peak_memory_queue_depth() == 1);
    assert(simulator.stats().local_cache_hits() == 0);
    assert(simulator.stats().local_cache_misses() == 1);
    assert(simulator.stats().local_cache_misses(1) == 1);

    const Request& request = simulator.requests().at(response.request_id);
    assert(request.current_stage == RequestStage::Completed);
}

void test_local_lookup_always_forwards_to_memory_under_always_remote() {
    Simulator simulator(make_single_request_config());
    simulator.run();

    const std::vector<EventType> expected_types{
        EventType::GenerateRequest,
        EventType::LocalCacheLookup,
        EventType::ForwardToMemory,
        EventType::MemoryServiceStart,
        EventType::MemoryServiceComplete,
        EventType::ReturnResponse,
        EventType::RequestComplete,
    };

    std::vector<EventType> seen_types;
    for (const EventRecord& event : simulator.event_log()) {
        seen_types.push_back(event.type);
    }

    assert(seen_types == expected_types);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 0);
}

void test_repeated_reads_under_always_remote_continue_to_miss() {
    const SimulationConfig config{
        {
            ComputeNodeConfig{
                1,
                {
                    RequestSpec{2001, 8},
                    RequestSpec{2001, 8},
                },
            },
        },
        99,
        3,
        10,
        8,
        LocalCacheConfig{
            32,
            1,
            LocalCachePolicyType::AlwaysRemote,
        },
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 2);
    assert(simulator.responses().size() == 2);
    assert(simulator.responses()[0].served_from_tier == ServedFromTier::Memory);
    assert(simulator.responses()[1].served_from_tier == ServedFromTier::Memory);
    assert(simulator.stats().local_cache_hits() == 0);
    assert(simulator.stats().local_cache_misses() == 2);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 2);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 0);
}

void test_repeated_reads_under_lru_hit_locally_after_first_miss() {
    const SimulationConfig config{
        {
            ComputeNodeConfig{
                1,
                {
                    RequestSpec{4001, 8},
                    RequestSpec{4001, 8},
                },
            },
        },
        99,
        4,
        10,
        8,
        LocalCacheConfig{
            32,
            2,
            LocalCachePolicyType::Lru,
        },
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 2);
    assert(simulator.responses().size() == 2);
    assert(simulator.responses()[0].served_from_tier == ServedFromTier::Memory);
    assert(simulator.responses()[1].served_from_tier == ServedFromTier::LocalCache);

    const SimTime remote_latency = 4 + (10 + ((8 + 8 - 1) / 8)) + 4;
    assert(simulator.responses()[0].total_latency == remote_latency);
    assert(simulator.responses()[1].total_latency == 2);
    assert(simulator.stats().local_cache_hits() == 1);
    assert(simulator.stats().local_cache_misses() == 1);
    assert(simulator.stats().local_cache_hits(1) == 1);
    assert(simulator.stats().local_cache_misses(1) == 1);
    assert(std::abs(simulator.stats().local_cache_hit_rate(1) - 0.5) < 1e-9);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 1);
    assert(count_event_type(simulator.event_log(), EventType::MemoryServiceComplete) == 1);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 1);
    assert(simulator.stats().total_memory_wait() == 0);
}

void test_multi_node_private_lru_caches_still_work() {
    const SimulationConfig config{
        {
            ComputeNodeConfig{
                1,
                {
                    RequestSpec{5001, 8},
                    RequestSpec{5001, 8},
                },
            },
            ComputeNodeConfig{
                2,
                {
                    RequestSpec{5001, 8},
                    RequestSpec{5001, 8},
                },
            },
        },
        99,
        3,
        10,
        8,
        LocalCacheConfig{
            32,
            1,
            LocalCachePolicyType::Lru,
        },
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 4);
    assert(simulator.stats().completed_requests(1) == 2);
    assert(simulator.stats().completed_requests(2) == 2);
    assert(simulator.stats().local_cache_hits() == 2);
    assert(simulator.stats().local_cache_misses() == 2);
    assert(simulator.stats().local_cache_hits(1) == 1);
    assert(simulator.stats().local_cache_hits(2) == 1);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 2);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 2);
    assert(simulator.stats().peak_memory_queue_depth() == 2);
    assert(simulator.stats().total_memory_wait() > 0);
}

void test_invalid_config_rejects_duplicate_compute_ids() {
    try {
        const SimulationConfig config{
            {
                ComputeNodeConfig{1, {RequestSpec{1, 8}}},
                ComputeNodeConfig{1, {RequestSpec{2, 8}}},
            },
            99,
            1,
            10,
            8,
            LocalCacheConfig{
                16,
                1,
                LocalCachePolicyType::AlwaysRemote,
            },
        };

        Simulator simulator(config);
        (void)simulator;
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_synthetic_workload_runs_to_completion_with_epoch_metadata() {
    SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        0,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    SyntheticWorkloadConfig workload;
    workload.seed = 7;
    workload.compute_node_ids = {1, 2};
    workload.object_count = 16;
    workload.object_size_bytes = 8;
    workload.requests_per_node_per_epoch = 2;
    workload.epoch_count = 2;
    workload.hot_set_size = 2;
    workload.hot_access_probability = 1.0;
    workload.hot_set_mode = HotSetMode::EpochShift;
    workload.cross_node_overlap = CrossNodeOverlap::High;
    config.synthetic_workload = workload;

    Simulator simulator(config);
    simulator.run();

    assert(simulator.generated_workload().has_value());
    assert(simulator.config().compute_nodes.size() == 2);
    assert(simulator.stats().completed_requests() == 8);
    assert(simulator.stats().completed_requests(1) == 4);
    assert(simulator.stats().completed_requests(2) == 4);
    assert(simulator.stats().local_cache_hits() == 0);
    assert(simulator.stats().local_cache_misses() == 8);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 8);

    std::size_t epoch_zero_requests = 0;
    std::size_t epoch_one_requests = 0;
    for (const auto& entry : simulator.requests()) {
        const Request& request = entry.second;
        assert(request.current_stage == RequestStage::Completed);
        assert(request.source_node_id == 1 || request.source_node_id == 2);
        assert(request.epoch_id == 0 || request.epoch_id == 1);
        if (request.epoch_id == 0) {
            ++epoch_zero_requests;
        } else {
            ++epoch_one_requests;
        }
    }

    assert(epoch_zero_requests == 4);
    assert(epoch_one_requests == 4);
}

void test_synthetic_high_overlap_lru_repeated_reads_hit_locally() {
    SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        64,
        1,
        LocalCachePolicyType::Lru,
    };

    SyntheticWorkloadConfig workload;
    workload.seed = 11;
    workload.compute_node_ids = {1, 2};
    workload.object_count = 8;
    workload.object_size_bytes = 8;
    workload.requests_per_node_per_epoch = 3;
    workload.epoch_count = 1;
    workload.hot_set_size = 1;
    workload.hot_access_probability = 1.0;
    workload.hot_set_mode = HotSetMode::Static;
    workload.cross_node_overlap = CrossNodeOverlap::High;
    config.synthetic_workload = workload;

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 6);
    assert(simulator.stats().local_cache_misses() == 2);
    assert(simulator.stats().local_cache_hits() == 4);
    assert(simulator.stats().local_cache_misses(1) == 1);
    assert(simulator.stats().local_cache_misses(2) == 1);
    assert(simulator.stats().local_cache_hits(1) == 2);
    assert(simulator.stats().local_cache_hits(2) == 2);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 2);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 4);
}

void test_synthetic_always_remote_sends_all_reads_to_memory() {
    SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        64,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    SyntheticWorkloadConfig workload;
    workload.seed = 11;
    workload.compute_node_ids = {1, 2};
    workload.object_count = 8;
    workload.object_size_bytes = 8;
    workload.requests_per_node_per_epoch = 3;
    workload.epoch_count = 1;
    workload.hot_set_size = 1;
    workload.hot_access_probability = 1.0;
    workload.hot_set_mode = HotSetMode::Static;
    workload.cross_node_overlap = CrossNodeOverlap::High;
    config.synthetic_workload = workload;

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 6);
    assert(simulator.stats().local_cache_hits() == 0);
    assert(simulator.stats().local_cache_misses() == 6);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 6);
    for (const Response& response : simulator.responses()) {
        assert(response.served_from_tier == ServedFromTier::Memory);
    }
}

void test_repeated_reads_under_hotness_only_hit_after_threshold() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{6001, 8, 0},
                RequestSpec{6001, 8, 0},
                RequestSpec{6001, 8, 0},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        8,
        1,
        LocalCachePolicyType::HotnessOnly,
        hotness_config(2, HotnessHistoryMode::Epoch),
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 3);
    assert(simulator.stats().local_cache_misses() == 2);
    assert(simulator.stats().local_cache_hits() == 1);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 2);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 1);
    assert(simulator.responses()[0].served_from_tier == ServedFromTier::Memory);
    assert(simulator.responses()[1].served_from_tier == ServedFromTier::Memory);
    assert(simulator.responses()[2].served_from_tier == ServedFromTier::LocalCache);
}

void test_hotness_only_resets_across_epoch_shift() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{6101, 8, 0},
                RequestSpec{6101, 8, 0},
                RequestSpec{6201, 8, 1},
                RequestSpec{6201, 8, 1},
                RequestSpec{6201, 8, 1},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        8,
        1,
        LocalCachePolicyType::HotnessOnly,
        hotness_config(2, HotnessHistoryMode::Epoch),
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 5);
    assert(simulator.stats().local_cache_misses() == 4);
    assert(simulator.stats().local_cache_hits() == 1);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 4);
    assert(simulator.compute_node(1).local_cache().contains(6201));
    assert(!simulator.compute_node(1).local_cache().contains(6101));
    assert(simulator.responses().back().served_from_tier == ServedFromTier::LocalCache);
}

void test_global_hottest_replication_hits_on_first_request() {
    SimulationConfig config;
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        8,
        1,
        LocalCachePolicyType::GlobalHottestReplication,
    };

    SyntheticWorkloadConfig workload;
    workload.seed = 123;
    workload.compute_node_ids = {1, 2};
    workload.object_count = 8;
    workload.object_size_bytes = 8;
    workload.requests_per_node_per_epoch = 2;
    workload.epoch_count = 1;
    workload.hot_set_size = 1;
    workload.hot_access_probability = 1.0;
    workload.hot_set_mode = HotSetMode::Static;
    workload.cross_node_overlap = CrossNodeOverlap::High;
    config.synthetic_workload = workload;

    Simulator simulator(config);
    simulator.run();

    assert(simulator.generated_workload().has_value());
    const auto& epoch = simulator.generated_workload()->epochs.front();
    const dm_sim::ObjectId hot_object =
        epoch.hot_sets_by_node.at(1).front();

    assert(simulator.stats().completed_requests() == 4);
    assert(simulator.stats().local_cache_hits() == 4);
    assert(simulator.stats().local_cache_misses() == 0);
    assert(count_event_type(simulator.event_log(), EventType::ForwardToMemory) == 0);
    assert(count_event_type(simulator.event_log(), EventType::LocalCacheHitComplete) == 4);

    for (const Response& response : simulator.responses()) {
        assert(response.served_from_tier == ServedFromTier::LocalCache);
    }

    assert(simulator.compute_node(1).local_cache().contains(hot_object));
    assert(simulator.compute_node(2).local_cache().contains(hot_object));
}

void test_global_hottest_replication_requires_synthetic_workload() {
    try {
        SimulationConfig config;
        config.compute_nodes = {
            ComputeNodeConfig{1, {RequestSpec{1, 8, 0}}},
        };
        config.memory_node_id = 99;
        config.local_cache = LocalCacheConfig{
            8,
            1,
            LocalCachePolicyType::GlobalHottestReplication,
        };

        Simulator simulator(config);
        (void)simulator;
        assert(false);
    } catch (const std::invalid_argument&) {
    }
}

void test_contention_tracks_distinct_requester_overlap() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{7001, 8, 0},
                RequestSpec{7002, 8, 0},
            },
        },
        ComputeNodeConfig{
            2,
            {
                RequestSpec{7001, 8, 0},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        0,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    Simulator simulator(config);
    simulator.run();

    const std::optional<dm_sim::ObjectContentionStats> shared_object =
        simulator.stats().object_contention(0, 7001);
    const std::optional<dm_sim::ObjectContentionStats> private_object =
        simulator.stats().object_contention(0, 7002);

    assert(shared_object.has_value());
    assert(private_object.has_value());
    assert(shared_object->remote_accesses == 2);
    assert(shared_object->distinct_requesters == 2);
    assert(private_object->remote_accesses == 1);
    assert(private_object->distinct_requesters == 1);
    assert(shared_object->total_queue_wait > 0);
}

void test_contention_tracks_service_time_by_object_size() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{7101, 8, 0},
                RequestSpec{7102, 24, 0},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        0,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    Simulator simulator(config);
    simulator.run();

    const std::optional<dm_sim::ObjectContentionStats> small_object =
        simulator.stats().object_contention(0, 7101);
    const std::optional<dm_sim::ObjectContentionStats> large_object =
        simulator.stats().object_contention(0, 7102);

    assert(small_object.has_value());
    assert(large_object.has_value());
    assert(small_object->total_remote_service_time == 6);
    assert(large_object->total_remote_service_time == 8);
    assert(large_object->total_remote_service_time >
           small_object->total_remote_service_time);
    assert(small_object->bytes_served == 8);
    assert(large_object->bytes_served == 24);
}

void test_contention_separates_epoch_shifted_requests() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{7201, 8, 0},
                RequestSpec{7201, 8, 1},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        0,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    Simulator simulator(config);
    simulator.run();

    const std::optional<dm_sim::ObjectContentionStats> epoch_zero =
        simulator.stats().object_contention(0, 7201);
    const std::optional<dm_sim::ObjectContentionStats> epoch_one =
        simulator.stats().object_contention(1, 7201);

    assert(epoch_zero.has_value());
    assert(epoch_one.has_value());
    assert(epoch_zero->remote_accesses == 1);
    assert(epoch_one->remote_accesses == 1);
    assert(simulator.stats().contention_by_epoch(0).size() == 1);
    assert(simulator.stats().contention_by_epoch(1).size() == 1);
}

void test_global_epoch_barrier_waits_for_prior_epoch_completion() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{8001, 8, 0},
                RequestSpec{8002, 8, 1},
            },
        },
        ComputeNodeConfig{
            2,
            {
                RequestSpec{8003, 8, 0},
                RequestSpec{8004, 8, 1},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        0,
        1,
        LocalCachePolicyType::AlwaysRemote,
    };

    Simulator simulator(config);
    simulator.run();

    SimTime latest_epoch_zero_complete = 0;
    std::optional<SimTime> first_epoch_one_lookup;
    for (const EventRecord& event : simulator.event_log()) {
        if (event.request_id == dm_sim::kInvalidRequestId) {
            continue;
        }

        const Request& request = simulator.requests().at(event.request_id);
        if (request.epoch_id == 0 && event.type == EventType::RequestComplete) {
            latest_epoch_zero_complete =
                std::max(latest_epoch_zero_complete, event.time);
        }

        if (request.epoch_id == 1 && event.type == EventType::LocalCacheLookup) {
            if (!first_epoch_one_lookup.has_value() ||
                event.time < *first_epoch_one_lookup) {
                first_epoch_one_lookup = event.time;
            }
        }
    }

    assert(first_epoch_one_lookup.has_value());
    assert(*first_epoch_one_lookup >= latest_epoch_zero_complete);
}

void test_contention_aware_uses_previous_epoch_and_hits_after_admission() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{8101, 8, 0},
                RequestSpec{8101, 8, 1},
                RequestSpec{8101, 8, 1},
            },
        },
        ComputeNodeConfig{
            2,
            {
                RequestSpec{8101, 8, 0},
                RequestSpec{8101, 8, 1},
                RequestSpec{8101, 8, 1},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        8,
        1,
        LocalCachePolicyType::ContentionAware,
        HotnessPolicyConfig{},
        contention_config(1.0),
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 6);
    assert(simulator.stats().local_cache_hits() == 2);
    assert(simulator.stats().local_cache_misses() == 4);

    const std::optional<dm_sim::ObjectContentionStats> epoch_zero =
        simulator.stats().object_contention(0, 8101);
    const std::optional<dm_sim::ObjectContentionStats> epoch_one =
        simulator.stats().object_contention(1, 8101);
    assert(epoch_zero.has_value());
    assert(epoch_one.has_value());
    assert(epoch_zero->remote_accesses == 2);
    assert(epoch_one->remote_accesses == 2);

    std::size_t admitted_epoch_one = 0;
    for (const dm_sim::PolicyDecisionRecord& decision :
         simulator.policy_diagnostics()) {
        if (decision.epoch_id == 1 && decision.admitted) {
            ++admitted_epoch_one;
            assert(decision.score.remote_accesses > 0.0);
        }
    }
    assert(admitted_epoch_one == 2);
}

void test_contention_aware_does_not_use_current_epoch_telemetry() {
    SimulationConfig config;
    config.compute_nodes = {
        ComputeNodeConfig{
            1,
            {
                RequestSpec{8201, 8, 0},
                RequestSpec{8202, 8, 1},
                RequestSpec{8202, 8, 1},
            },
        },
        ComputeNodeConfig{
            2,
            {
                RequestSpec{8201, 8, 0},
                RequestSpec{8202, 8, 1},
                RequestSpec{8202, 8, 1},
            },
        },
    };
    config.memory_node_id = 99;
    config.one_way_link_latency = 2;
    config.memory_base_latency = 5;
    config.memory_bandwidth_bytes_per_time = 8;
    config.local_cache = LocalCacheConfig{
        8,
        1,
        LocalCachePolicyType::ContentionAware,
        HotnessPolicyConfig{},
        contention_config(1.0),
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().local_cache_hits() == 0);
    assert(simulator.stats().local_cache_misses() == 6);
    const std::optional<dm_sim::ObjectContentionStats> new_epoch_one_object =
        simulator.stats().object_contention(1, 8202);
    assert(new_epoch_one_object.has_value());
    assert(new_epoch_one_object->remote_accesses == 4);
}

}  // namespace

int main() {
    test_single_request_path_records_latency_under_always_remote();
    test_local_lookup_always_forwards_to_memory_under_always_remote();
    test_repeated_reads_under_always_remote_continue_to_miss();
    test_repeated_reads_under_lru_hit_locally_after_first_miss();
    test_multi_node_private_lru_caches_still_work();
    test_invalid_config_rejects_duplicate_compute_ids();
    test_synthetic_workload_runs_to_completion_with_epoch_metadata();
    test_synthetic_high_overlap_lru_repeated_reads_hit_locally();
    test_synthetic_always_remote_sends_all_reads_to_memory();
    test_repeated_reads_under_hotness_only_hit_after_threshold();
    test_hotness_only_resets_across_epoch_shift();
    test_global_hottest_replication_hits_on_first_request();
    test_global_hottest_replication_requires_synthetic_workload();
    test_contention_tracks_distinct_requester_overlap();
    test_contention_tracks_service_time_by_object_size();
    test_contention_separates_epoch_shifted_requests();
    test_global_epoch_barrier_waits_for_prior_epoch_completion();
    test_contention_aware_uses_previous_epoch_and_hits_after_admission();
    test_contention_aware_does_not_use_current_epoch_telemetry();
    return 0;
}
