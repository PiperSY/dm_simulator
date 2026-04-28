#pragma once

#include <cstdint>

#include "sim/types.hpp"

namespace dm_sim {

struct CacheEntry {
    ObjectId object_id = 0;
    std::uint64_t size_bytes = 0;
    SimTime insert_time = 0;
    SimTime last_access_time = 0;
};

}  // namespace dm_sim
