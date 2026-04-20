#pragma once

#include <cstdint>

namespace dm_sim {

using SimTime = std::uint64_t;
using NodeId = std::uint32_t;
using ObjectId = std::uint64_t;
using RequestId = std::uint64_t;
using EpochId = std::uint64_t;

constexpr RequestId kInvalidRequestId = 0;

}  // namespace dm_sim
