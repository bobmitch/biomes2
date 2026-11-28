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

// What one instance draws of its model. A standing tree, the stump left
// behind, the whole felled log -- and a PIECE, a band of the trunk between
// two heights, which is what a log chopped in half, a billet of firewood and
// a clump of the crown falling away all are. One mesh, five uses.
enum TreeDrawKind : uint32_t {
    kTreeStanding = 0, kTreeLog = 1, kTreeStump = 2,
    kTreePiece = 3,    // a band of the TRUNK: bark and the discs closing it
    kTreeDebris = 4    // a band of the CROWN: its branches and leaves, no discs
};

// The kind and the fade in one word. The fade is stored as the share of
// pixels to THROW AWAY, so a zeroed instance is an opaque standing tree.
inline uint32_t tree_mode(uint32_t kind, float fade = 1.0f) {
    const float cut = 1.0f - (fade < 0.0f ? 0.0f : (fade > 1.0f ? 1.0f : fade));
    return (kind & 0xFu) | (uint32_t(cut * 255.0f + 0.5f) << 8);
}

// What the GPU reads per tree. 80 bytes, StructuredBuffer-aligned.
struct TreeGpuInstance {
    float    pos[3];     // trunk base, world metres
    float    scale;      // height / the species' nominal height
    float    rot[4];     // quaternion x, y, z, w
    uint32_t model;      // species * kTreeVariants + variant
    uint32_t mode;       // tree_mode(): kind in bits 0-3, the fade's cut in 8-15
    float    hit_time;   // last chop, on the frame clock, for the shake
    uint32_t id;         // worldgen tree id
    float    clip_lo;    // kTreePiece: the model-local y band it draws ...
    float    clip_hi;
    float    cap_lo;     // ... the end discs' radial scale there, 0 for none ...
    float    cap_hi;
    float    axis_lo[2]; // ... and where the trunk's own axis stands (x, z) at
    float    axis_hi[2]; //     each end: it wanders, so a disc on the model's
                         //     axis would sit off the opening by a third of a
                         //     radius at the top of a tall trunk.
};
static_assert(sizeof(TreeGpuInstance) == 80, "read as a StructuredBuffer");

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

    // A standing tree's base moved off the generated ground: the player dug
    // it out from under it, and it drops to the new ground (Renderer's
    // settle_trees). Stored by id, so a tile streamed in again comes back
    // where its tree came to rest; standing() carries it, so drawing,
    // shadows and trunk colliders all follow. set_base only records;
    // commit_bases applies every change since in one pass.
    void set_base(uint32_t id, float y) { bases_[id] = y; bases_dirty_ = true; }
    void clear_base(uint32_t id) { if (bases_.erase(id)) bases_dirty_ = true; }
    void commit_bases();

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
    std::unordered_map<uint32_t, float> bases_;
    bool bases_dirty_ = false;
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
