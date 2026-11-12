#include "engine/svdag/brick_pager.h"

namespace engine {

void BrickPager::init(uint32_t pool_slots) {
    pool_slots_ = pool_slots;
    leases_.clear();
    free_slots_.clear();
    free_slots_.reserve(pool_slots);
    for (uint32_t i = pool_slots; i-- > 0;) free_slots_.push_back(i);
    evictions_ = 0;
    misses_ = 0;
}

bool BrickPager::evict_one(uint64_t frame, uint32_t& out_slot) {
    // Oldest first, and never one this frame already needs. A frame that asks
    // for more bricks than the pool holds would otherwise evict what it just
    // paged in and mesh against a pool full of the wrong terrain.
    std::unordered_map<BrickKey, BrickLease, BrickKeyHash>::iterator victim = leases_.end();
    uint64_t oldest = ~0ull;
    for (std::unordered_map<BrickKey, BrickLease, BrickKeyHash>::iterator it = leases_.begin();
         it != leases_.end(); ++it) {
        if (it->second.last_used >= frame) continue;
        if (it->second.last_used < oldest) {
            oldest = it->second.last_used;
            victim = it;
        }
    }
    if (victim == leases_.end()) return false;
    out_slot = victim->second.slot;
    leases_.erase(victim);
    ++evictions_;
    return true;
}

void BrickPager::acquire(const std::vector<BrickKey>& keys, uint64_t frame,
                         std::vector<uint32_t>& out_slots,
                         std::vector<BrickKey>& out_uploads) {
    out_slots.clear();
    out_slots.reserve(keys.size());

    for (size_t i = 0; i < keys.size(); ++i) {
        std::unordered_map<BrickKey, BrickLease, BrickKeyHash>::iterator it =
            leases_.find(keys[i]);
        if (it != leases_.end()) {
            it->second.last_used = frame;
            out_slots.push_back(it->second.slot);
            if (!it->second.uploaded) out_uploads.push_back(keys[i]);
            continue;
        }

        uint32_t slot = 0;
        if (!free_slots_.empty()) {
            slot = free_slots_.back();
            free_slots_.pop_back();
        } else if (!evict_one(frame, slot)) {
            // Every slot is spoken for this frame. Dropping the brick is the
            // only option left, and the chunk meshes without that edit --
            // reported as a miss rather than hidden, because a pool this
            // small is a configuration problem, not a transient.
            ++misses_;
            out_slots.push_back(0);
            continue;
        }

        BrickLease lease;
        lease.slot = slot;
        lease.last_used = frame;
        lease.uploaded = false;
        leases_[keys[i]] = lease;
        out_slots.push_back(slot);
        out_uploads.push_back(keys[i]);
    }
}

void BrickPager::invalidate(const BrickKey& k) {
    std::unordered_map<BrickKey, BrickLease, BrickKeyHash>::iterator it = leases_.find(k);
    if (it != leases_.end()) it->second.uploaded = false;
}

void BrickPager::mark_uploaded(const BrickKey& k) {
    std::unordered_map<BrickKey, BrickLease, BrickKeyHash>::iterator it = leases_.find(k);
    if (it != leases_.end()) it->second.uploaded = true;
}

}  // namespace engine
