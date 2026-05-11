#include "sim/simulator.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "cache/cache_policy.hpp"
#include "cache/contention_policy.hpp"
#include "cache/hotness_policy.hpp"
#include "cache/local_cache.hpp"
#include "cache/lru_policy.hpp"
#include "workloads/generators.hpp"

namespace dm_sim {

namespace {

// Factory function to create cache policy instances based on the specified policy type.
std::unique_ptr<CachePolicy> make_cache_policy(
    const LocalCacheConfig& local_cache_config,
    const Stats& stats) {
    switch (local_cache_config.policy_type) {
    case LocalCachePolicyType::AlwaysRemote:
        return std::make_unique<AlwaysRemotePolicy>();
    case LocalCachePolicyType::Lru:
        return std::make_unique<LruPolicy>();
    case LocalCachePolicyType::HotnessOnly:
        return std::make_unique<HotnessOnlyPolicy>(local_cache_config.hotness);
    case LocalCachePolicyType::GlobalHottestReplication:
        return std::make_unique<GlobalHottestReplicationPolicy>();
    case LocalCachePolicyType::ContentionAware:
        return std::make_unique<ContentionAwarePolicy>(
            local_cache_config.contention,
            local_cache_config.capacity_bytes,
            stats);
    }

    throw std::invalid_argument("Unknown local cache policy type");
}
/*********************************** 
 * Builds a global replica plan based on the access patterns of compute nodes across epochs. 
 * The plan identifies which objects should be replicated in the local cache for each epoch, prioritizing objects with higher request counts.
 * This function processes the request specifications of all compute nodes to determine the demand for each object in each epoch and generates 
 *   a sorted list of replicas to be installed in the local cache according to the GlobalHottestReplicationPolicy.
 ***********************************/
GlobalReplicaPlan build_global_replica_plan(
    const std::vector<ComputeNodeConfig>& compute_nodes) {
    // Tracks how often one object is requested during one epoch, plus the
    // largest observed object size needed to reserve cache space for it.
    struct ObjectDemand {
        std::uint64_t request_count = 0;
        std::uint64_t size_bytes = 0;
    };

    // First pass: group all requests by epoch, then by object ID, so the
    // replication policy can rank objects using global demand.
    std::unordered_map<EpochId, std::unordered_map<ObjectId, ObjectDemand>>
        demand_by_epoch;

    for (const ComputeNodeConfig& node_config : compute_nodes) {
        for (const RequestSpec& request : node_config.requests) {
            ObjectDemand& demand =
                demand_by_epoch[request.epoch_id][request.object_id];
            ++demand.request_count;
            demand.size_bytes = std::max(demand.size_bytes, request.size_bytes);
        }
    }

    // Second pass: convert each epoch's demand table into an ordered replica
    // list that LocalCache::install_replicas can consume.
    GlobalReplicaPlan plan;
    for (const auto& epoch_entry : demand_by_epoch) {
        std::vector<CacheReplica> replicas;
        replicas.reserve(epoch_entry.second.size());

        // A replica carries only the object identity and size. The request
        // count stays in epoch_entry.second and is used only for sorting.
        for (const auto& object_entry : epoch_entry.second) {
            replicas.push_back(CacheReplica{
                object_entry.first,
                object_entry.second.size_bytes,
            });
        }

        // Put the hottest objects first. Ties are deterministic so repeated
        // runs produce the same replica order.
        std::sort(replicas.begin(),
                  replicas.end(),
                  [&epoch_entry](const CacheReplica& lhs,
                                 const CacheReplica& rhs) {
                      const std::uint64_t lhs_count =
                          epoch_entry.second.at(lhs.object_id).request_count;
                      const std::uint64_t rhs_count =
                          epoch_entry.second.at(rhs.object_id).request_count;
                      if (lhs_count != rhs_count) {
                          return lhs_count > rhs_count;
                      }

                      return lhs.object_id < rhs.object_id;
                  });

        plan[epoch_entry.first] = std::move(replicas);
    }

    return plan;
}

}  // namespace


Simulator::Simulator(SimulationConfig config)
    : config_(std::move(config)),
      memory_node_(config_.memory_node_id, config_, stats_, request_table_) {
    if (config_.synthetic_workload.has_value()) {
        if (!config_.compute_nodes.empty()) {
            throw std::invalid_argument(
                "Use either synthetic_workload or explicit compute_nodes, not both");
        }

        generated_workload_ =
            generate_synthetic_workload(*config_.synthetic_workload);
        config_.compute_nodes.reserve(generated_workload_->node_workloads.size());
        for (const NodeWorkload& node_workload :
             generated_workload_->node_workloads) {
            config_.compute_nodes.push_back(
                ComputeNodeConfig{node_workload.node_id, node_workload.requests});
        }
    }

    validate_config();

    if (config_.memory_bandwidth_bytes_per_time == 0) {
        throw std::invalid_argument(
            "memory_bandwidth_bytes_per_time must be greater than zero");
    }

    if (config_.local_cache.policy_type ==
        LocalCachePolicyType::GlobalHottestReplication) {
        global_replica_plan_ = build_global_replica_plan(config_.compute_nodes);
    }

    const GlobalReplicaPlan* replica_plan =
        config_.local_cache.policy_type ==
                LocalCachePolicyType::GlobalHottestReplication
            ? &global_replica_plan_
            : nullptr;

    for (const ComputeNodeConfig& node_config : config_.compute_nodes) {
        compute_nodes_.emplace(
            node_config.node_id,
            std::make_unique<ComputeNode>(node_config.node_id,
                                          config_.memory_node_id,
                                          WorkloadCursor(node_config.requests),
                                          config_.one_way_link_latency,
                                          config_.local_cache.hit_latency,
                                          LocalCache(
                                              config_.local_cache.capacity_bytes,
                                              make_cache_policy(config_.local_cache,
                                                                stats_)),
                                          replica_plan,
                                          next_request_id_,
                                          request_table_,
                                          responses_,
                                          stats_));
    }
}

void Simulator::run() {
    release_next_epoch_if_ready(scheduler_);

    scheduler_.run_until_empty([this](const Event& event, Scheduler& scheduler) {
        dispatch_event(event, scheduler);
        release_next_epoch_if_ready(scheduler);
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

const ComputeNode& Simulator::compute_node(NodeId node_id) const {
    const auto it = compute_nodes_.find(node_id);
    if (it == compute_nodes_.end()) {
        throw std::out_of_range("Unknown compute node ID");
    }

    return *(it->second);
}

const MemoryNode& Simulator::memory_node() const noexcept {
    return memory_node_;
}

const SimulationConfig& Simulator::config() const noexcept {
    return config_;
}

const std::optional<GeneratedWorkload>& Simulator::generated_workload()
    const noexcept {
    return generated_workload_;
}

std::vector<PolicyDecisionRecord> Simulator::policy_diagnostics() const {
    std::vector<PolicyDecisionRecord> diagnostics;
    for (const ComputeNodeConfig& node_config : config_.compute_nodes) {
        const auto compute_it = compute_nodes_.find(node_config.node_id);
        if (compute_it == compute_nodes_.end()) {
            continue;
        }

        std::vector<PolicyDecisionRecord> node_diagnostics =
            compute_it->second->policy_diagnostics();
        diagnostics.insert(diagnostics.end(),
                           node_diagnostics.begin(),
                           node_diagnostics.end());
    }

    std::sort(diagnostics.begin(),
              diagnostics.end(),
              [](const PolicyDecisionRecord& lhs,
                 const PolicyDecisionRecord& rhs) {
                  if (lhs.epoch_id != rhs.epoch_id) {
                      return lhs.epoch_id < rhs.epoch_id;
                  }
                  if (lhs.request_id != rhs.request_id) {
                      return lhs.request_id < rhs.request_id;
                  }
                  if (lhs.node_id != rhs.node_id) {
                      return lhs.node_id < rhs.node_id;
                  }
                  return lhs.object_id < rhs.object_id;
              });
    return diagnostics;
}

void Simulator::dispatch_event(const Event& event, Scheduler& scheduler) {
    event_log_.push_back(
        EventRecord{event.time, event.type, event.target_id, event.request_id});

    const auto compute_it = compute_nodes_.find(event.target_id);
    if (compute_it != compute_nodes_.end()) {
        compute_it->second->handle_event(event, scheduler);
        return;
    }

    if (event.target_id == config_.memory_node_id) {
        memory_node_.handle_event(event, scheduler);
        return;
    }

    throw std::logic_error("Unknown target_id in simulator dispatch");
}

void Simulator::release_next_epoch_if_ready(Scheduler& scheduler) {
    if (!scheduler.empty() || !all_compute_nodes_idle()) {
        return;
    }

    const std::optional<EpochId> next_epoch = next_unreleased_epoch();
    if (!next_epoch.has_value()) {
        return;
    }

    released_epoch_ = *next_epoch;
    for (const ComputeNodeConfig& node_config : config_.compute_nodes) {
        const auto compute_it = compute_nodes_.find(node_config.node_id);
        if (compute_it == compute_nodes_.end()) {
            continue;
        }

        const std::optional<EpochId> node_next_epoch =
            compute_it->second->next_request_epoch();
        if (node_next_epoch.has_value() && *node_next_epoch == *next_epoch) {
            scheduler.schedule(Event(scheduler.now(),
                                     EventType::GenerateRequest,
                                     node_config.node_id));
        }
    }
}

bool Simulator::all_compute_nodes_idle() const noexcept {
    for (const auto& entry : compute_nodes_) {
        if (entry.second->outstanding_requests() != 0) {
            return false;
        }
    }

    return true;
}

std::optional<EpochId> Simulator::next_unreleased_epoch() const {
    std::optional<EpochId> next_epoch;
    for (const auto& entry : compute_nodes_) {
        const std::optional<EpochId> node_epoch =
            entry.second->next_request_epoch();
        if (!node_epoch.has_value()) {
            continue;
        }

        if (released_epoch_.has_value() && *node_epoch <= *released_epoch_) {
            continue;
        }

        if (!next_epoch.has_value() || *node_epoch < *next_epoch) {
            next_epoch = *node_epoch;
        }
    }

    return next_epoch;
}

void Simulator::validate_config() const {
    if (config_.local_cache.policy_type ==
            LocalCachePolicyType::GlobalHottestReplication &&
        !generated_workload_.has_value()) {
        throw std::invalid_argument(
            "Global hottest replication requires synthetic_workload");
    }

    std::unordered_set<NodeId> seen_compute_node_ids;

    for (const ComputeNodeConfig& node_config : config_.compute_nodes) {
        if (node_config.node_id == config_.memory_node_id) {
            throw std::invalid_argument(
                "Compute node ID must not match the memory node ID");
        }

        const auto [_, inserted] = seen_compute_node_ids.insert(node_config.node_id);
        if (!inserted) {
            throw std::invalid_argument("Compute node IDs must be unique");
        }

        for (std::size_t i = 1; i < node_config.requests.size(); ++i) {
            if (node_config.requests[i].epoch_id <
                node_config.requests[i - 1].epoch_id) {
                throw std::invalid_argument(
                    "Compute node request streams must be sorted by epoch_id");
            }
        }
    }
}

}  // namespace dm_sim
