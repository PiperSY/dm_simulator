#include "cache/local_cache.hpp"

namespace dm_sim {

LocalCache::LocalCache(std::size_t capacity_bytes, std::unique_ptr<CachePolicy> policy)
    : capacity_bytes_(capacity_bytes), policy_(std::move(policy)) {}

bool LocalCache::lookup(ObjectId object_id, SimTime access_time) {
    Request request;
    request.object_id = object_id;
    return lookup(request, access_time);
}

bool LocalCache::lookup(const Request& request, SimTime access_time) {
    const ObjectId object_id = request.object_id;
    const auto it = entries_.find(object_id);
    if (it == entries_.end()) {
        ++miss_count_;
        policy_->on_lookup(request, access_time, false);
        return false;
    }

    ++hit_count_;
    policy_->on_lookup(request, access_time, true);
    policy_->on_access(it->second, access_time);
    // Attribute the hit to the placement that originally installed this
    // object. This is what later becomes admission_yield in the reports.
    record_hit(object_id);
    return true;
}

bool LocalCache::admit(const Request& request,
                       const Response& response,
                       SimTime access_time) {
    std::vector<ObjectId> planned_victims;
    // Keep policy diagnostics and policy-neutral lifecycle diagnostics in
    // lockstep, but keep their storage separate. Some policies have no score
    // diagnostics, yet every policy has admission outcomes worth comparing.
    auto record_result = [&](bool admitted, const char* reason) {
        policy_->on_admission_result(request,
                                     response,
                                     access_time,
                                     admitted,
                                     reason,
                                     planned_victims);
        if (admitted) {
            record_successful_placement(request,
                                        access_time,
                                        reason,
                                        "remote_response",
                                        planned_victims);
        } else {
            record_rejected_admission(request,
                                      access_time,
                                      reason,
                                      planned_victims);
        }
        return admitted;
    };

    if (!policy_->should_admit(request, response)) {
        return record_result(false, "policy_rejected");
    }

    if (request.size_bytes > capacity_bytes_) {
        return record_result(false, "object_too_large");
    }

    std::unordered_map<ObjectId, CacheEntry> candidate_entries = entries_;
    std::size_t projected_occupancy = occupancy_bytes_;
    // Victim selection is planned against a copy first. That lets us record the
    // complete victim list if the admission succeeds, while leaving the real
    // cache unchanged if a policy refuses to evict anything.
    while (projected_occupancy + request.size_bytes > capacity_bytes_) {
        const std::optional<ObjectId> victim =
            policy_->select_victim(candidate_entries, request);
        if (!victim.has_value()) {
            return record_result(false, "no_victim");
        }

        const auto victim_it = candidate_entries.find(*victim);
        if (victim_it == candidate_entries.end()) {
            return record_result(false, "invalid_victim");
        }

        projected_occupancy -= victim_it->second.size_bytes;
        planned_victims.push_back(*victim);
        candidate_entries.erase(victim_it);
    }

    for (ObjectId victim : planned_victims) {
        evict(victim, access_time);
    }

    CacheEntry entry;
    entry.object_id = request.object_id;
    entry.size_bytes = request.size_bytes;
    entry.insert_time = access_time;
    entry.last_access_time = access_time;
    policy_->on_access(entry, access_time);

    occupancy_bytes_ += entry.size_bytes;
    bytes_admitted_ += entry.size_bytes;
    entries_[entry.object_id] = entry;
    return record_result(true, "admitted");
}

void LocalCache::on_epoch_start(EpochId epoch_id) {
    policy_->on_epoch_start(epoch_id);
}

void LocalCache::install_replicas(const std::vector<CacheReplica>& replicas,
                                  SimTime install_time) {
    install_replicas(replicas, install_time, 0, 0);
}

void LocalCache::install_replicas(const std::vector<CacheReplica>& replicas,
                                  SimTime install_time,
                                  NodeId node_id,
                                  EpochId epoch_id) {
    // Oracle/global replication replaces the cache at epoch boundaries. Treat
    // removed replicas as evicted placements and new replicas as admissions so
    // their reuse can be compared directly against online policies.
    clear_entries(install_time);

    for (const CacheReplica& replica : replicas) {
        if (replica.size_bytes > capacity_bytes_) {
            continue;
        }
        if (occupancy_bytes_ + replica.size_bytes > capacity_bytes_) {
            break;
        }
        if (entries_.find(replica.object_id) != entries_.end()) {
            continue;
        }

        CacheEntry entry;
        entry.object_id = replica.object_id;
        entry.size_bytes = replica.size_bytes;
        entry.insert_time = install_time;
        entry.last_access_time = install_time;

        occupancy_bytes_ += entry.size_bytes;
        bytes_admitted_ += entry.size_bytes;
        entries_[entry.object_id] = entry;
        record_successful_replica(node_id,
                                  epoch_id,
                                  replica.object_id,
                                  replica.size_bytes,
                                  install_time);
    }
}

bool LocalCache::contains(ObjectId object_id) const noexcept {
    return entries_.find(object_id) != entries_.end();
}

std::size_t LocalCache::occupancy_bytes() const noexcept {
    return occupancy_bytes_;
}

std::size_t LocalCache::capacity_bytes() const noexcept {
    return capacity_bytes_;
}

std::size_t LocalCache::entry_count() const noexcept {
    return entries_.size();
}

std::size_t LocalCache::hit_count() const noexcept {
    return hit_count_;
}

std::size_t LocalCache::miss_count() const noexcept {
    return miss_count_;
}

std::uint64_t LocalCache::bytes_admitted() const noexcept {
    return bytes_admitted_;
}

std::uint64_t LocalCache::bytes_evicted() const noexcept {
    return bytes_evicted_;
}

std::vector<PolicyDecisionRecord> LocalCache::policy_diagnostics() const {
    return policy_->diagnostics();
}

const std::vector<CacheAdmissionRecord>&
LocalCache::cache_admission_diagnostics() const noexcept {
    return admission_records_;
}

void LocalCache::evict(ObjectId object_id, SimTime eviction_time) {
    const auto it = entries_.find(object_id);
    if (it == entries_.end()) {
        return;
    }

    const auto placement_it = active_placement_by_object_.find(object_id);
    if (placement_it != active_placement_by_object_.end()) {
        // Close the active placement record but keep the row. Post-processing
        // can then ask whether this evicted object was needed again later.
        CacheAdmissionRecord& record = admission_records_[placement_it->second];
        record.evicted = true;
        record.eviction_time = eviction_time;
        active_placement_by_object_.erase(placement_it);
    }

    occupancy_bytes_ -= it->second.size_bytes;
    bytes_evicted_ += it->second.size_bytes;
    entries_.erase(it);
}

void LocalCache::clear_entries(SimTime eviction_time) {
    for (const auto& entry : entries_) {
        const auto placement_it =
            active_placement_by_object_.find(entry.first);
        if (placement_it != active_placement_by_object_.end()) {
            CacheAdmissionRecord& record =
                admission_records_[placement_it->second];
            record.evicted = true;
            record.eviction_time = eviction_time;
        }
        bytes_evicted_ += entry.second.size_bytes;
    }

    entries_.clear();
    active_placement_by_object_.clear();
    occupancy_bytes_ = 0;
}

void LocalCache::record_hit(ObjectId object_id) {
    const auto placement_it = active_placement_by_object_.find(object_id);
    if (placement_it == active_placement_by_object_.end()) {
        return;
    }

    CacheAdmissionRecord& record = admission_records_[placement_it->second];
    ++record.future_hit_count;
    record.reused_after_admit = true;
}

void LocalCache::record_successful_placement(
    const Request& request,
    SimTime placement_time,
    const char* reason,
    const char* placement_source,
    const std::vector<ObjectId>& evicted_objects) {
    CacheAdmissionRecord record;
    record.node_id = request.source_node_id;
    record.epoch_id = request.epoch_id;
    record.request_id = request.request_id;
    record.object_id = request.object_id;
    record.time = placement_time;
    record.admitted = true;
    record.reason = reason;
    record.placement_source = placement_source;
    record.size_bytes = request.size_bytes;
    record.evicted_objects = evicted_objects;

    admission_records_.push_back(record);
    // The object is now resident because of this successful admission. Future
    // hits and evictions update this same record.
    active_placement_by_object_[request.object_id] =
        admission_records_.size() - 1;
}

void LocalCache::record_successful_replica(NodeId node_id,
                                           EpochId epoch_id,
                                           ObjectId object_id,
                                           std::uint64_t size_bytes,
                                           SimTime placement_time) {
    CacheAdmissionRecord record;
    record.node_id = node_id;
    record.epoch_id = epoch_id;
    record.request_id = kInvalidRequestId;
    record.object_id = object_id;
    record.time = placement_time;
    record.admitted = true;
    record.reason = "replica_installed";
    record.placement_source = "global_replica";
    record.size_bytes = size_bytes;

    admission_records_.push_back(record);
    // Replicas do not correspond to a request_id, but once installed they
    // should earn future-hit credit exactly like demand-filled entries.
    active_placement_by_object_[object_id] = admission_records_.size() - 1;
}

void LocalCache::record_rejected_admission(
    const Request& request,
    SimTime attempt_time,
    const char* reason,
    const std::vector<ObjectId>& evicted_objects) {
    CacheAdmissionRecord record;
    record.node_id = request.source_node_id;
    record.epoch_id = request.epoch_id;
    record.request_id = request.request_id;
    record.object_id = request.object_id;
    record.time = attempt_time;
    record.admitted = false;
    record.reason = reason;
    record.placement_source = "remote_response";
    record.size_bytes = request.size_bytes;
    record.evicted_objects = evicted_objects;

    admission_records_.push_back(record);
}

}  // namespace dm_sim
