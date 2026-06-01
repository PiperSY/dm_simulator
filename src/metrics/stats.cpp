#include "metrics/stats.hpp"

#include <algorithm>

namespace dm_sim {

// Records an end-to-end request latency in both the global totals and the
// per-node latency bucket for the node that completed the request.
void Stats::record_latency(NodeId node_id, SimTime latency) {
    total_latency_ += latency;
    latencies_.push_back(latency);

    NodeLatencyStats& node_stats = per_node_stats_[node_id];
    node_stats.total_latency += latency;
    node_stats.latencies.push_back(latency);
}

// Adds one memory wait sample and updates the running maximum wait time.
void Stats::record_memory_wait(SimTime wait_time) {
    total_memory_wait_ += wait_time;
    ++memory_wait_samples_;

    if (wait_time > max_memory_wait_) {
        max_memory_wait_ = wait_time;
    }
}

// Tracks the largest memory queue depth observed during the simulation.
void Stats::observe_memory_queue_depth(std::size_t queue_depth) {
    if (queue_depth > peak_memory_queue_depth_) {
        peak_memory_queue_depth_ = queue_depth;
    }
    if (queue_depth > peak_memory_channel_queue_depth_) {
        peak_memory_channel_queue_depth_ = queue_depth;
    }
}

// Records a local cache hit globally and for the node that performed it.
void Stats::record_cache_hit(NodeId node_id) {
    ++local_cache_hits_;
    ++per_node_cache_stats_[node_id].hits;
}

// Records a local cache miss globally and for the node that performed it.
void Stats::record_cache_miss(NodeId node_id) {
    ++local_cache_misses_;
    ++per_node_cache_stats_[node_id].misses;
}

// Counts a remote object access and updates requester diversity and observed
// queue depth for that object's epoch-specific contention bucket.
void Stats::record_remote_access(const Request& request,
                                 MemoryChannelId memory_channel_id,
                                 std::size_t observed_queue_depth) {
    InternalObjectContentionStats& bucket =
        contention_bucket(request, memory_channel_id);
    ++bucket.stats.remote_accesses;
    bucket.requesters.insert(request.source_node_id);
    bucket.stats.distinct_requesters = bucket.requesters.size();
    if (observed_queue_depth > bucket.stats.max_observed_queue_depth) {
        bucket.stats.max_observed_queue_depth = observed_queue_depth;
    }

    InternalChannelContentionStats& channel =
        channel_bucket(request.epoch_id, memory_channel_id);
    ++channel.stats.remote_accesses;
    if (observed_queue_depth > channel.stats.max_queue_depth) {
        channel.stats.max_queue_depth = observed_queue_depth;
    }
}

// Records time spent waiting in an object's remote-service queue and refreshes
// aggregate wait metrics for that object in the current epoch.
void Stats::record_object_queue_wait(const Request& request,
                                     MemoryChannelId memory_channel_id,
                                     SimTime wait_time) {
    InternalObjectContentionStats& bucket =
        contention_bucket(request, memory_channel_id);
    bucket.stats.total_queue_wait += wait_time;
    ++bucket.stats.queue_wait_samples;
    if (wait_time > bucket.stats.max_queue_wait) {
        bucket.stats.max_queue_wait = wait_time;
    }
    bucket.stats.average_queue_wait =
        static_cast<double>(bucket.stats.total_queue_wait) /
        static_cast<double>(bucket.stats.queue_wait_samples);

    InternalChannelContentionStats& channel =
        channel_bucket(request.epoch_id, memory_channel_id);
    channel.stats.total_queue_wait += wait_time;
    ++channel.stats.queue_wait_samples;
    if (wait_time > channel.stats.max_queue_wait) {
        channel.stats.max_queue_wait = wait_time;
    }
    channel.stats.average_queue_wait =
        static_cast<double>(channel.stats.total_queue_wait) /
        static_cast<double>(channel.stats.queue_wait_samples);
}

// Adds service-time and byte-count contribution for a completed remote object
// access.
void Stats::record_object_service(const Request& request,
                                  MemoryChannelId memory_channel_id,
                                  SimTime service_time) {
    InternalObjectContentionStats& bucket =
        contention_bucket(request, memory_channel_id);
    bucket.stats.total_remote_service_time += service_time;
    bucket.stats.bytes_served += request.size_bytes;

    InternalChannelContentionStats& channel =
        channel_bucket(request.epoch_id, memory_channel_id);
    channel.stats.total_remote_service_time += service_time;
    channel.stats.bytes_served += request.size_bytes;
}

// Returns the total number of completed requests with recorded latency samples.
std::size_t Stats::completed_requests() const noexcept {
    return latencies_.size();
}

// Returns the sum of all recorded request latencies.
SimTime Stats::total_latency() const noexcept {
    return total_latency_;
}

// Returns the mean request latency across all nodes, or zero with no samples.
double Stats::average_latency() const noexcept {
    if (latencies_.empty()) {
        return 0.0;
    }

    return static_cast<double>(total_latency_) /
           static_cast<double>(latencies_.size());
}

// Returns the full ordered list of recorded request latencies.
const std::vector<SimTime>& Stats::latencies() const noexcept {
    return latencies_;
}

// Returns the latency samples for one node, or an empty static vector if that
// node has not completed any requests.
const std::vector<SimTime>& Stats::latencies(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        static const std::vector<SimTime> empty_latencies;
        return empty_latencies;
    }

    return it->second.latencies;
}

// Returns the number of completed requests recorded for one node.
std::size_t Stats::completed_requests(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        return 0;
    }

    return it->second.latencies.size();
}

// Returns the sum of recorded latencies for one node.
SimTime Stats::total_latency(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end()) {
        return 0;
    }

    return it->second.total_latency;
}

// Returns the mean latency for one node, or zero if the node has no samples.
double Stats::average_latency(NodeId node_id) const noexcept {
    const auto it = per_node_stats_.find(node_id);
    if (it == per_node_stats_.end() || it->second.latencies.empty()) {
        return 0.0;
    }

    return static_cast<double>(it->second.total_latency) /
           static_cast<double>(it->second.latencies.size());
}

// Returns the accumulated memory wait time across all recorded samples.
SimTime Stats::total_memory_wait() const noexcept {
    return total_memory_wait_;
}

// Returns the average memory wait time, or zero if no waits were recorded.
double Stats::average_memory_wait() const noexcept {
    if (memory_wait_samples_ == 0) {
        return 0.0;
    }

    return static_cast<double>(total_memory_wait_) /
           static_cast<double>(memory_wait_samples_);
}

// Returns the longest single memory wait observed.
SimTime Stats::max_memory_wait() const noexcept {
    return max_memory_wait_;
}

// Returns the highest memory queue depth observed.
std::size_t Stats::peak_memory_queue_depth() const noexcept {
    return peak_memory_queue_depth_;
}

std::size_t Stats::peak_memory_channel_queue_depth() const noexcept {
    return peak_memory_channel_queue_depth_;
}

// Returns the total number of local cache hits.
std::size_t Stats::local_cache_hits() const noexcept {
    return local_cache_hits_;
}

// Returns the total number of local cache misses.
std::size_t Stats::local_cache_misses() const noexcept {
    return local_cache_misses_;
}

// Returns the global local-cache hit rate, or zero if there were no lookups.
double Stats::local_cache_hit_rate() const noexcept {
    const std::size_t total = local_cache_hits_ + local_cache_misses_;
    if (total == 0) {
        return 0.0;
    }

    return static_cast<double>(local_cache_hits_) / static_cast<double>(total);
}

// Returns the number of local cache hits for one node.
std::size_t Stats::local_cache_hits(NodeId node_id) const noexcept {
    const auto it = per_node_cache_stats_.find(node_id);
    if (it == per_node_cache_stats_.end()) {
        return 0;
    }

    return it->second.hits;
}

// Returns the number of local cache misses for one node.
std::size_t Stats::local_cache_misses(NodeId node_id) const noexcept {
    const auto it = per_node_cache_stats_.find(node_id);
    if (it == per_node_cache_stats_.end()) {
        return 0;
    }

    return it->second.misses;
}

// Returns the local-cache hit rate for one node, or zero if it has no lookups.
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

// Looks up contention metrics for one object in one epoch.
std::optional<ObjectContentionStats> Stats::object_contention(
    EpochId epoch_id,
    ObjectId object_id) const {
    const auto epoch_it = contention_by_epoch_.find(epoch_id);
    if (epoch_it == contention_by_epoch_.end()) {
        return std::nullopt;
    }

    const auto object_it = epoch_it->second.find(object_id);
    if (object_it == epoch_it->second.end()) {
        return std::nullopt;
    }

    return snapshot_contention(object_it->second);
}

// Returns all object contention snapshots for an epoch sorted by object ID.
std::vector<ObjectContentionStats> Stats::contention_by_epoch(
    EpochId epoch_id) const {
    const auto epoch_it = contention_by_epoch_.find(epoch_id);
    if (epoch_it == contention_by_epoch_.end()) {
        return {};
    }

    std::vector<ObjectContentionStats> stats;
    stats.reserve(epoch_it->second.size());
    for (const auto& object_entry : epoch_it->second) {
        // Convert from the internal requester-tracking bucket to the public
        // reporting structure before returning the data.
        stats.push_back(snapshot_contention(object_entry.second));
    }

    std::sort(stats.begin(),
              stats.end(),
              [](const ObjectContentionStats& lhs,
                 const ObjectContentionStats& rhs) {
                  return lhs.object_id < rhs.object_id;
              });
    return stats;
}

// Looks up contention metrics for an object in the epoch before current_epoch.
std::optional<ObjectContentionStats> Stats::previous_epoch_object_contention(
    EpochId current_epoch,
    ObjectId object_id) const {
    if (current_epoch == 0) {
        return std::nullopt;
    }

    return object_contention(current_epoch - 1, object_id);
}

// Returns all contention snapshots for the epoch before current_epoch.
std::vector<ObjectContentionStats> Stats::previous_epoch_contention(
    EpochId current_epoch) const {
    if (current_epoch == 0) {
        return {};
    }

    return contention_by_epoch(current_epoch - 1);
}

// Returns every object contention snapshot from every epoch, sorted
// deterministically by epoch and then object ID.
std::vector<ObjectContentionStats> Stats::all_contention_stats() const {
    std::vector<ObjectContentionStats> stats;
    for (const auto& epoch_entry : contention_by_epoch_) {
        for (const auto& object_entry : epoch_entry.second) {
            // Snapshot each bucket so derived fields, such as requester count
            // and average queue wait, are current at reporting time.
            stats.push_back(snapshot_contention(object_entry.second));
        }
    }

    std::sort(stats.begin(),
              stats.end(),
              [](const ObjectContentionStats& lhs,
                 const ObjectContentionStats& rhs) {
                  if (lhs.epoch_id != rhs.epoch_id) {
                      return lhs.epoch_id < rhs.epoch_id;
                  }
                  return lhs.object_id < rhs.object_id;
              });
    return stats;
}

// Returns the highest-contention object snapshots according to the requested
// sort key, using epoch and object ID as deterministic tie breakers.
std::vector<ObjectContentionStats> Stats::top_contention_objects(
    ContentionSortKey sort_key,
    std::size_t limit) const {
    std::vector<ObjectContentionStats> stats = all_contention_stats();

    std::sort(stats.begin(),
              stats.end(),
              [sort_key](const ObjectContentionStats& lhs,
                         const ObjectContentionStats& rhs) {
                  // Keep result order stable when the selected metric ties.
                  auto tie_break = [](const ObjectContentionStats& left,
                                      const ObjectContentionStats& right) {
                      if (left.epoch_id != right.epoch_id) {
                          return left.epoch_id < right.epoch_id;
                      }
                      return left.object_id < right.object_id;
                  };

                  switch (sort_key) {
                  case ContentionSortKey::TotalQueueWait:
                      if (lhs.total_queue_wait != rhs.total_queue_wait) {
                          return lhs.total_queue_wait > rhs.total_queue_wait;
                      }
                      break;
                  case ContentionSortKey::TotalRemoteServiceTime:
                      if (lhs.total_remote_service_time !=
                          rhs.total_remote_service_time) {
                          return lhs.total_remote_service_time >
                                 rhs.total_remote_service_time;
                      }
                      break;
                  case ContentionSortKey::DistinctRequesters:
                      if (lhs.distinct_requesters != rhs.distinct_requesters) {
                          return lhs.distinct_requesters > rhs.distinct_requesters;
                      }
                      break;
                  }

                  return tie_break(lhs, rhs);
              });

    if (stats.size() > limit) {
        stats.resize(limit);
    }

    return stats;
}

std::optional<ChannelContentionStats> Stats::channel_contention(
    EpochId epoch_id,
    MemoryChannelId memory_channel_id) const {
    const auto epoch_it = channel_contention_by_epoch_.find(epoch_id);
    if (epoch_it == channel_contention_by_epoch_.end()) {
        return std::nullopt;
    }

    const auto channel_it = epoch_it->second.find(memory_channel_id);
    if (channel_it == epoch_it->second.end()) {
        return std::nullopt;
    }

    return snapshot_channel_contention(channel_it->second);
}

std::vector<ChannelContentionStats> Stats::channel_contention_by_epoch(
    EpochId epoch_id) const {
    const auto epoch_it = channel_contention_by_epoch_.find(epoch_id);
    if (epoch_it == channel_contention_by_epoch_.end()) {
        return {};
    }

    std::vector<ChannelContentionStats> stats;
    stats.reserve(epoch_it->second.size());
    for (const auto& channel_entry : epoch_it->second) {
        stats.push_back(snapshot_channel_contention(channel_entry.second));
    }

    std::sort(stats.begin(),
              stats.end(),
              [](const ChannelContentionStats& lhs,
                 const ChannelContentionStats& rhs) {
                  return lhs.memory_channel_id < rhs.memory_channel_id;
              });
    return stats;
}

std::vector<ChannelContentionStats> Stats::all_channel_contention_stats() const {
    std::vector<ChannelContentionStats> stats;
    for (const auto& epoch_entry : channel_contention_by_epoch_) {
        for (const auto& channel_entry : epoch_entry.second) {
            stats.push_back(snapshot_channel_contention(channel_entry.second));
        }
    }

    std::sort(stats.begin(),
              stats.end(),
              [](const ChannelContentionStats& lhs,
                 const ChannelContentionStats& rhs) {
                  if (lhs.epoch_id != rhs.epoch_id) {
                      return lhs.epoch_id < rhs.epoch_id;
                  }
                  return lhs.memory_channel_id < rhs.memory_channel_id;
              });
    return stats;
}

// Retrieves or creates the internal contention bucket for a request's
// epoch/object pair and stamps the public identifiers onto it.
Stats::InternalObjectContentionStats& Stats::contention_bucket(
    const Request& request,
    MemoryChannelId memory_channel_id) {
    InternalObjectContentionStats& bucket =
        contention_by_epoch_[request.epoch_id][request.object_id];
    bucket.stats.epoch_id = request.epoch_id;
    bucket.stats.object_id = request.object_id;
    bucket.stats.memory_channel_id = memory_channel_id;
    return bucket;
}

Stats::InternalChannelContentionStats& Stats::channel_bucket(
    EpochId epoch_id,
    MemoryChannelId memory_channel_id) {
    InternalChannelContentionStats& bucket =
        channel_contention_by_epoch_[epoch_id][memory_channel_id];
    bucket.stats.epoch_id = epoch_id;
    bucket.stats.memory_channel_id = memory_channel_id;
    return bucket;
}

// Builds a public contention snapshot from the internal bucket, recomputing
// derived values from the latest raw counters and requester set.
ObjectContentionStats Stats::snapshot_contention(
    const InternalObjectContentionStats& internal_stats) {
    ObjectContentionStats snapshot = internal_stats.stats;
    snapshot.distinct_requesters = internal_stats.requesters.size();
    if (snapshot.queue_wait_samples == 0) {
        snapshot.average_queue_wait = 0.0;
    } else {
        snapshot.average_queue_wait =
            static_cast<double>(snapshot.total_queue_wait) /
            static_cast<double>(snapshot.queue_wait_samples);
    }
    return snapshot;
}

ChannelContentionStats Stats::snapshot_channel_contention(
    const InternalChannelContentionStats& internal_stats) {
    ChannelContentionStats snapshot = internal_stats.stats;
    if (snapshot.queue_wait_samples == 0) {
        snapshot.average_queue_wait = 0.0;
    } else {
        snapshot.average_queue_wait =
            static_cast<double>(snapshot.total_queue_wait) /
            static_cast<double>(snapshot.queue_wait_samples);
    }
    return snapshot;
}

}  // namespace dm_sim
