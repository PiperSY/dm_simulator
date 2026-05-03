#pragma once

#include <vector>

#include "sim/types.hpp"

namespace dm_sim {

[[nodiscard]] double compute_percentile(std::vector<SimTime> values,
                                        double percentile);
[[nodiscard]] double median_latency(std::vector<SimTime> values);
[[nodiscard]] double p95_latency(std::vector<SimTime> values);
[[nodiscard]] double p99_latency(std::vector<SimTime> values);

}  // namespace dm_sim
