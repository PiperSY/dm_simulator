#include <cassert>
#include <memory>

#include "cache/cache_policy.hpp"
#include "cache/hotness_policy.hpp"
#include "cache/local_cache.hpp"
#include "cache/lru_policy.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace {

using dm_sim::AlwaysRemotePolicy;
using dm_sim::CacheReplica;
using dm_sim::GlobalHottestReplicationPolicy;
using dm_sim::HotnessOnlyPolicy;
using dm_sim::HotnessPolicyConfig;
using dm_sim::LocalCache;
using dm_sim::LruPolicy;
using dm_sim::ObjectId;
using dm_sim::Request;
using dm_sim::Response;
using dm_sim::ServedFromTier;

Request make_request(dm_sim::RequestId request_id,
                     ObjectId object_id,
                     std::uint64_t size_bytes) {
    Request request;
    request.request_id = request_id;
    request.source_node_id = 1;
    request.object_id = object_id;
    request.size_bytes = size_bytes;
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
}

void test_always_remote_never_admits() {
    LocalCache cache(64, std::make_unique<AlwaysRemotePolicy>());

    assert(!cache.admit(make_request(1, 2001, 16), make_response(1, 2001), 1));
    assert(!cache.contains(2001));
    assert(cache.entry_count() == 0);
    assert(cache.occupancy_bytes() == 0);
}

void test_object_larger_than_capacity_is_rejected() {
    LocalCache cache(8, std::make_unique<LruPolicy>());

    assert(!cache.admit(make_request(1, 3001, 16), make_response(1, 3001), 1));
    assert(cache.entry_count() == 0);
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

    cache.install_replicas({CacheReplica{9001, 8}}, 2);
    assert(!cache.contains(8001));
    assert(!cache.contains(8002));
    assert(cache.contains(9001));
    assert(cache.entry_count() == 1);
    assert(cache.occupancy_bytes() == 8);
    assert(cache.bytes_evicted() == 16);
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
    return 0;
}
