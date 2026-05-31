#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>

#include "cache/cache_policy.hpp"
#include "sim/config.hpp"

namespace dm_sim {

/*********************************** 
 * HotnessOnlyPolicy is a cache policy that admits entries into the cache based on their access frequency (hotness). 
 * It maintains access counts for each object and uses a configurable threshold to determine admission. 
 * The policy can track one epoch, all epochs, or a bounded window of recent epochs.
 ***********************************/
class HotnessOnlyPolicy final : public CachePolicy {
public:
    explicit HotnessOnlyPolicy(HotnessPolicyConfig config);

    // Overrides for CachePolicy virtual methods to implement the hotness-based admission and eviction logic.
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

    // Retrieves the access count for a specific object ID, which is used to determine the hotness of the object for admission decisions.
    [[nodiscard]] std::uint64_t access_count(ObjectId object_id) const;

private:
    // Helper method to get the access count for an object, returning 0 if the object has not been accessed before.
    [[nodiscard]] std::uint64_t count_for(ObjectId object_id) const;
    void prune_window(EpochId epoch_id) const;
    void subtract_epoch_counts(
        const std::unordered_map<ObjectId, std::uint64_t>& epoch_counts) const;
    // Configuration for admission threshold and how much demand history is retained.
    HotnessPolicyConfig config_;
    // Effective hotness counts after applying the configured history mode.
    mutable std::unordered_map<ObjectId, std::uint64_t> access_counts_;
    // Windowed mode keeps per-epoch buckets so old demand can be subtracted.
    mutable std::unordered_map<EpochId, std::unordered_map<ObjectId, std::uint64_t>>
        epoch_access_counts_;
};

/*********************************** 
 * GlobalHottestReplicationPolicy is a cache policy that admits entries based on their global access frequency across all compute nodes. 
 * It selects victims for eviction based on the least frequently accessed entries, with ties broken by recency and object ID.
 * This policy utilizes oracle knowledge of the global access patterns to make admission and eviction decisions and serves as a basline rather than a practical policy.
 ***********************************/
class GlobalHottestReplicationPolicy final : public CachePolicy {
public:
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries,
        const Request& incoming_request) const override;
};

}  // namespace dm_sim
