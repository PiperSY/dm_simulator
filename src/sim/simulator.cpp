#include "sim/simulator.hpp"

#include <stdexcept>

namespace dm_sim {

Simulator::Simulator(SimulationConfig config)
    : config_(std::move(config)),
      compute_node_(config_.compute_node_id,
                    config_.memory_node_id,
                    config_,
                    next_request_id_,
                    request_table_,
                    responses_,
                    stats_),
      memory_node_(config_.memory_node_id, config_, request_table_) {
    if (config_.memory_bandwidth_bytes_per_time == 0) {
        throw std::invalid_argument(
            "memory_bandwidth_bytes_per_time must be greater than zero");
    }
}

void Simulator::run() {
    if (!config_.requests.empty()) {
        scheduler_.schedule(
            Event(0, EventType::GenerateRequest, config_.compute_node_id));
    }

    scheduler_.run_until_empty([this](const Event& event, Scheduler& scheduler) {
        dispatch_event(event, scheduler);
    });
}

const Stats& Simulator::stats() const noexcept {
    return stats_;
}

const std::unordered_map<RequestId, Request>& Simulator::requests() const noexcept {
    return request_table_;
}

const std::vector<Response>& Simulator::responses() const noexcept {
    return responses_;
}

const std::vector<EventRecord>& Simulator::event_log() const noexcept {
    return event_log_;
}

const ComputeNode& Simulator::compute_node() const noexcept {
    return compute_node_;
}

const MemoryNode& Simulator::memory_node() const noexcept {
    return memory_node_;
}

const SimulationConfig& Simulator::config() const noexcept {
    return config_;
}

void Simulator::dispatch_event(const Event& event, Scheduler& scheduler) {
    event_log_.push_back(
        EventRecord{event.time, event.type, event.target_id, event.request_id});

    if (event.target_id == config_.compute_node_id) {
        compute_node_.handle_event(event, scheduler);
        return;
    }

    if (event.target_id == config_.memory_node_id) {
        memory_node_.handle_event(event, scheduler);
        return;
    }

    throw std::logic_error("Unknown target_id in simulator dispatch");
}

}  // namespace dm_sim
