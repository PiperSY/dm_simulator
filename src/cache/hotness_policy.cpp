#include "cache/hotness_policy.hpp"

namespace dm_sim {

HotnessOnlyPolicy::HotnessOnlyPolicy(HotnessPolicyConfig config)
    : config_(config) {}

void HotnessOnlyPolicy::on_lookup(const Request& request,
                                  SimTime access_time,
                                  bool hit) const {
    (void)access_time;
    (void)hit;
    ++access_counts_[request.object_id];
}

void HotnessOnlyPolicy::on_epoch_start(EpochId epoch_id) const {
    (void)epoch_id;
    if (config_.reset_on_epoch_change) {
        access_counts_.clear();
    }
}

void HotnessOnlyPolicy::on_access(CacheEntry& entry, SimTime access_time) const {
    entry.last_access_time = access_time;
}

bool HotnessOnlyPolicy::should_admit(const Request& request,
                                     const Response& response) const {
    (void)response;
    return count_for(request.object_id) >= config_.min_admit_count;
}

std::optional<ObjectId> HotnessOnlyPolicy::select_victim(
    const std::unordered_map<ObjectId, CacheEntry>& entries,
    const Request& incoming_request) const {
    if (entries.empty()) {
        return std::nullopt;
    }

    auto coldest = entries.begin();
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const std::uint64_t candidate_count = count_for(it->first);
        const std::uint64_t coldest_count = count_for(coldest->first);

        if (candidate_count < coldest_count) {
            coldest = it;
            continue;
        }

        if (candidate_count == coldest_count &&
            it->second.last_access_time < coldest->second.last_access_time) {
            coldest = it;
            continue;
        }

        if (candidate_count == coldest_count &&
            it->second.last_access_time == coldest->second.last_access_time &&
            it->second.object_id < coldest->second.object_id) {
            coldest = it;
        }
    }

    if (count_for(incoming_request.object_id) <= count_for(coldest->first)) {
        return std::nullopt;
    }

    return coldest->first;
}

std::uint64_t HotnessOnlyPolicy::access_count(ObjectId object_id) const {
    return count_for(object_id);
}

std::uint64_t HotnessOnlyPolicy::count_for(ObjectId object_id) const {
    const auto it = access_counts_.find(object_id);
    if (it == access_counts_.end()) {
        return 0;
    }

    return it->second;
}

void GlobalHottestReplicationPolicy::on_access(CacheEntry& entry,
                                               SimTime access_time) const {
    entry.last_access_time = access_time;
}

bool GlobalHottestReplicationPolicy::should_admit(
    const Request& request,
    const Response& response) const {
    (void)request;
    (void)response;
    return false;
}

std::optional<ObjectId> GlobalHottestReplicationPolicy::select_victim(
    const std::unordered_map<ObjectId, CacheEntry>& entries,
    const Request& incoming_request) const {
    (void)entries;
    (void)incoming_request;
    return std::nullopt;
}

}  // namespace dm_sim
