#include <cassert>
#include <memory>

#include "cache/cache_policy.hpp"
#include "cache/local_cache.hpp"
#include "cache/lru_policy.hpp"
#include "model/request.hpp"
#include "model/response.hpp"

namespace {

using dm_sim::AlwaysRemotePolicy;
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

}  // namespace

int main() {
    test_lookup_insert_then_hit();
    test_byte_capacity_and_lru_eviction();
    test_always_remote_never_admits();
    test_object_larger_than_capacity_is_rejected();
    return 0;
}
