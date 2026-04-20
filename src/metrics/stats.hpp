#pragma once

#include <cstddef>
#include <vector>

#include "sim/types.hpp"

namespace dm_sim {

class Stats {
public:
    void record_latency(SimTime latency);

    [[nodiscard]] std::size_t completed_requests() const noexcept;
    [[nodiscard]] SimTime total_latency() const noexcept;
    [[nodiscard]] double average_latency() const noexcept;
    [[nodiscard]] const std::vector<SimTime>& latencies() const noexcept;

private:
    SimTime total_latency_ = 0;
    std::vector<SimTime> latencies_;
};

}  // namespace dm_sim
