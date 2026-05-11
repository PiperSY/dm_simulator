#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "cache/cache_entry.hpp"
#include "cache/cache_policy.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace dm_sim {

struct CacheReplica {
    ObjectId object_id = 0;
    std::uint64_t size_bytes = 0;
};

using GlobalReplicaPlan = std::unordered_map<EpochId, std::vector<CacheReplica>>;

/*********************************** 
 * LocalCache class represents a local cache in the simulation. It manages cache entries, tracks occupancy and hit/miss statistics, 
 * and interacts with a specified cache policy to determine admission and eviction of entries. The LocalCache provides methods for 
 * looking up entries, admitting new entries based on requests and responses, and retrieving cache statistics.
 ***********************************/
class LocalCache {
public:
    LocalCache(std::size_t capacity_bytes, std::unique_ptr<CachePolicy> policy);

    // Delete copy constructor and copy assignment operator to prevent copying of the LocalCache, which manages unique resources (cache entries and policy).
    LocalCache(const LocalCache&) = delete;
    LocalCache& operator=(const LocalCache&) = delete;
    // Default move constructor and move assignment operator to allow moving of the LocalCache.
    LocalCache(LocalCache&&) noexcept = default;
    LocalCache& operator=(LocalCache&&) noexcept = default;

    // Lookup an object in the cache, updating hit/miss statistics and access times as appropriate. 
    [[nodiscard]] bool lookup(ObjectId object_id, SimTime access_time);
    [[nodiscard]] bool lookup(const Request& request, SimTime access_time);
    // Attempt to admit a new entry into the cache based on the given request and response, using the cache policy to determine admission and eviction.
    [[nodiscard]] bool admit(const Request& request,
                             const Response& response,
                             SimTime access_time);
    // Handle the start of a new epoch -> update state as needed for the new epoch.
    void on_epoch_start(EpochId epoch_id);
    // Install a set of cache replicas into the local cache, evicting existing entries as needed to make space.
    void install_replicas(const std::vector<CacheReplica>& replicas,
                          SimTime install_time);
    // Check if the cache contains an entry for the specified object ID.
    [[nodiscard]] bool contains(ObjectId object_id) const noexcept;
    // Retrieve a constant reference to the cache entry for the specified object ID, if it exists.
    [[nodiscard]] const CacheEntry& entry(ObjectId object_id) const;
    // Retrieve a reference to the cache entry for the specified object ID, if it exists.
    [[nodiscard]] CacheEntry& entry(ObjectId object_id);
    // Getters for cache statistics, including current occupancy, total capacity, entry count, hit/miss counts, and bytes admitted/evicted.
    [[nodiscard]] std::size_t occupancy_bytes() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept;
    [[nodiscard]] std::size_t hit_count() const noexcept;
    [[nodiscard]] std::size_t miss_count() const noexcept;
    [[nodiscard]] std::uint64_t bytes_admitted() const noexcept;
    [[nodiscard]] std::uint64_t bytes_evicted() const noexcept;
    [[nodiscard]] std::vector<PolicyDecisionRecord> policy_diagnostics() const;

private:
    // Evict the specified object from the cache, updating occupancy and eviction statistics accordingly.
    void evict(ObjectId object_id);
    // Clear all entries from the cache, resetting occupancy and statistics. Used when installing new replicas for a new epoch.
    void clear_entries();

    // Cache capacity in bytes, current occupancy in bytes, hit/miss statistics, and the cache policy used for admission and eviction decisions.
    std::size_t capacity_bytes_ = 0;
    std::size_t occupancy_bytes_ = 0;
    std::size_t hit_count_ = 0;
    std::size_t miss_count_ = 0;
    std::uint64_t bytes_admitted_ = 0;
    std::uint64_t bytes_evicted_ = 0;
    std::unique_ptr<CachePolicy> policy_;
    std::unordered_map<ObjectId, CacheEntry> entries_;
};

}  // namespace dm_sim
