#include "metrics/histogram.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace dm_sim {

double compute_percentile(std::vector<SimTime> values, double percentile) {
    if (values.empty()) {
        return 0.0;
    }

    std::sort(values.begin(), values.end());

    if (percentile <= 0.0) {
        return static_cast<double>(values.front());
    }
    if (percentile >= 100.0) {
        return static_cast<double>(values.back());
    }

    const double position =
        (percentile / 100.0) * static_cast<double>(values.size() - 1);
    const std::size_t lower_index =
        static_cast<std::size_t>(std::floor(position));
    const std::size_t upper_index =
        static_cast<std::size_t>(std::ceil(position));

    if (lower_index == upper_index) {
        return static_cast<double>(values[lower_index]);
    }

    const double weight = position - static_cast<double>(lower_index);
    return static_cast<double>(values[lower_index]) * (1.0 - weight) +
           static_cast<double>(values[upper_index]) * weight;
}

double median_latency(std::vector<SimTime> values) {
    return compute_percentile(std::move(values), 50.0);
}

double p95_latency(std::vector<SimTime> values) {
    return compute_percentile(std::move(values), 95.0);
}

double p99_latency(std::vector<SimTime> values) {
    return compute_percentile(std::move(values), 99.0);
}

}  // namespace dm_sim
