#pragma once

#include <cstdint>

 #include "sim/types.hpp"

namespace dm_sim {

class Scheduler;

// Types of events that can occur in the simulation.
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

// Schedulable event with event identifiers.
struct Event {
    Event() = default;

    // Constructor for creating an event with specified parameters.
    Event(SimTime event_time,
          EventType event_type,
          NodeId event_target_id,
          RequestId event_request_id = kInvalidRequestId)
        : time(event_time),
          type(event_type),
          target_id(event_target_id),
          request_id(event_request_id) {}

    // Returns a unique sequence number for the event, which can be used for tie-breaking in the scheduler.
    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_;
    }

    SimTime time = 0;
    EventType type = EventType::GenerateRequest;
    NodeId target_id = 0;
    RequestId request_id = kInvalidRequestId;

private:
    // Friend class scheduler allows access to private sequence_ member for event ordering.
    friend class Scheduler;

    std::uint64_t sequence_ = 0;
};

}  // namespace dm_sim
