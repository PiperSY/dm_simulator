#include <cassert>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <vector>

#include "model/request.hpp"
#include "model/response.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"

namespace {

using dm_sim::ComputeNodeConfig;
using dm_sim::EventRecord;
using dm_sim::EventType;
using dm_sim::LocalCacheConfig;
using dm_sim::LocalCachePolicyType;
using dm_sim::Request;
using dm_sim::RequestSpec;
using dm_sim::RequestStage;
using dm_sim::Response;
using dm_sim::ServedFromTier;
using dm_sim::SimulationConfig;
using dm_sim::Simulator;
using dm_sim::SimTime;

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

}  // namespace

int main() {
    test_single_request_path_records_latency_under_always_remote();
    test_local_lookup_always_forwards_to_memory_under_always_remote();
    test_repeated_reads_under_always_remote_continue_to_miss();
    test_repeated_reads_under_lru_hit_locally_after_first_miss();
    test_multi_node_private_lru_caches_still_work();
    test_invalid_config_rejects_duplicate_compute_ids();
    return 0;
}
