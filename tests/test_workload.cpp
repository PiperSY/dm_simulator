#include <algorithm>
#include <cassert>
#include <cstddef>
#include <unordered_set>
#include <vector>

#include "workloads/generators.hpp"
#include "workloads/workload.hpp"

namespace {

using dm_sim::CrossNodeOverlap;
using dm_sim::EpochHotSetMetadata;
using dm_sim::GeneratedWorkload;
using dm_sim::HotSetMode;
using dm_sim::NodeId;
using dm_sim::NodeWorkload;
using dm_sim::ObjectId;
using dm_sim::RequestSpec;
using dm_sim::SyntheticWorkloadConfig;
using dm_sim::WorkloadCursor;

SyntheticWorkloadConfig make_config() {
    SyntheticWorkloadConfig config;
    config.seed = 1234;
    config.compute_node_ids = {1, 2};
    config.object_count = 64;
    config.object_size_bytes = 32;
    config.requests_per_node_per_epoch = 4;
    config.epoch_count = 3;
    config.hot_set_size = 4;
    config.hot_access_probability = 0.8;
    config.hot_set_mode = HotSetMode::Static;
    config.cross_node_overlap = CrossNodeOverlap::High;
    return config;
}

bool same_requests(const std::vector<RequestSpec>& lhs,
                   const std::vector<RequestSpec>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].object_id != rhs[i].object_id ||
            lhs[i].size_bytes != rhs[i].size_bytes ||
            lhs[i].epoch_id != rhs[i].epoch_id) {
            return false;
        }
    }

    return true;
}

bool same_generated_workload(const GeneratedWorkload& lhs,
                             const GeneratedWorkload& rhs) {
    if (lhs.node_workloads.size() != rhs.node_workloads.size() ||
        lhs.epochs.size() != rhs.epochs.size()) {
        return false;
    }

    for (std::size_t i = 0; i < lhs.node_workloads.size(); ++i) {
        if (lhs.node_workloads[i].node_id != rhs.node_workloads[i].node_id ||
            !same_requests(lhs.node_workloads[i].requests,
                           rhs.node_workloads[i].requests)) {
            return false;
        }
    }

    for (std::size_t i = 0; i < lhs.epochs.size(); ++i) {
        if (lhs.epochs[i].epoch_id != rhs.epochs[i].epoch_id ||
            lhs.epochs[i].hot_sets_by_node != rhs.epochs[i].hot_sets_by_node) {
            return false;
        }
    }

    return true;
}

std::size_t intersection_size(const std::vector<ObjectId>& lhs,
                              const std::vector<ObjectId>& rhs) {
    const std::unordered_set<ObjectId> lhs_lookup(lhs.begin(), lhs.end());
    std::size_t count = 0;
    for (ObjectId object_id : rhs) {
        if (lhs_lookup.find(object_id) != lhs_lookup.end()) {
            ++count;
        }
    }

    return count;
}

const std::vector<ObjectId>& hot_set_for_node(
    const EpochHotSetMetadata& epoch_metadata,
    NodeId node_id) {
    return epoch_metadata.hot_sets_by_node.at(node_id);
}

void test_same_seed_produces_identical_workloads() {
    const SyntheticWorkloadConfig config = make_config();

    const GeneratedWorkload first = generate_synthetic_workload(config);
    const GeneratedWorkload second = generate_synthetic_workload(config);

    assert(same_generated_workload(first, second));
}

void test_different_seed_can_change_workload() {
    SyntheticWorkloadConfig first_config = make_config();
    SyntheticWorkloadConfig second_config = make_config();
    second_config.seed = 9876;

    const GeneratedWorkload first = generate_synthetic_workload(first_config);
    const GeneratedWorkload second = generate_synthetic_workload(second_config);

    assert(!same_generated_workload(first, second));
}

void test_static_hot_sets_remain_stable_across_epochs() {
    SyntheticWorkloadConfig config = make_config();
    config.hot_set_mode = HotSetMode::Static;
    config.cross_node_overlap = CrossNodeOverlap::Low;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    for (NodeId node_id : config.compute_node_ids) {
        const std::vector<ObjectId>& first_epoch_hot_set =
            hot_set_for_node(workload.epochs.front(), node_id);
        for (const EpochHotSetMetadata& epoch_metadata : workload.epochs) {
            assert(hot_set_for_node(epoch_metadata, node_id) == first_epoch_hot_set);
        }
    }
}

void test_epoch_shift_changes_hot_sets() {
    SyntheticWorkloadConfig config = make_config();
    config.hot_set_mode = HotSetMode::EpochShift;
    config.cross_node_overlap = CrossNodeOverlap::Low;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    bool saw_change = false;
    for (NodeId node_id : config.compute_node_ids) {
        const std::vector<ObjectId>& first_epoch_hot_set =
            hot_set_for_node(workload.epochs.front(), node_id);
        for (std::size_t i = 1; i < workload.epochs.size(); ++i) {
            if (hot_set_for_node(workload.epochs[i], node_id) !=
                first_epoch_hot_set) {
                saw_change = true;
            }
        }
    }

    assert(saw_change);
}

void test_overlap_presets_shape_hot_sets() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 32;
    config.hot_set_size = 4;
    config.epoch_count = 1;
    config.hot_set_mode = HotSetMode::Static;

    config.cross_node_overlap = CrossNodeOverlap::High;
    const GeneratedWorkload high = generate_synthetic_workload(config);
    assert(hot_set_for_node(high.epochs.front(), 1) ==
           hot_set_for_node(high.epochs.front(), 2));

    config.cross_node_overlap = CrossNodeOverlap::Medium;
    const GeneratedWorkload medium = generate_synthetic_workload(config);
    assert(intersection_size(hot_set_for_node(medium.epochs.front(), 1),
                             hot_set_for_node(medium.epochs.front(), 2)) == 2);

    config.cross_node_overlap = CrossNodeOverlap::Low;
    const GeneratedWorkload low = generate_synthetic_workload(config);
    assert(intersection_size(hot_set_for_node(low.epochs.front(), 1),
                             hot_set_for_node(low.epochs.front(), 2)) == 0);
}

void test_generated_request_counts_and_epochs() {
    SyntheticWorkloadConfig config = make_config();
    config.requests_per_node_per_epoch = 5;
    config.epoch_count = 3;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    assert(workload.node_workloads.size() == config.compute_node_ids.size());
    for (const NodeWorkload& node_workload : workload.node_workloads) {
        assert(node_workload.requests.size() ==
               config.requests_per_node_per_epoch *
                   static_cast<std::size_t>(config.epoch_count));

        for (std::size_t i = 0; i < node_workload.requests.size(); ++i) {
            const std::size_t expected_epoch =
                i / config.requests_per_node_per_epoch;
            assert(node_workload.requests[i].epoch_id == expected_epoch);
            assert(node_workload.requests[i].size_bytes ==
                   config.object_size_bytes);
        }
    }
}

void test_workload_cursor_issues_requests_in_order() {
    std::vector<RequestSpec> requests{
        RequestSpec{101, 8, 0},
        RequestSpec{202, 8, 1},
    };
    WorkloadCursor cursor(requests);

    assert(cursor.total_count() == 2);
    assert(cursor.remaining_count() == 2);
    assert(cursor.has_next());

    const RequestSpec first = cursor.next();
    assert(first.object_id == 101);
    assert(first.epoch_id == 0);
    assert(cursor.issued_count() == 1);
    assert(cursor.remaining_count() == 1);

    const RequestSpec second = cursor.next();
    assert(second.object_id == 202);
    assert(second.epoch_id == 1);
    assert(cursor.issued_count() == 2);
    assert(!cursor.has_next());
}

}  // namespace

int main() {
    test_same_seed_produces_identical_workloads();
    test_different_seed_can_change_workload();
    test_static_hot_sets_remain_stable_across_epochs();
    test_epoch_shift_changes_hot_sets();
    test_overlap_presets_shape_hot_sets();
    test_generated_request_counts_and_epochs();
    test_workload_cursor_issues_requests_in_order();
    return 0;
}
