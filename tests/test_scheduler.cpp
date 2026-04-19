#include <cassert>
#include <cstdint>
#include <optional>
#include <vector>

#include "sim/scheduler.hpp"

namespace {

using dm_sim::Event;
using dm_sim::EventType;
using dm_sim::Scheduler;
using dm_sim::SimTime;

void test_pop_ordering() {
    Scheduler scheduler;

    assert(scheduler.schedule(Event(20, EventType::RequestComplete, 3)));
    assert(scheduler.schedule(Event(10, EventType::GenerateRequest, 1)));
    assert(scheduler.schedule(Event(20, EventType::ReturnResponse, 2)));

    const std::optional<Event> first = scheduler.pop_next();

    assert(first.has_value());
    assert(first->time == 10);
    assert(first->target_id == 1);
    assert(scheduler.now() == 10);

    const std::optional<Event> second = scheduler.pop_next();
    assert(second.has_value());
    assert(second->time == 20);
    assert(second->target_id == 3);
    assert(scheduler.now() == 20);

    const std::optional<Event> third = scheduler.pop_next();
    assert(third.has_value());
    assert(third->time == 20);
    assert(third->target_id == 2);
    assert(second->sequence() < third->sequence());
    assert(scheduler.empty());
}

void test_run_until_empty_determinism() {
    Scheduler scheduler;

    assert(scheduler.schedule(Event(5, EventType::GenerateRequest, 1)));
    assert(scheduler.schedule(Event(5, EventType::LocalCacheLookup, 2)));
    assert(scheduler.schedule(Event(10, EventType::ForwardToMemory, 3)));

    std::vector<std::uint32_t> seen_targets;
    std::vector<SimTime> seen_times;

    scheduler.run_until_empty([&](const Event& event, Scheduler& active_scheduler) {
        assert(active_scheduler.now() == event.time);
        seen_targets.push_back(event.target_id);
        seen_times.push_back(event.time);

        if (event.target_id == 1) {
            assert(active_scheduler.schedule(
                Event(5, EventType::MemoryQueueEnqueue, 4)));
            assert(active_scheduler.schedule(
                Event(12, EventType::RequestComplete, 5)));
        }
    });

    const std::vector<std::uint32_t> expected_targets{1, 2, 4, 3, 5};
    const std::vector<SimTime> expected_times{5, 5, 5, 10, 12};

    assert(seen_targets == expected_targets);
    assert(seen_times == expected_times);
    assert(scheduler.now() == 12);
}

void test_rejects_past_events() {
    Scheduler scheduler;

    assert(scheduler.schedule(Event(7, EventType::GenerateRequest, 1)));

    const std::optional<Event> event = scheduler.pop_next();
    assert(event.has_value());
    assert(scheduler.now() == 7);

    assert(!scheduler.schedule(Event(6, EventType::ReturnResponse, 2)));
    assert(scheduler.empty());
}

}  // namespace

int main() {
    test_pop_ordering();
    test_run_until_empty_determinism();
    test_rejects_past_events();
    return 0;
}
