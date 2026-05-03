#include "metrics/stats.hpp"

namespace dm_sim {

void Stats::record_latency(NodeId node_id, SimTime latency) {
    total_latency_ += latency;
    latencies_.push_back(latency);

    NodeLatencyStats& node_stats = per_node_stats_[node_id];
    node_stats.total_latency += latency;
    node_stats.latencies.push_back(latency);
}

void Stats::record_memory_wait(SimTime wait_time) {
    total_memory_wait_ += wait_time;
    ++memory_wait_samples_;

    if (wait_time > max_memory_wait_) {
        max_memory_wait_ = wait_time;
    }
}

void Stats::observe_memory_queue_depth(std::size_t queue_depth) {
    if (queue_depth > peak_memory_queue_depth_) {
        peak_memory_queue_depth_ = queue_depth;
    }
}

void Stats::record_cache_hit(NodeId node_id) {
    ++local_cache_hits_;
    ++per_node_cache_stats_[node_id].hits;
}

void Stats::record_cache_miss(NodeId node_id) {
    ++local_cache_misses_;
    ++per_node_cache_stats_[node_id].misses;
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

const std::vector<SimTime>& Stats::latencies(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        static const std::vector<SimTime> empty_latencies;
        return empty_latencies;
    }

    return it->second.latencies;
}

std::size_t Stats::completed_requests(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        return 0;
    }

    return it->second.latencies.size();
}

SimTime Stats::total_latency(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        return 0;
    }

    return it->second.total_latency;
}

double Stats::average_latency(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end() || it->second.latencies.empty()) {
        return 0.0;
    }

    return static_cast<double>(it->second.total_latency) /
           static_cast<double>(it->second.latencies.size());
}

SimTime Stats::total_memory_wait() const noexcept {
    return total_memory_wait_;
}

double Stats::average_memory_wait() const noexcept {
    if (memory_wait_samples_ == 0) {
        return 0.0;
    }

    return static_cast<double>(total_memory_wait_) /
           static_cast<double>(memory_wait_samples_);
}

SimTime Stats::max_memory_wait() const noexcept {
    return max_memory_wait_;
}

std::size_t Stats::peak_memory_queue_depth() const noexcept {
    return peak_memory_queue_depth_;
}

std::size_t Stats::local_cache_hits() const noexcept {
    return local_cache_hits_;
}

std::size_t Stats::local_cache_misses() const noexcept {
    return local_cache_misses_;
}

double Stats::local_cache_hit_rate() const noexcept {
    const std::size_t total = local_cache_hits_ + local_cache_misses_;
    if (total == 0) {
        return 0.0;
    }

    return static_cast<double>(local_cache_hits_) / static_cast<double>(total);
}

std::size_t Stats::local_cache_hits(NodeId node_id) const noexcept {
    const auto it = per_node_cache_stats_.find(node_id);
    if (it == per_node_cache_stats_.end()) {
        return 0;
    }

    return it->second.hits;
}

std::size_t Stats::local_cache_misses(NodeId node_id) const noexcept {
    const auto it = per_node_cache_stats_.find(node_id);
    if (it == per_node_cache_stats_.end()) {
        return 0;
    }

    return it->second.misses;
}

double Stats::local_cache_hit_rate(NodeId node_id) const noexcept {
    const auto it = per_node_cache_stats_.find(node_id);
    if (it == per_node_cache_stats_.end()) {
        return 0.0;
    }

    const std::size_t total = it->second.hits + it->second.misses;
    if (total == 0) {
        return 0.0;
    }

    return static_cast<double>(it->second.hits) / static_cast<double>(total);
}

}  // namespace dm_sim
