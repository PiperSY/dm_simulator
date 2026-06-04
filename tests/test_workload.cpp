#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
using dm_sim::ObjectSizeMode;
using dm_sim::RequestSpec;
using dm_sim::SyntheticWorkloadConfig;
using dm_sim::WorkloadIssueMode;
using dm_sim::WorkloadCursor;
using dm_sim::memory_channel_for_object;

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
            lhs[i].epoch_id != rhs[i].epoch_id ||
            lhs[i].scheduled_issue_offset !=
                rhs[i].scheduled_issue_offset) {
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

std::size_t positional_difference_count(const std::vector<ObjectId>& lhs,
                                        const std::vector<ObjectId>& rhs) {
    assert(lhs.size() == rhs.size());

    std::size_t count = 0;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i]) {
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

void test_epoch_shift_zero_churn_keeps_hot_sets_stable() {
    SyntheticWorkloadConfig config = make_config();
    config.hot_set_mode = HotSetMode::EpochShift;
    config.hot_set_churn_fraction = 0.0;
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

void test_epoch_shift_full_churn_replaces_hot_sets() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 64;
    config.hot_set_size = 4;
    config.hot_set_mode = HotSetMode::EpochShift;
    config.hot_set_churn_fraction = 1.0;
    config.cross_node_overlap = CrossNodeOverlap::Low;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    for (NodeId node_id : config.compute_node_ids) {
        assert(intersection_size(hot_set_for_node(workload.epochs[0], node_id),
                                 hot_set_for_node(workload.epochs[1], node_id)) ==
               0);
    }
}

void test_partial_churn_changes_configured_fraction_of_segments() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 64;
    config.hot_set_size = 4;
    config.hot_set_mode = HotSetMode::EpochShift;
    config.hot_set_churn_fraction = 0.5;
    config.cross_node_overlap = CrossNodeOverlap::Medium;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    const std::vector<ObjectId>& node_one_epoch_zero =
        hot_set_for_node(workload.epochs[0], 1);
    const std::vector<ObjectId>& node_one_epoch_one =
        hot_set_for_node(workload.epochs[1], 1);
    assert(positional_difference_count(node_one_epoch_zero,
                                       node_one_epoch_one) == 2);
    assert(intersection_size(node_one_epoch_zero, node_one_epoch_one) == 2);
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

void test_partial_churn_preserves_overlap_presets() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 64;
    config.hot_set_size = 4;
    config.epoch_count = 3;
    config.hot_set_mode = HotSetMode::EpochShift;
    config.hot_set_churn_fraction = 0.5;

    config.cross_node_overlap = CrossNodeOverlap::High;
    const GeneratedWorkload high = generate_synthetic_workload(config);
    for (const EpochHotSetMetadata& epoch_metadata : high.epochs) {
        assert(intersection_size(hot_set_for_node(epoch_metadata, 1),
                                 hot_set_for_node(epoch_metadata, 2)) == 4);
    }

    config.cross_node_overlap = CrossNodeOverlap::Medium;
    const GeneratedWorkload medium = generate_synthetic_workload(config);
    for (const EpochHotSetMetadata& epoch_metadata : medium.epochs) {
        assert(intersection_size(hot_set_for_node(epoch_metadata, 1),
                                 hot_set_for_node(epoch_metadata, 2)) == 2);
    }

    config.cross_node_overlap = CrossNodeOverlap::Low;
    const GeneratedWorkload low = generate_synthetic_workload(config);
    for (const EpochHotSetMetadata& epoch_metadata : low.epochs) {
        assert(intersection_size(hot_set_for_node(epoch_metadata, 1),
                                 hot_set_for_node(epoch_metadata, 2)) == 0);
    }
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
            assert(node_workload.requests[i].scheduled_issue_offset == 0);
        }
    }
}

void test_scheduled_bursty_offsets_are_deterministic_and_ordered() {
    SyntheticWorkloadConfig config = make_config();
    config.issue_mode = WorkloadIssueMode::ScheduledBursty;
    config.requests_per_node_per_epoch = 6;
    config.epoch_count = 2;
    config.burst_size = 3;
    config.burst_interval = 20;
    config.intra_burst_gap = 2;
    config.node_phase_jitter = 0;

    const GeneratedWorkload first = generate_synthetic_workload(config);
    const GeneratedWorkload second = generate_synthetic_workload(config);
    assert(same_generated_workload(first, second));

    for (const NodeWorkload& node_workload : first.node_workloads) {
        for (std::size_t i = 0; i < node_workload.requests.size(); ++i) {
            const RequestSpec& request = node_workload.requests[i];
            const std::size_t request_in_epoch =
                i % config.requests_per_node_per_epoch;
            const dm_sim::SimTime expected_offset =
                (request_in_epoch / config.burst_size) *
                    config.burst_interval +
                (request_in_epoch % config.burst_size) *
                    config.intra_burst_gap;
            assert(request.scheduled_issue_offset == expected_offset);

            if (request_in_epoch > 0) {
                const RequestSpec& previous = node_workload.requests[i - 1];
                assert(previous.epoch_id == request.epoch_id);
                assert(previous.scheduled_issue_offset <=
                       request.scheduled_issue_offset);
            }
        }
    }
}

void test_scheduled_bursty_jitter_can_change_with_seed() {
    SyntheticWorkloadConfig first_config = make_config();
    first_config.issue_mode = WorkloadIssueMode::ScheduledBursty;
    first_config.node_phase_jitter = 8;

    const GeneratedWorkload first = generate_synthetic_workload(first_config);

    bool saw_different_offset = false;
    for (std::uint64_t seed_delta = 1;
         seed_delta <= 20 && !saw_different_offset;
         ++seed_delta) {
        SyntheticWorkloadConfig second_config = first_config;
        second_config.seed = first_config.seed + seed_delta;
        const GeneratedWorkload second =
            generate_synthetic_workload(second_config);

        for (std::size_t node_index = 0;
             node_index < first.node_workloads.size();
             ++node_index) {
            const std::vector<RequestSpec>& lhs =
                first.node_workloads[node_index].requests;
            const std::vector<RequestSpec>& rhs =
                second.node_workloads[node_index].requests;
            for (std::size_t i = 0; i < lhs.size(); ++i) {
                if (lhs[i].scheduled_issue_offset !=
                    rhs[i].scheduled_issue_offset) {
                    saw_different_offset = true;
                }
            }
        }
    }

    assert(saw_different_offset);
}

void test_scheduled_bursty_rejects_overlapping_burst_spacing() {
    SyntheticWorkloadConfig config = make_config();
    config.issue_mode = WorkloadIssueMode::ScheduledBursty;
    config.burst_size = 4;
    config.burst_interval = 2;
    config.intra_burst_gap = 1;

    try {
        (void)generate_synthetic_workload(config);
        assert(false);
    } catch (const std::invalid_argument& error) {
        const std::string message = error.what();
        assert(message.find("burst_interval") != std::string::npos);
    }
}

void test_bimodal_sizes_are_per_object_and_do_not_change_access_order() {
    SyntheticWorkloadConfig fixed_config = make_config();
    fixed_config.compute_node_ids = {1, 2};
    fixed_config.object_count = 32;
    fixed_config.requests_per_node_per_epoch = 32;
    fixed_config.epoch_count = 2;
    fixed_config.hot_set_mode = HotSetMode::EpochShift;
    fixed_config.cross_node_overlap = CrossNodeOverlap::Medium;

    SyntheticWorkloadConfig bimodal_config = fixed_config;
    bimodal_config.object_size_mode = ObjectSizeMode::Bimodal;
    bimodal_config.object_size_small_bytes = 8;
    bimodal_config.object_size_large_bytes = 64;
    bimodal_config.large_object_probability = 0.5;

    const GeneratedWorkload fixed = generate_synthetic_workload(fixed_config);
    const GeneratedWorkload bimodal = generate_synthetic_workload(bimodal_config);

    bool saw_small = false;
    bool saw_large = false;
    std::unordered_map<ObjectId, std::uint64_t> size_by_object;
    for (std::size_t node_index = 0;
         node_index < fixed.node_workloads.size();
         ++node_index) {
        const std::vector<RequestSpec>& fixed_requests =
            fixed.node_workloads[node_index].requests;
        const std::vector<RequestSpec>& bimodal_requests =
            bimodal.node_workloads[node_index].requests;
        assert(fixed_requests.size() == bimodal_requests.size());

        for (std::size_t i = 0; i < fixed_requests.size(); ++i) {
            assert(fixed_requests[i].object_id == bimodal_requests[i].object_id);
            assert(fixed_requests[i].epoch_id == bimodal_requests[i].epoch_id);

            const std::uint64_t size = bimodal_requests[i].size_bytes;
            assert(size == bimodal_config.object_size_small_bytes ||
                   size == bimodal_config.object_size_large_bytes);
            saw_small = saw_small ||
                        size == bimodal_config.object_size_small_bytes;
            saw_large = saw_large ||
                        size == bimodal_config.object_size_large_bytes;

            const auto [iterator, inserted] =
                size_by_object.emplace(bimodal_requests[i].object_id, size);
            if (!inserted) {
                assert(iterator->second == size);
            }
        }
    }

    assert(saw_small);
    assert(saw_large);
}

void test_hot_object_channel_restriction_places_hot_sets_on_selected_channels() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 64;
    config.memory_channel_count = 4;
    config.hot_object_channel_count = 1;
    config.hot_set_size = 4;
    config.epoch_count = 3;
    config.hot_set_mode = HotSetMode::EpochShift;
    config.hot_set_churn_fraction = 0.5;
    config.cross_node_overlap = CrossNodeOverlap::High;

    const GeneratedWorkload workload = generate_synthetic_workload(config);

    for (const EpochHotSetMetadata& epoch_metadata : workload.epochs) {
        for (NodeId node_id : config.compute_node_ids) {
            for (ObjectId object_id : hot_set_for_node(epoch_metadata, node_id)) {
                assert(memory_channel_for_object(object_id,
                                                 config.memory_channel_count) ==
                       0);
            }
        }
    }
}

void test_hot_object_channel_restriction_validates_available_objects() {
    SyntheticWorkloadConfig config = make_config();
    config.compute_node_ids = {1, 2};
    config.object_count = 16;
    config.memory_channel_count = 4;
    config.hot_object_channel_count = 1;
    config.hot_set_size = 8;
    config.cross_node_overlap = CrossNodeOverlap::High;

    try {
        (void)generate_synthetic_workload(config);
        assert(false);
    } catch (const std::invalid_argument&) {
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
    test_epoch_shift_zero_churn_keeps_hot_sets_stable();
    test_epoch_shift_full_churn_replaces_hot_sets();
    test_partial_churn_changes_configured_fraction_of_segments();
    test_overlap_presets_shape_hot_sets();
    test_partial_churn_preserves_overlap_presets();
    test_generated_request_counts_and_epochs();
    test_scheduled_bursty_offsets_are_deterministic_and_ordered();
    test_scheduled_bursty_jitter_can_change_with_seed();
    test_scheduled_bursty_rejects_overlapping_burst_spacing();
    test_bimodal_sizes_are_per_object_and_do_not_change_access_order();
    test_hot_object_channel_restriction_places_hot_sets_on_selected_channels();
    test_hot_object_channel_restriction_validates_available_objects();
    test_workload_cursor_issues_requests_in_order();
    return 0;
}
