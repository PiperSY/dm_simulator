#pragma once

#include <cstdint>

namespace dm_sim {

// Common type definitions used across the simulation components. Aliases for code readability and maintainability.
using SimTime = std::uint64_t;
using NodeId = std::uint32_t;
using ObjectId = std::uint64_t;
using RequestId = std::uint64_t;
using EpochId = std::uint64_t;

// A constant representing an invalid request ID, used for initialization and error handling.
constexpr RequestId kInvalidRequestId = 0;

}  // namespace dm_sim
