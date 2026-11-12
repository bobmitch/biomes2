// ---------------------------------------------------------------------------
// The edit overlay: player edits only, as ABSOLUTE density and material.
//
// NOT a sparse voxel DAG, and that is the design document's own
// recommendation rather than a shortcut. A DAG's advantage is deduplicating
// repeated structure across a large volume, and what this stores is sparse,
// small and mostly append-only. A hash map of bricks is perhaps a tenth of the
// code, has no pointer-chasing descent in the sampling path, and serialises
// trivially. The sampling interface is the same either way, so promoting to a
// DAG later is a contained swap -- and deciding before there is a single edit
// to measure is the expensive version of that choice.
//
// Invariant 5: base terrain is never stored. A brick holds absolute values for
// the voxels that were edited and nothing for the rest, which fall through to
// the field function. Storing deltas against the base would tie every saved
// edit to the generator version that produced it, and a silent generator
// change would then corrupt them rather than merely move the world.
// ---------------------------------------------------------------------------
#ifndef ENGINE_BRICK_STORE_H
#define ENGINE_BRICK_STORE_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine {

// Edits live on the LOD 0 lattice. A coarser chunk samples the same store at
// its own spacing, which point-samples the edit rather than filtering it --
// visible as aliasing on a dug hole seen from far away, not as the hole
// disappearing.
static const float kEditVoxel = 0.5f;

static const int kBrickSide = 16;                 // 8 m at 0.5 m voxels
static const int kBrickVoxels = kBrickSide * kBrickSide * kBrickSide;
static const int kBrickMaskWords = kBrickVoxels / 32;

struct BrickKey {
    int x, y, z;
    bool operator==(const BrickKey& o) const { return x == o.x && y == o.y && z == o.z; }
};

struct BrickKeyHash {
    size_t operator()(const BrickKey& k) const {
        uint64_t h = 1469598103934665603ull;
        const int v[3] = {k.x, k.y, k.z};
        for (int i = 0; i < 3; ++i) {
            h ^= static_cast<uint64_t>(static_cast<uint32_t>(v[i]));
            h *= 1099511628211ull;
        }
        return static_cast<size_t>(h);
    }
};

struct Brick {
    float    density[kBrickVoxels];
    uint8_t  material[kBrickVoxels];
    uint32_t edited[kBrickMaskWords];   // which voxels this brick actually owns
    uint32_t slot;                      // its place in the GPU pool
    bool     dirty;                     // needs re-uploading
};

class BrickStore {
public:
    // Carve or fill a sphere. Returns the world-space box that changed, so the
    // caller knows which chunks to re-mesh.
    //
    // `strength` is the absolute density written at the centre, falling to the
    // surface value at the rim: negative digs, positive fills. Absolute, not a
    // subtraction from the base -- see invariant 5.
    bool edit_sphere(float cx, float cy, float cz, float radius,
                     float strength, uint8_t material,
                     float out_min[3], float out_max[3]);

    // Every brick overlapping a world box, as GPU pool slots.
    void bricks_in_box(const float lo[3], const float hi[3],
                       std::vector<uint32_t>& out_slots,
                       std::vector<int>& out_origins) const;

    size_t brick_count() const { return bricks_.size(); }
    size_t bytes() const { return bricks_.size() * sizeof(Brick); }

    // Bricks whose contents changed since the last upload.
    void take_dirty(std::vector<BrickKey>& out);
    const Brick* find(const BrickKey& k) const;

private:
    Brick& touch(const BrickKey& k, bool& created);

    std::unordered_map<BrickKey, Brick, BrickKeyHash> bricks_;
    std::vector<BrickKey> dirty_;
    uint32_t next_slot_ = 0;
};

}  // namespace engine

#endif  // ENGINE_BRICK_STORE_H
