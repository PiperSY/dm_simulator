#include "cache/lru_policy.hpp"

namespace dm_sim {

void CachePolicy::on_lookup(const Request& request,
                            SimTime access_time,
                            bool hit) const {
    (void)request;
    (void)access_time;
    (void)hit;
}

void CachePolicy::on_epoch_start(EpochId epoch_id) const {
    (void)epoch_id;
}

AdmissionDecision CachePolicy::admission_decision(
    const Request& request,
    const Response& response) const {
    return should_admit(request, response)
               ? AdmissionDecision{true, "admitted"}
               : AdmissionDecision{false, "policy_rejected"};
}

void CachePolicy::on_admission_result(
    const Request& request,
    const Response& response,
    SimTime access_time,
    bool admitted,
    const std::string& reason,
    const std::vector<ObjectId>& evicted_objects) const {
    (void)request;
    (void)response;
    (void)access_time;
    (void)admitted;
    (void)reason;
    (void)evicted_objects;
}

std::vector<PolicyDecisionRecord> CachePolicy::diagnostics() const {
    return {};
}

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
    const std::unordered_map<ObjectId, CacheEntry>& entries,
    const Request& incoming_request) const {
    (void)entries;
    (void)incoming_request;
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
    const std::unordered_map<ObjectId, CacheEntry>& entries,
    const Request& incoming_request) const {
    (void)incoming_request;

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
