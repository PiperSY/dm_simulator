#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "cache/cache_policy.hpp"
#include "metrics/stats.hpp"
#include "sim/config.hpp"

namespace dm_sim {

/*********************************** 
 * ContentionAwarePolicy implements a cache eviction policy that takes into account contention metrics to make informed decisions about which objects to admit and evict. 
 * It uses a configurable scoring mechanism based on various contention-related features, such as remote access counts, distinct requesters, queue wait times, and remote service times. 
 * The policy tracks access patterns and contention metrics across epochs to adapt its decisions over time.
 *  - comfig: Configuration parameters for the contention-aware policy, including weights for different contention features and thresholds for admission decisions.
 *  - cache_capacity_bytes: The total capacity of the cache in bytes, used to calculate size penalties in the scoring mechanism.
 *  - stats: A reference to the Stats object, which provides access to contention metrics and other statistics needed for scoring and decision-making.
 ***********************************/
class ContentionAwarePolicy final : public CachePolicy {
public:
    ContentionAwarePolicy(ContentionPolicyConfig config,
                          std::uint64_t cache_capacity_bytes,
                          const Stats& stats);

    // Override methods from CachePolicy to implement the contention-aware eviction strategy, including handling cache lookups, epoch changes, access events, admission decisions, and diagnostics.
    void on_lookup(const Request& request,
                   SimTime access_time,
                   bool hit) const override;
    void on_epoch_start(EpochId epoch_id) const override;
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] AdmissionDecision admission_decision(
        const Request& request,
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

    // Helper method to calculate the contention score components for a given object, which are used to determine the overall contention score and make admission/eviction decisions.
    [[nodiscard]] ContentionScoreComponents score_object(
        ObjectId object_id,
        std::uint64_t size_bytes) const;
    // Local access count for the specified object ID, which is used as a feature in the contention scoring mechanism to reflect the "hotness" of the object within the local cache.
    [[nodiscard]] std::uint64_t local_access_count(ObjectId object_id) const;

private:
    // Normalization maxima for contention features, used to normalize feature values when calculating contention scores. 
    // These maxima are updated based on observed contention metrics to ensure that scores are scaled appropriately.
    struct NormalizationMaxima {
        double remote_accesses = 0.0;
        double distinct_requesters = 0.0;
        double total_queue_wait = 0.0;
        double total_remote_service_time = 0.0;
        double cost_density = 0.0;
    };
    // Struct to hold the raw components of the contention score for an object, which are calculated based on the observed contention metrics and the configured weights.
    struct ScoringContentionStats {
        double remote_accesses = 0.0;
        double distinct_requesters = 0.0;
        double bytes_served = 0.0;
        double total_queue_wait = 0.0;
        double total_remote_service_time = 0.0;
    };

    // Normalizes a given value based on the corresponding maximum value observed for that feature, which helps to scale the contention score components appropriately and 
    //  ensure that they are comparable across different objects and epochs.
    [[nodiscard]] double normalized(double value, double max_value) const;
    // Count of local accesses for the specified object ID, which is used to determine the local hotness component of the contention score. 
    [[nodiscard]] std::uint64_t count_for(ObjectId object_id) const;
    [[nodiscard]] std::uint64_t confirmation_count_for(
        ObjectId object_id) const;
    [[nodiscard]] bool uses_smoothed_telemetry() const noexcept;
    [[nodiscard]] bool uses_local_confirmation() const noexcept;
    [[nodiscard]] bool uses_hysteresis() const noexcept;
    void prune_local_confirmation(EpochId epoch_id) const;
    // Estimates prior remote pain per byte of cache space from completed epoch summaries.
    [[nodiscard]] double cost_per_cache_byte(
        const ScoringContentionStats& stats) const;
    // Adds a snapshot of contention statistics for a given object to the internal state. Used to track contention metrics across epochs and inform future scoring and decision-making.
    void add_contention_snapshot(const ObjectContentionStats& object_stats,
                                 double weight) const;
    // Updates the normalization maxima based on observed contention metrics, ensuring that the scoring mechanism remains effective and appropriately scaled as contention patterns evolve over time.
    void refresh_normalization_maxima() const;
    // Label identifying the active variant of the contention policy. Used in diagnostics and output display.
    [[nodiscard]] std::string variant_label() const;

    ContentionPolicyConfig config_;                                             // Configuration parameters for the contention-aware policy, including weights for different contention features and thresholds for admission decisions.
    std::uint64_t cache_capacity_bytes_ = 0;                                    // The total capacity of the cache in bytes, used to calculate size penalties in the scoring mechanism.
    const Stats& stats_;                                                        // A reference to the Stats object -> access to contention metrics and other statistics needed for scoring and decision-making.
    mutable std::optional<EpochId> current_epoch_;                              // The current epoch ID -> track epoch changes and update internal state.
    mutable std::unordered_map<ObjectId, std::uint64_t> local_access_counts_;   // Local access counts for objects -> used to determine the local hotness component of the contention score.
    // Composite confirmation is independent from local hotness reset behavior:
    // epoch buckets provide bounded memory and the aggregate keeps lookup O(1).
    mutable std::map<
        EpochId,
        std::unordered_map<ObjectId, std::uint64_t>>
        local_confirmation_by_epoch_;
    mutable std::unordered_map<ObjectId, std::uint64_t>
        local_confirmation_counts_;
    mutable std::unordered_map<ObjectId, ScoringContentionStats>                 
        previous_epoch_stats_;                                                  // Prior contention telemetry used to calculate scores for the current epoch.
    mutable NormalizationMaxima maxima_;                                        // Normalization maxima instance for contention features.
    mutable std::vector<PolicyDecisionRecord> diagnostics_;                     // Diagnostics records of policy decisions -> used for analysis and debugging.
};

}  // namespace dm_sim
