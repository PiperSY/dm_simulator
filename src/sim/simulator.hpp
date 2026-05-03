#pragma once

#include <memory>
#include <optional>
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

// Record of events for logging purposes, capturing the time, type, target node, and associated request ID of each event.
struct EventRecord {
    SimTime time = 0;
    EventType type = EventType::GenerateRequest;
    NodeId target_id = 0;
    RequestId request_id = kInvalidRequestId;
};

/*********************************** 
 * Simulator class orchestrates the entire simulation, managing compute nodes, the memory node, event scheduling, 
 * and statistics collection. It initializes the simulation based on the provided configuration, runs the event loop, 
 * and provides access to the collected statistics, requests, responses, and event logs after the simulation completes.
 ***********************************/
class Simulator {
public:
    // Constructor that initializes the simulator with the given configuration. It sets up compute nodes, the memory node, and validates the configuration.
    explicit Simulator(SimulationConfig config);
    // Runs the simulation by signaling the scheduler to process events until the queue is empty.
    void run();

    // Accessor methods to retrieve statistics, requests, responses, event logs, and node information after the simulation has completed.
    [[nodiscard]] const Stats& stats() const noexcept;
    // Returns a constant reference to the map of requests, providing read-only access to the details of each request processed during the simulation.
    [[nodiscard]] const std::unordered_map<RequestId, Request>& requests() const noexcept;
    // Returns a constant reference to the vector of responses, providing read-only access to the details of each response generated during the simulation.
    [[nodiscard]] const std::vector<Response>& responses() const noexcept;
    // Returns a constant reference to the vector of event records. 
    [[nodiscard]] const std::vector<EventRecord>& event_log() const noexcept;
    // Returns a constant reference to the compute node with the specified ID. Throws an exception if the node ID is unknown.
    [[nodiscard]] const ComputeNode& compute_node(NodeId node_id) const;
    // Returns a constant reference to the memory node. Currently only one memory node.
    [[nodiscard]] const MemoryNode& memory_node() const noexcept;
    // Returns a constant reference to the simulation configuration used to initialize the simulator.
    [[nodiscard]] const SimulationConfig& config() const noexcept;
    [[nodiscard]] const std::optional<GeneratedWorkload>& generated_workload()
        const noexcept;

private:
    // Dispatches the given event to the appropriate node (compute or memory) based on the event's target ID.
    void dispatch_event(const Event& event, Scheduler& scheduler);
    // Validates the simulation configuration to ensure it meets necessary constraints and requirements before running the simulation.
    void validate_config() const;

    SimulationConfig config_;
    std::optional<GeneratedWorkload> generated_workload_;
    Scheduler scheduler_;
    RequestId next_request_id_ = 1;
    std::unordered_map<RequestId, Request> request_table_;
    std::vector<Response> responses_;
    Stats stats_;
    std::unordered_map<NodeId, std::unique_ptr<ComputeNode>> compute_nodes_;
    MemoryNode memory_node_;
    std::vector<EventRecord> event_log_;
};

}  // namespace dm_sim
