#include "cache/contention_policy.hpp"

#include <algorithm>

namespace dm_sim {

ContentionAwarePolicy::ContentionAwarePolicy(
    ContentionPolicyConfig config,
    std::uint64_t cache_capacity_bytes,
    const Stats& stats)
    : config_(config),
      cache_capacity_bytes_(cache_capacity_bytes),
      stats_(stats) {}

void ContentionAwarePolicy::on_lookup(const Request& request,
                                      SimTime access_time,
                                      bool hit) const {
    (void)access_time;
    (void)hit;
    // The ComputeNode normally announces epoch changes before lookup, but this keepss 
    // callers from using stale previous-epoch snapshots.
    if (!current_epoch_.has_value() || *current_epoch_ != request.epoch_id) {
        on_epoch_start(request.epoch_id);
    }

    // Count both hits and misses: either one implies intent for object.
    ++local_access_counts_[request.object_id];
}

void ContentionAwarePolicy::on_epoch_start(EpochId epoch_id) const {
    // Avoid unnecessary work if the epoch ID is unchanged. This can happen if the ComputeNode fails to announce an epoch change or if multiple lookups occur before the first announcement.
    if (current_epoch_.has_value() && *current_epoch_ == epoch_id) {
        return;
    }

    // Update current epoch and reset local access counts if configured to do so. 
    //This ensures that the policy's internal state is correctly aligned with the current epoch and that local hotness calculations reflect only accesses within the current epoch if reset_on_epoch_change is true.
    current_epoch_ = epoch_id;
    if (config_.reset_on_epoch_change) {
        local_access_counts_.clear();
    }

    previous_epoch_stats_.clear();
    maxima_ = NormalizationMaxima{};

    // Snapshot once at epoch start. This is the realism boundary for Phase 8:
    // current-epoch remote misses update Stats, but they cannot affect cache
    // decisions until the next epoch.
    for (const ObjectContentionStats& object_stats :
         stats_.previous_epoch_contention(epoch_id)) {
        previous_epoch_stats_[object_stats.object_id] = object_stats;
        // Track per-signal maxima so each raw metric can be converted to a
        // portable 0..1 value before applying weights.
        maxima_.remote_accesses =
            std::max(maxima_.remote_accesses, object_stats.remote_accesses);
        maxima_.distinct_requesters = std::max(maxima_.distinct_requesters,
                                               object_stats.distinct_requesters);
        maxima_.total_queue_wait = std::max(maxima_.total_queue_wait,
                                            object_stats.total_queue_wait);
        maxima_.total_remote_service_time =
            std::max(maxima_.total_remote_service_time,
                     object_stats.total_remote_service_time);
    }
}

void ContentionAwarePolicy::on_access(CacheEntry& entry,
                                      SimTime access_time) const {
    entry.last_access_time = access_time;
}

bool ContentionAwarePolicy::should_admit(const Request& request,
                                         const Response& response) const {
    (void)response;
    return score_object(request.object_id, request.size_bytes).total_score >=
           config_.min_admit_score;
}

std::optional<ObjectId> ContentionAwarePolicy::select_victim(
    const std::unordered_map<ObjectId, CacheEntry>& entries,
    const Request& incoming_request) const {
    if (entries.empty()) {
        return std::nullopt;
    }

    auto victim = entries.begin();
    ContentionScoreComponents victim_score =
        score_object(victim->first, victim->second.size_bytes);

    // Lower score means lower expected contention relief if kept local. Ties
    // fall back to LRU-style age and then object ID so eviction is repeatable.
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const ContentionScoreComponents candidate_score =
            score_object(it->first, it->second.size_bytes);

        if (candidate_score.total_score < victim_score.total_score) {
            victim = it;
            victim_score = candidate_score;
            continue;
        }

        if (candidate_score.total_score == victim_score.total_score &&
            it->second.last_access_time < victim->second.last_access_time) {
            victim = it;
            victim_score = candidate_score;
            continue;
        }

        if (candidate_score.total_score == victim_score.total_score &&
            it->second.last_access_time == victim->second.last_access_time &&
            it->second.object_id < victim->second.object_id) {
            victim = it;
            victim_score = candidate_score;
        }
    }

    const ContentionScoreComponents incoming_score =
        score_object(incoming_request.object_id, incoming_request.size_bytes);
    // Avoid churn: replacing a resident is only worthwhile if the incoming
    // object is strictly better than the weakest object already cached.
    if (incoming_score.total_score <= victim_score.total_score) {
        return std::nullopt;
    }

    return victim->first;
}

void ContentionAwarePolicy::on_admission_result(
    const Request& request,
    const Response& response,
    SimTime access_time,
    bool admitted,
    const std::string& reason,
    const std::vector<ObjectId>& evicted_objects) const {
    (void)response;
    (void)access_time;

    // Diagnostics record the final LocalCache outcome, not just the policy's
    // initial score check, so capacity and victim-selection failures are
    // visible in experiment output.
    PolicyDecisionRecord record;
    record.node_id = request.source_node_id;
    record.epoch_id = request.epoch_id;
    record.request_id = request.request_id;
    record.object_id = request.object_id;
    record.admitted = admitted;
    record.reason = reason;
    record.score = score_object(request.object_id, request.size_bytes);
    record.evicted_objects = evicted_objects;
    diagnostics_.push_back(record);
}

std::vector<PolicyDecisionRecord> ContentionAwarePolicy::diagnostics() const {
    return diagnostics_;
}

ContentionScoreComponents ContentionAwarePolicy::score_object(
    ObjectId object_id,
    std::uint64_t size_bytes) const {
    ContentionScoreComponents score;
    const ContentionPolicyWeights& weights = config_.weights;

    // Local hotness is the cold-start signal. It reaches 1.0 once the object
    // has been seen local_hotness_threshold times in this epoch.
    score.local_hotness = normalized(
        static_cast<double>(count_for(object_id)),
        static_cast<double>(config_.local_hotness_threshold));

    const auto previous_it = previous_epoch_stats_.find(object_id);
    if (previous_it != previous_epoch_stats_.end()) {
        const ObjectContentionStats& previous = previous_it->second;
        // These are previous-epoch global-pressure signals. Missing objects
        // keep zeroes, which prevents same-epoch contention from leaking into
        // the current admission decision.
        score.remote_accesses = normalized(
            static_cast<double>(previous.remote_accesses),
            static_cast<double>(maxima_.remote_accesses));
        score.distinct_requesters = normalized(
            static_cast<double>(previous.distinct_requesters),
            static_cast<double>(maxima_.distinct_requesters));
        score.queue_wait = normalized(
            static_cast<double>(previous.total_queue_wait),
            static_cast<double>(maxima_.total_queue_wait));
        score.remote_service_time = normalized(
            static_cast<double>(previous.total_remote_service_time),
            static_cast<double>(maxima_.total_remote_service_time));
    }

    // Penalize objects by the fraction of local cache they would consume. A
    // zero-capacity cache treats every object as maximally expensive; the
    // LocalCache capacity check still performs the final rejection.
    if (cache_capacity_bytes_ == 0) {
        score.size_penalty = 1.0;
    } else {
        score.size_penalty = std::min(
            static_cast<double>(size_bytes) /
                static_cast<double>(cache_capacity_bytes_),
            1.0);
    }

    // Benefit signals add pressure-relief value; size subtracts opportunity
    // cost so a large object must justify occupying more of the cache.
    score.total_score =
        weights.local_hotness_weight * score.local_hotness +
        weights.remote_access_weight * score.remote_accesses +
        weights.distinct_requester_weight * score.distinct_requesters +
        weights.queue_wait_weight * score.queue_wait +
        weights.remote_service_time_weight * score.remote_service_time -
        weights.size_penalty_weight * score.size_penalty;

    return score;
}

std::uint64_t ContentionAwarePolicy::local_access_count(
    ObjectId object_id) const {
    return count_for(object_id);
}

double ContentionAwarePolicy::normalized(double value, double max_value) const {
    // If no object had a nonzero value for a signal last epoch, that signal
    // contributes nothing this epoch instead of producing NaN or infinity.
    if (max_value <= 0.0) {
        return 0.0;
    }

    return std::min(value / max_value, 1.0);
}

std::uint64_t ContentionAwarePolicy::count_for(ObjectId object_id) const {
    const auto it = local_access_counts_.find(object_id);
    if (it == local_access_counts_.end()) {
        return 0;
    }

    return it->second;
}

}  // namespace dm_sim
