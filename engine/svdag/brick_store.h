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
#include <string>
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

// No GPU slot is held here. A slot is a LEASE the pager hands out, not part
// of a brick's identity: the pool is finite and the store is not.
struct Brick {
    float    density[kBrickVoxels];
    uint8_t  material[kBrickVoxels];
    uint32_t edited[kBrickMaskWords];   // which voxels this brick actually owns
};

// A brick's place in the GPU pool, while it has one.
struct BrickLease {
    uint32_t slot;
    uint64_t last_used;   // frame number, for eviction
    bool     uploaded;    // the pool holds this brick's current contents
};

// What a save file records about the world it was made in. The trivial field
// has no generator version; M1 must put the real one here, because the saved
// edits are absolute but the terrain AROUND them is generated, and a silent
// generator change moves that terrain out from under them.
static const uint32_t kSaveMagic = 0x44453242u;   // "B2ED"
static const uint32_t kSaveFormat = 1;
static const uint32_t kTrivialFieldId = 0;

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

    // Every brick overlapping a world box, by key. Turning those into GPU
    // slots is the pager's job.
    void bricks_in_box(const float lo[3], const float hi[3],
                       std::vector<BrickKey>& out) const;

    size_t brick_count() const { return bricks_.size(); }
    size_t bytes() const { return bricks_.size() * sizeof(Brick); }

    // Every brick, in no particular order. The pager walks this.
    const std::unordered_map<BrickKey, Brick, BrickKeyHash>& all() const { return bricks_; }

    // Only the voxels each brick owns are written, so a save is the size of
    // the edits rather than the size of the bricks holding them.
    bool save(const std::string& path, uint32_t field_id, std::string& error) const;
    bool load(const std::string& path, uint32_t expect_field_id,
              std::string& error, bool& field_mismatch);

    size_t edited_voxels() const;

    // Bricks whose contents changed since the last upload.
    void take_dirty(std::vector<BrickKey>& out);
    const Brick* find(const BrickKey& k) const;

private:
    Brick& touch(const BrickKey& k, bool& created);

    std::unordered_map<BrickKey, Brick, BrickKeyHash> bricks_;
    std::vector<BrickKey> dirty_;
};

}  // namespace engine

#endif  // ENGINE_BRICK_STORE_H
