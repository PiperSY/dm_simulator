#pragma once

#include <optional>
#include <unordered_map>

#include "cache/cache_entry.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace dm_sim {

// Base class for cache policies
class CachePolicy {
public:
    // Virtual destructor for proper cleanup of derived classes
    virtual ~CachePolicy() = default;

    // Called when a cache entry is accessed (e.g., read or written)
    virtual void on_access(CacheEntry& entry, SimTime access_time) const = 0;
    // Determines whether a request should be admitted into the cache based on the request and response
    [[nodiscard]] virtual bool should_admit(const Request& request,
                                            const Response& response) const = 0;
    // Selects a victim entry to evict from the cache when space is needed                                        
    [[nodiscard]] virtual std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries) const = 0;
};

// A simple cache policy that never admits any entries into the cache and always evicts the least recently accessed entry
class AlwaysRemotePolicy final : public CachePolicy {
public:
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries) const override;
};

}  // namespace dm_sim
