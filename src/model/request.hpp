#pragma once

#include <cstdint>

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
};

struct Request {
    RequestId request_id = kInvalidRequestId;
    NodeId source_node_id = 0;
    ObjectId object_id = 0;
    OperationType operation_type = OperationType::Read;
    SimTime issue_time = 0;
    std::uint64_t size_bytes = 0;
    RequestStage current_stage = RequestStage::Generated;
};

}  // namespace dm_sim
