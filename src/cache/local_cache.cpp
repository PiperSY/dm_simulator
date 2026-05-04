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
    return true;
}

bool LocalCache::admit(const Request& request,
                       const Response& response,
                       SimTime access_time) {
    if (!policy_->should_admit(request, response)) {
        return false;
    }

    if (request.size_bytes > capacity_bytes_) {
        return false;
    }

    while (occupancy_bytes_ + request.size_bytes > capacity_bytes_) {
        const std::optional<ObjectId> victim =
            policy_->select_victim(entries_, request);
        if (!victim.has_value()) {
            return false;
        }

        evict(*victim);
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
    return true;
}

void LocalCache::on_epoch_start(EpochId epoch_id) {
    policy_->on_epoch_start(epoch_id);
}

void LocalCache::install_replicas(const std::vector<CacheReplica>& replicas,
                                  SimTime install_time) {
    clear_entries();

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

void LocalCache::evict(ObjectId object_id) {
    const auto it = entries_.find(object_id);
    if (it == entries_.end()) {
        return;
    }

    occupancy_bytes_ -= it->second.size_bytes;
    bytes_evicted_ += it->second.size_bytes;
    entries_.erase(it);
}

void LocalCache::clear_entries() {
    for (const auto& entry : entries_) {
        bytes_evicted_ += entry.second.size_bytes;
    }

    entries_.clear();
    occupancy_bytes_ = 0;
}

}  // namespace dm_sim
