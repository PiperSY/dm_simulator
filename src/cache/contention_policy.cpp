#include "cache/contention_policy.hpp"

#include <algorithm>
#include <cmath>

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
    // The ComputeNode normally announces epoch changes before lookup, but this
    // keeps callers from using stale previous-epoch snapshots.
    if (!current_epoch_.has_value() || *current_epoch_ != request.epoch_id) {
        on_epoch_start(request.epoch_id);
    }

    // Count both hits and misses: either one implies intent for object.
    ++local_access_counts_[request.object_id];
}

void ContentionAwarePolicy::on_epoch_start(EpochId epoch_id) const {
    // Avoid rebuilding snapshots if multiple lookups arrive within the same epoch.
    if (current_epoch_.has_value() && *current_epoch_ == epoch_id) {
        return;
    }

    current_epoch_ = epoch_id;
    if (config_.reset_on_epoch_change) {
        local_access_counts_.clear();
    }

    previous_epoch_stats_.clear();
    maxima_ = NormalizationMaxima{};

    // Snapshot prior telemetry once at epoch start. V1 and most variants use
    // only epoch N-1. The smoothed variant optionally blends several prior
    // epochs using decay^age so older data helps only when configured.
    const std::uint64_t history_epochs =
        config_.variant == ContentionPolicyVariant::Smoothed
            ? config_.telemetry_history_epochs
            : 1;
    for (std::uint64_t age = 0; age < history_epochs; ++age) {
        if (epoch_id <= age) {
            break;
        }

        const EpochId source_epoch =
            epoch_id - static_cast<EpochId>(age) - 1;
        const double weight = std::pow(config_.telemetry_decay,
                                       static_cast<double>(age));
        for (const ObjectContentionStats& object_stats :
             stats_.contention_by_epoch(source_epoch)) {
            add_contention_snapshot(object_stats, weight);
        }
    }
    refresh_normalization_maxima();
}

void ContentionAwarePolicy::on_access(CacheEntry& entry,
                                      SimTime access_time) const {
    entry.last_access_time = access_time;
}

bool ContentionAwarePolicy::should_admit(const Request& request,
                                         const Response& response) const {
    (void)response;
    // Reuse-gated mode prevents a globally painful object from entering this
    // node's private cache until this node has shown enough local demand.
    if (config_.variant == ContentionPolicyVariant::ReuseGated &&
        count_for(request.object_id) < config_.local_reuse_gate_threshold) {
        return false;
    }

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
    // object is better than the weakest resident. Hysteresis raises that bar
    // by a configurable margin to avoid borderline admit/evict oscillation.
    const double required_margin =
        config_.variant == ContentionPolicyVariant::Hysteresis
            ? config_.eviction_score_margin
            : 0.0;
    if (incoming_score.total_score <=
        victim_score.total_score + required_margin) {
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
    record.policy_variant = variant_label();
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
        const ScoringContentionStats& previous = previous_it->second;
        // These are previous-epoch global-pressure signals. Missing objects
        // keep zeroes, which prevents same-epoch contention from leaking into
        // the current admission decision.
        score.remote_accesses =
            normalized(previous.remote_accesses, maxima_.remote_accesses);
        score.distinct_requesters = normalized(previous.distinct_requesters,
                                              maxima_.distinct_requesters);
        score.queue_wait =
            normalized(previous.total_queue_wait, maxima_.total_queue_wait);
        score.remote_service_time = normalized(
            previous.total_remote_service_time,
            maxima_.total_remote_service_time);
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

void ContentionAwarePolicy::add_contention_snapshot(
    const ObjectContentionStats& object_stats,
    double weight) const {
    ScoringContentionStats& aggregate =
        previous_epoch_stats_[object_stats.object_id];
    aggregate.remote_accesses +=
        static_cast<double>(object_stats.remote_accesses) * weight;
    aggregate.distinct_requesters +=
        static_cast<double>(object_stats.distinct_requesters) * weight;
    aggregate.total_queue_wait +=
        static_cast<double>(object_stats.total_queue_wait) * weight;
    aggregate.total_remote_service_time +=
        static_cast<double>(object_stats.total_remote_service_time) * weight;
}

void ContentionAwarePolicy::refresh_normalization_maxima() const {
    for (const auto& entry : previous_epoch_stats_) {
        const ScoringContentionStats& stats = entry.second;
        maxima_.remote_accesses =
            std::max(maxima_.remote_accesses, stats.remote_accesses);
        maxima_.distinct_requesters =
            std::max(maxima_.distinct_requesters, stats.distinct_requesters);
        maxima_.total_queue_wait =
            std::max(maxima_.total_queue_wait, stats.total_queue_wait);
        maxima_.total_remote_service_time =
            std::max(maxima_.total_remote_service_time,
                     stats.total_remote_service_time);
    }
}

std::string ContentionAwarePolicy::variant_label() const {
    switch (config_.variant) {
    case ContentionPolicyVariant::V1:
        return "v1";
    case ContentionPolicyVariant::Smoothed:
        return "smoothed";
    case ContentionPolicyVariant::ReuseGated:
        return "reuse_gated";
    case ContentionPolicyVariant::Hysteresis:
        return "hysteresis";
    }

    return "unknown";
}

}  // namespace dm_sim
