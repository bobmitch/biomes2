// ---------------------------------------------------------------------------
// The virtualised brick buffer.
//
// The GPU pool holds a fixed number of bricks; the store holds as many as the
// player has edited. So a slot is a LEASE rather than a brick's identity: a
// brick acquires one when a chunk about to be meshed needs it, and the
// least-recently-used brick gives one up when the pool is full.
//
// Eviction is by last-used frame and never touches a brick acquired this
// frame. Without that guard a frame needing more bricks than the pool holds
// would evict the ones it had just paged in, and the pool would thrash
// through the whole set without ever meshing anything correctly.
// ---------------------------------------------------------------------------
#ifndef ENGINE_BRICK_PAGER_H
#define ENGINE_BRICK_PAGER_H

#include "engine/svdag/brick_store.h"

#include <unordered_map>
#include <vector>

namespace engine {

class BrickPager {
public:
    void init(uint32_t pool_slots);

    // Give these bricks slots for this frame. `out_slots` comes back parallel
    // to `keys`; `out_uploads` lists the ones whose pool contents are now
    // stale and must be copied up before anything reads them.
    void acquire(const std::vector<BrickKey>& keys, uint64_t frame,
                 std::vector<uint32_t>& out_slots,
                 std::vector<BrickKey>& out_uploads);

    // The brick's contents changed, so whatever the pool holds is wrong.
    void invalidate(const BrickKey& k);
    // Record that a brick's contents have been copied into the pool.
    void mark_uploaded(const BrickKey& k);

    size_t   resident() const { return leases_.size(); }
    uint32_t pool_slots() const { return pool_slots_; }
    uint64_t evictions() const { return evictions_; }
    uint64_t misses() const { return misses_; }

private:
    bool evict_one(uint64_t frame, uint32_t& out_slot);

    std::unordered_map<BrickKey, BrickLease, BrickKeyHash> leases_;
    std::vector<uint32_t> free_slots_;
    uint32_t pool_slots_ = 0;
    uint64_t evictions_ = 0;
    uint64_t misses_ = 0;
};

}  // namespace engine

#endif  // ENGINE_BRICK_PAGER_H
