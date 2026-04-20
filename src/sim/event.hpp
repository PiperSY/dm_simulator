#pragma once

#include <cstdint>

 #include "sim/types.hpp"

namespace dm_sim {

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

    Event(SimTime event_time,
          EventType event_type,
          NodeId event_target_id,
          RequestId event_request_id = kInvalidRequestId)
        : time(event_time),
          type(event_type),
          target_id(event_target_id),
          request_id(event_request_id) {}

    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    SimTime time = 0;
    EventType type = EventType::GenerateRequest;
    NodeId target_id = 0;
    RequestId request_id = kInvalidRequestId;

private:
    friend class Scheduler;

    std::uint64_t sequence_ = 0;
};

}  // namespace dm_sim
