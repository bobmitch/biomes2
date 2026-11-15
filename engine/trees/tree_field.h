// ---------------------------------------------------------------------------
// Which trees exist near the camera, on the CPU.
//
// Placement is authoritative HERE, not on the GPU. Layer 8 is a CPU function
// pinned by the golden hash, and a tree the GPU decided to draw has to be the
// same tree physics collides with and a save names when it is felled. A GPU
// evaluation of the same keep test could land an ULP the other side of its
// threshold (invariant 8: GPU agreement is a tolerance, never a hash), and that
// is a tree you can see and walk through, or chop and not fell.
//
// Tiles of 16 x 16 candidate cells (64 m at 4 m cells) are generated on worker
// threads, nearest first, and dropped once well outside the draw distance.
// The GPU gets one flat instance list, rebuilt when the set changes.
// ---------------------------------------------------------------------------
#ifndef ENGINE_TREE_FIELD_H
#define ENGINE_TREE_FIELD_H

#include "worldgen/world.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

class WorldGpu;

// What the GPU reads per tree. 48 bytes, StructuredBuffer-aligned.
struct TreeGpuInstance {
    float    pos[3];     // trunk base, world metres
    float    scale;      // height / the species' nominal height
    float    rot[4];     // quaternion x, y, z, w
    uint32_t model;      // species * kTreeVariants + variant
    uint32_t mode;       // 0 standing, 1 felled log, 2 stump
    float    hit_time;   // last chop, on the frame clock, for the shake
    uint32_t id;         // worldgen tree id
};
static_assert(sizeof(TreeGpuInstance) == 48, "read as a StructuredBuffer");

// Mesh variants per species. A tree's variant byte from layer 8 picks one.
static const uint32_t kTreeVariants = 4;
static const int kTreeTileCells = 16;

class TreeField {
public:
    TreeField();
    ~TreeField();

    void init(const WorldGpu* world, int threads);
    void shutdown();

    // Keep tiles out to `radius` metres resident around (x, z); queue what is
    // missing, nearest first; drop what is far outside. Collects finished
    // tiles. Returns true if the standing set changed.
    bool update(float x, float z, float radius);

    // Every standing tree resident, minus the felled ones. Valid until the
    // next update that returns true.
    const std::vector<worldgen::TreeInstance>& standing() const { return standing_; }
    unsigned long long version() const { return version_; }

    // Felled trees no longer stand; the renderer and physics carry them as
    // logs and stumps from then on.
    void set_felled(uint32_t id, bool felled);
    bool is_felled(uint32_t id) const { return felled_.count(id) != 0; }

    // Diagnostics.
    size_t tiles_resident() const { return tiles_.size(); }
    size_t tiles_queued() const;
    double gen_ms_total() const { return gen_ms_total_; }
    unsigned long long tiles_generated() const { return tiles_generated_; }
    // True once every tile within `radius` of the last update is resident.
    bool settled() const { return settled_; }

private:
    struct TileKey {
        int x, z;
        bool operator==(const TileKey& o) const { return x == o.x && z == o.z; }
    };
    struct TileKeyHash {
        size_t operator()(const TileKey& k) const {
            return std::hash<long long>()((static_cast<long long>(k.x) << 32) ^ (k.z & 0xFFFFFFFFll));
        }
    };
    struct Tile {
        std::vector<worldgen::TreeInstance> trees;
    };

    void worker();
    void rebuild();

    const WorldGpu* world_ = nullptr;
    std::unordered_map<TileKey, Tile, TileKeyHash> tiles_;
    std::unordered_set<uint32_t> felled_;
    std::vector<worldgen::TreeInstance> standing_;
    unsigned long long version_ = 0;
    bool dirty_ = false;
    bool settled_ = false;

    // Work queue: wanted tiles not resident, nearest first. The workers pop
    // from the front; update() rebuilds it when the camera crosses a tile.
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<TileKey> queue_;
    std::unordered_set<TileKey, TileKeyHash> in_flight_;
    std::vector<std::pair<TileKey, Tile> > done_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    int  last_cx_ = 1 << 30, last_cz_ = 1 << 30;
    float last_radius_ = -1.0f;
    double gen_ms_total_ = 0.0;
    unsigned long long tiles_generated_ = 0;
};

// The rotation a standing tree is drawn with: its yaw, and a lean of up to
// 3 degrees in a direction of its own. Presentation; sin and cos are fine.
void tree_standing_rotation(const worldgen::TreeInstance& t, float q[4]);

}  // namespace engine

#endif  // ENGINE_TREE_FIELD_H
