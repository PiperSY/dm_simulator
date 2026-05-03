#include "workloads/workload.hpp"

#include <stdexcept>
#include <utility>

namespace dm_sim {

WorkloadCursor::WorkloadCursor(std::vector<RequestSpec> requests)
    : requests_(std::move(requests)) {}

bool WorkloadCursor::has_next() const noexcept {
    return next_index_ < requests_.size();
}

std::size_t WorkloadCursor::issued_count() const noexcept {
    return next_index_;
}

std::size_t WorkloadCursor::remaining_count() const noexcept {
    return requests_.size() - next_index_;
}

std::size_t WorkloadCursor::total_count() const noexcept {
    return requests_.size();
}

RequestSpec WorkloadCursor::next() {
    if (!has_next()) {
        throw std::out_of_range("WorkloadCursor has no remaining requests");
    }

    // Post-increment returns the current request and advances in one step.
    return requests_[next_index_++];
}

}  // namespace dm_sim
