#include <cassert>
#include <vector>

#include "model/request.hpp"
#include "model/response.hpp"
#include "sim/config.hpp"
#include "sim/simulator.hpp"

namespace {

using dm_sim::EventRecord;
using dm_sim::EventType;
using dm_sim::Request;
using dm_sim::RequestId;
using dm_sim::RequestSpec;
using dm_sim::RequestStage;
using dm_sim::Response;
using dm_sim::ServedFromTier;
using dm_sim::SimulationConfig;
using dm_sim::Simulator;
using dm_sim::SimTime;

SimulationConfig make_single_request_config() {
    return SimulationConfig{
        1,
        2,
        5,
        20,
        16,
        {
            RequestSpec{1001, 64},
        },
    };
}

void test_single_request_path_records_latency() {
    Simulator simulator(make_single_request_config());
    simulator.run();

    assert(simulator.stats().completed_requests() == 1);
    assert(simulator.responses().size() == 1);
    assert(simulator.compute_node().outstanding_requests() == 0);

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

    const Request& request = simulator.requests().at(response.request_id);
    assert(request.current_stage == RequestStage::Completed);
}

void test_local_lookup_always_forwards_to_memory() {
    Simulator simulator(make_single_request_config());
    simulator.run();

    std::vector<EventType> types;
    for (const EventRecord& event : simulator.event_log()) {
        types.push_back(event.type);
    }

    const std::vector<EventType> expected_types{
        EventType::GenerateRequest,
        EventType::LocalCacheLookup,
        EventType::ForwardToMemory,
        EventType::ReturnResponse,
        EventType::RequestComplete,
    };

    assert(types == expected_types);
}

void test_multiple_requests_complete_sequentially() {
    const SimulationConfig config{
        1,
        2,
        3,
        10,
        8,
        {
            RequestSpec{2001, 8},
            RequestSpec{2002, 16},
        },
    };

    Simulator simulator(config);
    simulator.run();

    assert(simulator.stats().completed_requests() == 2);
    assert(simulator.responses().size() == 2);
    assert(simulator.compute_node().issued_requests() == 2);

    const SimTime first_latency = 3 + (10 + ((8 + 8 - 1) / 8)) + 3;
    const SimTime second_latency = 3 + (10 + ((16 + 8 - 1) / 8)) + 3;

    assert(simulator.responses()[0].total_latency == first_latency);
    assert(simulator.responses()[1].total_latency == second_latency);
    assert(simulator.responses()[0].completion_time < simulator.responses()[1].completion_time);

    const std::vector<EventType> expected_types{
        EventType::GenerateRequest,
        EventType::LocalCacheLookup,
        EventType::ForwardToMemory,
        EventType::ReturnResponse,
        EventType::RequestComplete,
        EventType::GenerateRequest,
        EventType::LocalCacheLookup,
        EventType::ForwardToMemory,
        EventType::ReturnResponse,
        EventType::RequestComplete,
    };

    std::vector<EventType> seen_types;
    for (const EventRecord& event : simulator.event_log()) {
        seen_types.push_back(event.type);
    }

    assert(seen_types == expected_types);
}

}  // namespace

int main() {
    test_single_request_path_records_latency();
    test_local_lookup_always_forwards_to_memory();
    test_multiple_requests_complete_sequentially();
    return 0;
}
