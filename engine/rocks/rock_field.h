// ---------------------------------------------------------------------------
// Which rocks (layer 9) exist near the camera, on the CPU -- TreeField's
// shape, for the same reason: placement is authoritative here, because the
// rock the GPU draws must be the one physics collides with and a save names.
//
// Tiles of 8 x 8 boulder cells (64 m at 8 m cells) on worker threads,
// nearest first. A tile also owns every outcrop whose cell's ORIGIN lies in
// it, so each outcrop belongs to exactly one tile though its cells are 48 m.
// ---------------------------------------------------------------------------
#ifndef ENGINE_ROCK_FIELD_H
#define ENGINE_ROCK_FIELD_H

#include "worldgen/world.h"

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

static const int kRockTileCells = 8;   // boulder cells per tile side

class RockField {
public:
    RockField();
    ~RockField();

    void init(const WorldGpu* world, int threads);
    void shutdown();

    // Keep tiles out to `radius` metres resident around (x, z). Returns true
    // if the resident set changed.
    bool update(float x, float z, float radius);

    const std::vector<worldgen::RockInstance>& rocks() const { return rocks_; }

    // A rock the player dug out from under drops by `drop` metres (y and
    // ground both), by id, so it stays down when its tile streams in again.
    // TreeField::set_base's twin; commit_drops applies changes in one pass.
    void set_drop(uint32_t id, float drop) { drops_[id] = drop; drops_dirty_ = true; }
    void clear_drop(uint32_t id) { if (drops_.erase(id)) drops_dirty_ = true; }
    void commit_drops() { if (drops_dirty_) rebuild(); }
    unsigned long long version() const { return version_; }
    bool settled() const { return settled_; }
    size_t tiles_resident() const { return tiles_.size(); }

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
        std::vector<worldgen::RockInstance> rocks;
    };

    void worker();
    void rebuild();
    float tile_m() const;

    const WorldGpu* world_ = nullptr;
    std::unordered_map<TileKey, Tile, TileKeyHash> tiles_;
    std::vector<worldgen::RockInstance> rocks_;
    std::unordered_map<uint32_t, float> drops_;
    bool drops_dirty_ = false;
    unsigned long long version_ = 0;
    bool dirty_ = false;
    bool settled_ = false;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<TileKey> queue_;
    std::unordered_set<TileKey, TileKeyHash> in_flight_;
    std::vector<std::pair<TileKey, Tile> > done_;
    std::vector<std::thread> threads_;
    bool stop_ = false;
    int  last_cx_ = 1 << 30, last_cz_ = 1 << 30;
    float last_radius_ = -1.0f;
};

// The rotation a rock is drawn and collided with: its yaw, and a tilt of up
// to 8 degrees in a direction of its own. Presentation; sin and cos are fine.
void rock_rotation(const worldgen::RockInstance& r, float q[4]);

}  // namespace engine

#endif  // ENGINE_ROCK_FIELD_H
