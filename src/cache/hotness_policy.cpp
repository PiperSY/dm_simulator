#include "cache/hotness_policy.hpp"

#include <vector>

namespace dm_sim {

HotnessOnlyPolicy::HotnessOnlyPolicy(HotnessPolicyConfig config)
    : config_(config) {}

void HotnessOnlyPolicy::on_lookup(const Request& request,
                                  SimTime access_time,
                                  bool hit) const {
    (void)access_time;
    (void)hit;
    ++access_counts_[request.object_id];
    if (config_.history_mode == HotnessHistoryMode::Windowed) {
        ++epoch_access_counts_[request.epoch_id][request.object_id];
    }
}

void HotnessOnlyPolicy::on_epoch_start(EpochId epoch_id) const {
    if (config_.history_mode == HotnessHistoryMode::Epoch) {
        access_counts_.clear();
        epoch_access_counts_.clear();
        return;
    }

    if (config_.history_mode == HotnessHistoryMode::Windowed) {
        prune_window(epoch_id);
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

void HotnessOnlyPolicy::prune_window(EpochId epoch_id) const {
    std::vector<EpochId> epochs_to_remove;
    for (const auto& epoch_entry : epoch_access_counts_) {
        if (epoch_entry.first + config_.history_window_epochs <= epoch_id) {
            epochs_to_remove.push_back(epoch_entry.first);
        }
    }

    for (EpochId old_epoch : epochs_to_remove) {
        subtract_epoch_counts(epoch_access_counts_.at(old_epoch));
        epoch_access_counts_.erase(old_epoch);
    }
}

void HotnessOnlyPolicy::subtract_epoch_counts(
    const std::unordered_map<ObjectId, std::uint64_t>& epoch_counts) const {
    for (const auto& count_entry : epoch_counts) {
        auto total_it = access_counts_.find(count_entry.first);
        if (total_it == access_counts_.end()) {
            continue;
        }

        if (total_it->second <= count_entry.second) {
            access_counts_.erase(total_it);
            continue;
        }

        total_it->second -= count_entry.second;
    }
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
