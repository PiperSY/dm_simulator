#pragma once

#include <unordered_map>
#include <vector>

#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "model/response.hpp"
#include "nodes/compute_node.hpp"
#include "nodes/memory_node.hpp"
#include "sim/config.hpp"
#include "sim/event.hpp"
#include "sim/scheduler.hpp"

namespace dm_sim {

struct EventRecord {
    SimTime time = 0;
    EventType type = EventType::GenerateRequest;
    NodeId target_id = 0;
    RequestId request_id = kInvalidRequestId;
};

class Simulator {
public:
    explicit Simulator(SimulationConfig config);

    void run();

    [[nodiscard]] const Stats& stats() const noexcept;
    [[nodiscard]] const std::unordered_map<RequestId, Request>& requests() const noexcept;
    [[nodiscard]] const std::vector<Response>& responses() const noexcept;
    [[nodiscard]] const std::vector<EventRecord>& event_log() const noexcept;
    [[nodiscard]] const ComputeNode& compute_node() const noexcept;
    [[nodiscard]] const MemoryNode& memory_node() const noexcept;
    [[nodiscard]] const SimulationConfig& config() const noexcept;

private:
    void dispatch_event(const Event& event, Scheduler& scheduler);

    SimulationConfig config_;
    Scheduler scheduler_;
    RequestId next_request_id_ = 1;
    std::unordered_map<RequestId, Request> request_table_;
    std::vector<Response> responses_;
    Stats stats_;
    ComputeNode compute_node_;
    MemoryNode memory_node_;
    std::vector<EventRecord> event_log_;
};

}  // namespace dm_sim
