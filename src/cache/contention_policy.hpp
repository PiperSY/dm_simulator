#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "cache/cache_policy.hpp"
#include "metrics/stats.hpp"
#include "sim/config.hpp"

namespace dm_sim {

class ContentionAwarePolicy final : public CachePolicy {
public:
    ContentionAwarePolicy(ContentionPolicyConfig config,
                          std::uint64_t cache_capacity_bytes,
                          const Stats& stats);

    void on_lookup(const Request& request,
                   SimTime access_time,
                   bool hit) const override;
    void on_epoch_start(EpochId epoch_id) const override;
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries,
        const Request& incoming_request) const override;
    void on_admission_result(
        const Request& request,
        const Response& response,
        SimTime access_time,
        bool admitted,
        const std::string& reason,
        const std::vector<ObjectId>& evicted_objects) const override;
    [[nodiscard]] std::vector<PolicyDecisionRecord> diagnostics()
        const override;

    [[nodiscard]] ContentionScoreComponents score_object(
        ObjectId object_id,
        std::uint64_t size_bytes) const;
    [[nodiscard]] std::uint64_t local_access_count(ObjectId object_id) const;

private:
    struct NormalizationMaxima {
        std::size_t remote_accesses = 0;
        std::size_t distinct_requesters = 0;
        SimTime total_queue_wait = 0;
        SimTime total_remote_service_time = 0;
    };

    [[nodiscard]] double normalized(double value, double max_value) const;
    [[nodiscard]] std::uint64_t count_for(ObjectId object_id) const;

    ContentionPolicyConfig config_;
    std::uint64_t cache_capacity_bytes_ = 0;
    const Stats& stats_;
    mutable std::optional<EpochId> current_epoch_;
    mutable std::unordered_map<ObjectId, std::uint64_t> local_access_counts_;
    mutable std::unordered_map<ObjectId, ObjectContentionStats>
        previous_epoch_stats_;
    mutable NormalizationMaxima maxima_;
    mutable std::vector<PolicyDecisionRecord> diagnostics_;
};

}  // namespace dm_sim
