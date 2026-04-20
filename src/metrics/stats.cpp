#include "metrics/stats.hpp"

namespace dm_sim {

void Stats::record_latency(SimTime latency) {
    total_latency_ += latency;
    latencies_.push_back(latency);
}

std::size_t Stats::completed_requests() const noexcept {
    return latencies_.size();
}

SimTime Stats::total_latency() const noexcept {
    return total_latency_;
}

double Stats::average_latency() const noexcept {
    if (latencies_.empty()) {
        return 0.0;
    }

    return static_cast<double>(total_latency_) /
           static_cast<double>(latencies_.size());
}

const std::vector<SimTime>& Stats::latencies() const noexcept {
    return latencies_;
}

}  // namespace dm_sim
