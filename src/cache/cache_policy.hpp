#pragma once

#include <optional>
#include <unordered_map>

#include "cache/cache_entry.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace dm_sim {

class CachePolicy {
public:
    virtual ~CachePolicy() = default;

    virtual void on_access(CacheEntry& entry, SimTime access_time) const = 0;
    [[nodiscard]] virtual bool should_admit(const Request& request,
                                            const Response& response) const = 0;
    [[nodiscard]] virtual std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries) const = 0;
};

class AlwaysRemotePolicy final : public CachePolicy {
public:
    void on_access(CacheEntry& entry, SimTime access_time) const override;
    [[nodiscard]] bool should_admit(const Request& request,
                                    const Response& response) const override;
    [[nodiscard]] std::optional<ObjectId> select_victim(
        const std::unordered_map<ObjectId, CacheEntry>& entries) const override;
};

}  // namespace dm_sim
