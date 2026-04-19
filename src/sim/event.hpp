#pragma once

#include <cstdint>

namespace dm_sim {

using SimTime = std::uint64_t;

class Scheduler;

enum class EventType {
    GenerateRequest,
    LocalCacheLookup,
    LocalCacheHitComplete,
    ForwardToSharedCache,
    SharedCacheLookup,
    SharedCacheHitComplete,
    ForwardToMemory,
    MemoryQueueEnqueue,
    MemoryServiceStart,
    MemoryServiceComplete,
    ReturnResponse,
    RequestComplete,
    EpochAdvance,
};

struct Event {
    Event() = default;

    Event(SimTime event_time, EventType event_type, std::uint32_t event_target_id)
        : time(event_time), type(event_type), target_id(event_target_id) {}

    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    SimTime time = 0;
    EventType type = EventType::GenerateRequest;
    std::uint32_t target_id = 0;

private:
    friend class Scheduler;

    std::uint64_t sequence_ = 0;
};

}  // namespace dm_sim
