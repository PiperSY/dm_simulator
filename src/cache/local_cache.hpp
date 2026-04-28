#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "cache/cache_entry.hpp"
#include "cache/cache_policy.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace dm_sim {

class LocalCache {
public:
    LocalCache(std::size_t capacity_bytes, std::unique_ptr<CachePolicy> policy);

    LocalCache(const LocalCache&) = delete;
    LocalCache& operator=(const LocalCache&) = delete;
    LocalCache(LocalCache&&) noexcept = default;
    LocalCache& operator=(LocalCache&&) noexcept = default;

    [[nodiscard]] bool lookup(ObjectId object_id, SimTime access_time);
    [[nodiscard]] bool admit(const Request& request,
                             const Response& response,
                             SimTime access_time);
    [[nodiscard]] bool contains(ObjectId object_id) const noexcept;
    [[nodiscard]] std::size_t occupancy_bytes() const noexcept;
    [[nodiscard]] std::size_t capacity_bytes() const noexcept;
    [[nodiscard]] std::size_t entry_count() const noexcept;
    [[nodiscard]] std::size_t hit_count() const noexcept;
    [[nodiscard]] std::size_t miss_count() const noexcept;
    [[nodiscard]] std::uint64_t bytes_admitted() const noexcept;
    [[nodiscard]] std::uint64_t bytes_evicted() const noexcept;

private:
    void evict(ObjectId object_id);

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
