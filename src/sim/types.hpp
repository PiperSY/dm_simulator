#pragma once

#include <cstdint>

namespace dm_sim {

// Common type definitions used across the simulation components. Aliases for code readability and maintainability.
using SimTime = std::uint64_t;
using NodeId = std::uint32_t;
using ObjectId = std::uint64_t;
using RequestId = std::uint64_t;
using EpochId = std::uint64_t;
using MemoryChannelId = std::uint32_t;

// A constant representing an invalid request ID, used for initialization and error handling.
constexpr RequestId kInvalidRequestId = 0;

// Static object-to-channel mapping keeps channel-local contention reproducible
// without modeling hardware address hashing or dynamic memory placement.
[[nodiscard]] constexpr MemoryChannelId memory_channel_for_object(
    ObjectId object_id,
    std::uint64_t channel_count) noexcept {
    return object_id == 0
               ? 0
               : static_cast<MemoryChannelId>((object_id - 1) % channel_count);
}

}  // namespace dm_sim
