#pragma once

#include "cache/cache_policy.hpp"

namespace dm_sim {

/*********************************** 
 * LruPolicy implements a simple Least Recently Used (LRU) eviction strategy. 
 * It tracks the order of cache entries based on their last access time and evicts the least recently accessed entry when the cache is full.
 ***********************************/
class LruPolicy final : public CachePolicy {
public:
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries,
        const Request& incoming_request) const override;
};

}  // namespace dm_sim
