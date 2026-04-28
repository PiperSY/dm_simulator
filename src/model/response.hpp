#pragma once

#include <cstdint>

#include "sim/types.hpp"

namespace dm_sim {

enum class ServedFromTier {
    LocalCache,
    Memory,
};

struct Response {
    RequestId request_id = kInvalidRequestId;
    ObjectId object_id = 0;
    ServedFromTier served_from_tier = ServedFromTier::Memory;
    SimTime completion_time = 0;
    SimTime total_latency = 0;
    std::uint64_t bytes_transferred = 0;
};

}  // namespace dm_sim
