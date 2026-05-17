#include <cassert>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>

#include "cache/cache_policy.hpp"
#include "cache/contention_policy.hpp"
#include "cache/hotness_policy.hpp"
#include "cache/local_cache.hpp"
#include "cache/lru_policy.hpp"
#include "metrics/stats.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace {

using dm_sim::AlwaysRemotePolicy;
using dm_sim::CacheAdmissionRecord;
using dm_sim::CacheReplica;
using dm_sim::CacheEntry;
using dm_sim::ContentionAwarePolicy;
using dm_sim::ContentionPolicyConfig;
using dm_sim::GlobalHottestReplicationPolicy;
using dm_sim::HotnessOnlyPolicy;
using dm_sim::HotnessPolicyConfig;
using dm_sim::LocalCache;
using dm_sim::LruPolicy;
using dm_sim::ObjectId;
using dm_sim::PolicyDecisionRecord;
using dm_sim::Request;
using dm_sim::Response;
using dm_sim::ServedFromTier;
using dm_sim::Stats;

Request make_request(dm_sim::RequestId request_id,
                     ObjectId object_id,
                     std::uint64_t size_bytes,
                     dm_sim::EpochId epoch_id = 0,
                     dm_sim::NodeId source_node_id = 1) {
    Request request;
    request.request_id = request_id;
    request.source_node_id = source_node_id;
    request.object_id = object_id;
    request.size_bytes = size_bytes;
    request.epoch_id = epoch_id;
    return request;
}

Response make_response(dm_sim::RequestId request_id, ObjectId object_id) {
    Response response;
    response.request_id = request_id;
    response.object_id = object_id;
    response.served_from_tier = ServedFromTier::Memory;
    return response;
}

HotnessPolicyConfig hotness_config(std::uint64_t min_admit_count = 2,
                                   bool reset_on_epoch_change = true) {
    HotnessPolicyConfig config;
    config.min_admit_count = min_admit_count;
    config.reset_on_epoch_change = reset_on_epoch_change;
    return config;
}

ContentionPolicyConfig contention_config(double min_admit_score = 1.0) {
    ContentionPolicyConfig config;
    config.weights.local_hotness_weight = 0.0;
    config.weights.remote_access_weight = 0.0;
    config.weights.distinct_requester_weight = 0.0;
    config.weights.queue_wait_weight = 0.0;
    config.weights.remote_service_time_weight = 0.0;
    config.weights.size_penalty_weight = 0.0;
    config.min_admit_score = min_admit_score;
    config.local_hotness_threshold = 2;
    return config;
}

void record_remote_object(Stats& stats,
                          ObjectId object_id,
                          std::uint64_t accesses,
                          dm_sim::SimTime queue_wait,
                          dm_sim::SimTime service_time) {
    for (std::uint64_t i = 0; i < accesses; ++i) {
        const Request request = make_request(
            i + 1,
            object_id,
            8,
            0,
            static_cast<dm_sim::NodeId>(i + 1));
        stats.record_remote_access(request, static_cast<std::size_t>(i + 1));
        stats.record_object_queue_wait(request, queue_wait);
        stats.record_object_service(request, service_time);
    }
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) < 1e-9;
}

void test_lookup_insert_then_hit() {
    LocalCache cache(32, std::make_unique<LruPolicy>());

    assert(!cache.lookup(1001, 1));
    assert(cache.miss_count() == 1);

    const Request request = make_request(1, 1001, 16);
    const Response response = make_response(1, 1001);
    assert(cache.admit(request, response, 5));

    assert(cache.contains(1001));
    assert(cache.lookup(1001, 6));
    assert(cache.hit_count() == 1);
    assert(cache.occupancy_bytes() == 16);
    assert(cache.bytes_admitted() == 16);

    const std::vector<CacheAdmissionRecord>& admissions =
        cache.cache_admission_diagnostics();
    assert(admissions.size() == 1);
    assert(admissions[0].admitted);
    assert(admissions[0].future_hit_count == 1);
    assert(admissions[0].reused_after_admit);
    assert(admissions[0].placement_source == "remote_response");
}

void test_byte_capacity_and_lru_eviction() {
    LocalCache cache(16, std::make_unique<LruPolicy>());

    assert(cache.admit(make_request(1, 1001, 8), make_response(1, 1001), 1));
    assert(cache.admit(make_request(2, 1002, 8), make_response(2, 1002), 2));
    assert(cache.lookup(1001, 3));
    assert(cache.admit(make_request(3, 1003, 8), make_response(3, 1003), 4));

    assert(cache.contains(1001));
    assert(!cache.contains(1002));
    assert(cache.contains(1003));
    assert(cache.occupancy_bytes() == 16);
    assert(cache.bytes_evicted() == 8);

    const std::vector<CacheAdmissionRecord>& admissions =
        cache.cache_admission_diagnostics();
    assert(admissions.size() == 3);
    assert(admissions[0].object_id == 1001);
    assert(admissions[0].future_hit_count == 1);
    assert(admissions[0].evicted == false);
    assert(admissions[1].object_id == 1002);
    assert(admissions[1].evicted);
    assert(admissions[1].eviction_time == 4);
    assert(admissions[2].evicted_objects.size() == 1);
    assert(admissions[2].evicted_objects[0] == 1002);
}

void test_always_remote_never_admits() {
    LocalCache cache(64, std::make_unique<AlwaysRemotePolicy>());

    assert(!cache.admit(make_request(1, 2001, 16), make_response(1, 2001), 1));
    assert(!cache.contains(2001));
    assert(cache.entry_count() == 0);
    assert(cache.occupancy_bytes() == 0);

    const std::vector<CacheAdmissionRecord>& admissions =
        cache.cache_admission_diagnostics();
    assert(admissions.size() == 1);
    assert(!admissions[0].admitted);
    assert(admissions[0].reason == "policy_rejected");
}

void test_object_larger_than_capacity_is_rejected() {
    LocalCache cache(8, std::make_unique<LruPolicy>());

    assert(!cache.admit(make_request(1, 3001, 16), make_response(1, 3001), 1));
    assert(cache.entry_count() == 0);
    const std::vector<CacheAdmissionRecord>& admissions =
        cache.cache_admission_diagnostics();
    assert(admissions.size() == 1);
    assert(!admissions[0].admitted);
    assert(admissions[0].reason == "object_too_large");
}

void test_hotness_policy_counts_hits_and_misses() {
    HotnessOnlyPolicy policy(hotness_config());
    const Request request = make_request(1, 7001, 8);

    policy.on_lookup(request, 1, false);
    policy.on_lookup(request, 2, true);

    assert(policy.access_count(7001) == 2);
}

void test_hotness_policy_waits_for_admit_threshold() {
    LocalCache cache(16, std::make_unique<HotnessOnlyPolicy>(hotness_config()));
    const Request request = make_request(1, 7101, 8);
    const Response response = make_response(1, 7101);

    assert(!cache.lookup(request, 1));
    assert(!cache.admit(request, response, 2));
    assert(!cache.contains(7101));

    assert(!cache.lookup(request, 3));
    assert(cache.admit(request, response, 4));
    assert(cache.contains(7101));
}

void test_hotness_policy_evicts_coldest_resident() {
    LocalCache cache(16, std::make_unique<HotnessOnlyPolicy>(hotness_config()));

    const Request warm = make_request(1, 7201, 8);
    const Response warm_response = make_response(1, 7201);
    assert(!cache.lookup(warm, 1));
    assert(!cache.lookup(warm, 2));
    assert(cache.admit(warm, warm_response, 3));
    assert(cache.lookup(warm, 4));

    const Request cold = make_request(2, 7202, 8);
    const Response cold_response = make_response(2, 7202);
    assert(!cache.lookup(cold, 5));
    assert(!cache.lookup(cold, 6));
    assert(cache.admit(cold, cold_response, 7));

    const Request incoming = make_request(3, 7203, 8);
    const Response incoming_response = make_response(3, 7203);
    assert(!cache.lookup(incoming, 8));
    assert(!cache.lookup(incoming, 9));
    assert(!cache.lookup(incoming, 10));
    assert(cache.admit(incoming, incoming_response, 11));

    assert(cache.contains(7201));
    assert(!cache.contains(7202));
    assert(cache.contains(7203));
}

void test_hotness_policy_resets_scores_on_epoch_change() {
    HotnessOnlyPolicy policy(hotness_config());
    const Request request = make_request(1, 7301, 8);

    policy.on_lookup(request, 1, false);
    policy.on_lookup(request, 2, false);
    assert(policy.access_count(7301) == 2);

    policy.on_epoch_start(1);
    assert(policy.access_count(7301) == 0);
}

void test_global_replica_installation_respects_capacity_and_replaces_entries() {
    LocalCache cache(16, std::make_unique<GlobalHottestReplicationPolicy>());

    cache.install_replicas(
        {
            CacheReplica{8001, 8},
            CacheReplica{8002, 8},
            CacheReplica{8003, 8},
        },
        1);

    assert(cache.contains(8001));
    assert(cache.contains(8002));
    assert(!cache.contains(8003));
    assert(cache.entry_count() == 2);
    assert(cache.occupancy_bytes() == 16);
    assert(cache.hit_count() == 0);
    assert(cache.miss_count() == 0);
    assert(cache.bytes_admitted() == 16);

    const std::vector<CacheAdmissionRecord>& first_admissions =
        cache.cache_admission_diagnostics();
    assert(first_admissions.size() == 2);
    assert(first_admissions[0].placement_source == "global_replica");

    cache.install_replicas({CacheReplica{9001, 8}}, 2, 7, 1);
    assert(!cache.contains(8001));
    assert(!cache.contains(8002));
    assert(cache.contains(9001));
    assert(cache.entry_count() == 1);
    assert(cache.occupancy_bytes() == 8);
    assert(cache.bytes_evicted() == 16);

    const std::vector<CacheAdmissionRecord>& admissions =
        cache.cache_admission_diagnostics();
    assert(admissions.size() == 3);
    assert(admissions[0].evicted);
    assert(admissions[0].eviction_time == 2);
    assert(admissions[2].node_id == 7);
    assert(admissions[2].epoch_id == 1);
    assert(admissions[2].object_id == 9001);
    assert(admissions[2].placement_source == "global_replica");
}

void test_contention_policy_uses_local_hotness_for_epoch_zero() {
    Stats stats;
    ContentionPolicyConfig config = contention_config(1.0);
    config.weights.local_hotness_weight = 1.0;

    LocalCache cache(
        16,
        std::make_unique<ContentionAwarePolicy>(config, 16, stats));

    const Request first = make_request(1, 9101, 8, 0);
    assert(!cache.lookup(first, 1));
    assert(!cache.admit(first, make_response(1, 9101), 2));

    const Request second = make_request(2, 9101, 8, 0);
    assert(!cache.lookup(second, 3));
    assert(cache.admit(second, make_response(2, 9101), 4));
    assert(cache.contains(9101));

    const std::vector<PolicyDecisionRecord> diagnostics =
        cache.policy_diagnostics();
    assert(diagnostics.size() == 2);
    assert(!diagnostics[0].admitted);
    assert(diagnostics[1].admitted);
    assert(near(diagnostics[0].score.total_score, 0.5));
    assert(near(diagnostics[1].score.total_score, 1.0));
}

void test_contention_policy_scores_previous_epoch_contention() {
    Stats stats;
    record_remote_object(stats, 9201, 4, 6, 8);
    record_remote_object(stats, 9202, 1, 1, 2);

    ContentionPolicyConfig config = contention_config(0.0);
    config.weights.remote_access_weight = 1.0;
    config.weights.distinct_requester_weight = 1.0;
    config.weights.queue_wait_weight = 1.0;
    config.weights.remote_service_time_weight = 1.0;

    ContentionAwarePolicy policy(config, 64, stats);
    policy.on_epoch_start(1);

    const auto hot_score = policy.score_object(9201, 8);
    const auto cold_score = policy.score_object(9202, 8);
    assert(near(hot_score.remote_accesses, 1.0));
    assert(near(hot_score.queue_wait, 1.0));
    assert(hot_score.total_score > cold_score.total_score);
}

void test_contention_policy_evicts_lowest_scored_resident() {
    Stats stats;
    record_remote_object(stats, 9301, 1, 1, 1);
    record_remote_object(stats, 9302, 2, 1, 1);
    record_remote_object(stats, 9303, 4, 1, 1);

    ContentionPolicyConfig config = contention_config(0.0);
    config.weights.remote_access_weight = 1.0;

    ContentionAwarePolicy policy(config, 16, stats);
    policy.on_epoch_start(1);

    std::unordered_map<ObjectId, CacheEntry> entries;
    entries.emplace(9301, CacheEntry{9301, 8, 1, 1});
    entries.emplace(9302, CacheEntry{9302, 8, 2, 2});

    const Request strong_incoming = make_request(1, 9303, 8, 1);
    const std::optional<ObjectId> victim =
        policy.select_victim(entries, strong_incoming);
    assert(victim.has_value());
    assert(*victim == 9301);

    const Request weak_incoming = make_request(2, 9304, 8, 1);
    assert(!policy.select_victim(entries, weak_incoming).has_value());
}

}  // namespace

int main() {
    test_lookup_insert_then_hit();
    test_byte_capacity_and_lru_eviction();
    test_always_remote_never_admits();
    test_object_larger_than_capacity_is_rejected();
    test_hotness_policy_counts_hits_and_misses();
    test_hotness_policy_waits_for_admit_threshold();
    test_hotness_policy_evicts_coldest_resident();
    test_hotness_policy_resets_scores_on_epoch_change();
    test_global_replica_installation_respects_capacity_and_replaces_entries();
    test_contention_policy_uses_local_hotness_for_epoch_zero();
    test_contention_policy_scores_previous_epoch_contention();
    test_contention_policy_evicts_lowest_scored_resident();
    return 0;
}
