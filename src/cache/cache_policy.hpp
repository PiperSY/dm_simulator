#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/cache_entry.hpp"
#include "model/epoch.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace dm_sim {

struct ContentionScoreComponents {
    double local_hotness = 0.0;
    double remote_accesses = 0.0;
    double distinct_requesters = 0.0;
    double queue_wait = 0.0;
    double remote_service_time = 0.0;
    double cost_density = 0.0;
    double cost_per_cache_byte = 0.0;
    double size_penalty = 0.0;
    double total_score = 0.0;
};

struct PolicyDecisionRecord {
    NodeId node_id = 0;
    EpochId epoch_id = 0;
    RequestId request_id = kInvalidRequestId;
    ObjectId object_id = 0;
    bool admitted = false;
    std::string reason;
    std::string policy_variant;
    ContentionScoreComponents score;
    std::uint64_t recent_local_confirmation_count = 0;
    std::uint64_t required_local_confirmation_count = 0;
    bool local_confirmation_bypassed = false;
    std::vector<ObjectId> evicted_objects;
};

struct AdmissionDecision {
    bool admit = false;
    std::string reason = "policy_rejected";
};

// Base class for cache policies
class CachePolicy {
public:
    // Virtual destructor for proper cleanup of derived classes
    virtual ~CachePolicy() = default;

    // Called on every cache lookup, allowing the policy to update internal state based on the request, access time, and whether it was a hit or miss.
    virtual void on_lookup(const Request& request,
                           SimTime access_time,
                           bool hit) const;
    // Called at the start of a new epoch, allowing the policy to reset or update internal state as needed for the new epoch.
    virtual void on_epoch_start(EpochId epoch_id) const;
    // Called when a cache entry is accessed (e.g., read or written)
    virtual void on_access(CacheEntry& entry, SimTime access_time) const = 0;
    // Determines whether a request should be admitted into the cache based on the request and response
    [[nodiscard]] virtual bool should_admit(const Request& request,
                                            const Response& response) const = 0;
    // Provides a policy-specific rejection reason while preserving the
    // existing boolean admission interface for policies that do not need it.
    [[nodiscard]] virtual AdmissionDecision admission_decision(
        const Request& request,
        const Response& response) const;
    // Selects a victim entry to evict from the cache when space is needed                                        
    [[nodiscard]] virtual std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries,
        const Request& incoming_request) const = 0;
    virtual void on_admission_result(
        const Request& request,
        const Response& response,
        SimTime access_time,
        bool admitted,
        const std::string& reason,
        const std::vector<ObjectId>& evicted_objects) const;
    [[nodiscard]] virtual std::vector<PolicyDecisionRecord> diagnostics() const;
};

// A simple cache policy that never admits any entries into the cache and always evicts the least recently accessed entry
class AlwaysRemotePolicy final : public CachePolicy {
public:
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries,
        const Request& incoming_request) const override;
};

}  // namespace dm_sim
