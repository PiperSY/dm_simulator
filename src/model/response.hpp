#pragma once

#include <cstdint>

#include "sim/types.hpp"

namespace dm_sim {

enum class ServedFromTier {
    LocalCache,
    Memory,
};

/*********************************** 
 * Response struct represents the response to a memory request in the simulation, containing details such as the associated request ID, 
 * object ID, the tier from which the request was served (local cache or memory), completion time, total latency, and bytes transferred. 
 * The Response struct is used to track the outcome of a request and its performance characteristics.
 ***********************************/
struct Response {
    RequestId request_id = kInvalidRequestId;
    ObjectId object_id = 0;
    ServedFromTier served_from_tier = ServedFromTier::Memory;
    SimTime completion_time = 0;
    SimTime total_latency = 0;
    std::uint64_t bytes_transferred = 0;
};

}  // namespace dm_sim
