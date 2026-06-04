#pragma once

#include <cstdint>

#include "model/epoch.hpp"
#include "sim/types.hpp"

namespace dm_sim {

enum class OperationType {
    Read,
};

enum class RequestStage {
    Generated,
    LocalLookup,
    ForwardedToMemory,
    WaitingForResponse,
    Completed,
};

struct RequestSpec {
    ObjectId object_id = 0;
    std::uint64_t size_bytes = 0;
    EpochId epoch_id = 0;
    // In scheduled-bursty synthetic workloads this is the planned arrival time
    // relative to the epoch release. Completion-driven workloads leave it zero.
    SimTime scheduled_issue_offset = 0;
};

/*********************************** 
 * Request struct represents a memory request in the simulation, containing details such as the request ID, source node ID, 
 * object ID, operation type, issue time, size in bytes, epoch ID, current stage of processing, and timing information for 
 * memory enqueueing. The Request struct is used to track the lifecycle of a request as it moves through the system.
 ***********************************/
struct Request {
    RequestId request_id = kInvalidRequestId;
    NodeId source_node_id = 0;
    ObjectId object_id = 0;
    OperationType operation_type = OperationType::Read;
    SimTime issue_time = 0;
    std::uint64_t size_bytes = 0;
    EpochId epoch_id = 0;
    RequestStage current_stage = RequestStage::Generated;
    SimTime memory_enqueue_time = 0;
};

}  // namespace dm_sim
