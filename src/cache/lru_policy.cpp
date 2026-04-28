#include "cache/lru_policy.hpp"

namespace dm_sim {

void AlwaysRemotePolicy::on_access(CacheEntry& entry, SimTime access_time) const {
    (void)entry;
    (void)access_time;
}

bool AlwaysRemotePolicy::should_admit(const Request& request,
                                      const Response& response) const {
    (void)request;
    (void)response;
    return false;
}

std::optional<ObjectId> AlwaysRemotePolicy::select_victim(
    const std::unordered_map<ObjectId, CacheEntry>& entries) const {
    (void)entries;
    return std::nullopt;
}

void LruPolicy::on_access(CacheEntry& entry, SimTime access_time) const {
    entry.last_access_time = access_time;
}

bool LruPolicy::should_admit(const Request& request, const Response& response) const {
    (void)request;
    (void)response;
    return true;
}

std::optional<ObjectId> LruPolicy::select_victim(
    const std::unordered_map<ObjectId, CacheEntry>& entries) const {
    if (entries.empty()) {
        return std::nullopt;
    }

    auto oldest = entries.begin();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        if (it->second.last_access_time < oldest->second.last_access_time) {
            oldest = it;
            continue;
        }

        if (it->second.last_access_time == oldest->second.last_access_time &&
            it->second.object_id < oldest->second.object_id) {
            oldest = it;
        }
    }

    return oldest->first;
}

}  // namespace dm_sim
