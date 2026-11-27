// ---------------------------------------------------------------------------
// The navmesh round the player (ADR-007, decision 4): one Detour tile per
// 64 m zone, built from the field on worker threads (nav_tile.h), added on
// the main thread, dropped when the player has left. One mesh per agent size
// class; only the human class (the mimic's) is built so far.
//
// The main thread GATHERS what a tile needs that lives in engine state -- the
// trunks, rocks, stumps and resting logs in its box, and a copy of the edited
// bricks -- and a worker builds it. invalidate_box() asks for a rebuild of
// every tile a box touches (a dig, a log coming to rest); the old tile stays
// until the new one replaces it, and a result older than the latest request
// for its tile is thrown away.
// ---------------------------------------------------------------------------
#ifndef ENGINE_NAV_NAV_WORLD_H
#define ENGINE_NAV_NAV_WORLD_H

#include "engine/nav/nav_tile.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

class dtNavMesh;
class dtNavMeshQuery;

namespace engine {
namespace nav {

class NavWorld {
public:
    // Fills a tile's obstacles and edits (main thread).
    using GatherFn = std::function<void(TileInput& in)>;
    // Whether the engine state a gather reads is ready (trees and rocks
    // streamed in); tiles wait for it.
    using ReadyFn = std::function<bool()>;

    NavWorld() = default;
    ~NavWorld();
    NavWorld(const NavWorld&) = delete;
    NavWorld& operator=(const NavWorld&) = delete;

    bool init(const worldgen::SeedWorld* w, const worldgen::WorldParams* wp, const worldgen::ArchetypeTable* t,
              float zone_size, int threads, std::string& error);
    void shutdown();
    bool ready() const { return mesh_ != nullptr; }
    void set_gather(GatherFn g) { gather_ = std::move(g); }
    void set_ready(ReadyFn r) { ready_fn_ = std::move(r); }

    // Main thread, every frame: integrate finished tiles, request the tiles
    // within `radius` of (x, z) nearest first, drop those past radius + 96 m.
    void update(float x, float z, float radius);
    // Rebuild every tile the box touches; every tile (a load).
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();
    // True when every tile wanted at the last update is in and nothing is
    // being built.
    bool settled() const;
    bool has_tile(float x, float z) const;

    // Queries, on the human mesh. `include`: Flag bits a path may use.
    // The nearest point on the mesh to p within ext (half extents).
    bool nearest(const float p[3], const float ext[3], unsigned short include, float out[3],
                 unsigned char* area = nullptr) const;
    // A straight path (corners) from a to b; `complete` false when it ends
    // short (b unreachable: the nearest reachable point instead). Swimming
    // costs swim_cost x its length, wading 1.5x.
    bool path(const float a[3], const float b[3], unsigned short include, std::vector<float>& corners,
              float* length = nullptr, bool* complete = nullptr, float swim_cost = 3.0f) const;

    // Move from `from` towards `to` along the mesh (Detour's
    // moveAlongSurface: slides along edges, never leaves the mesh); `out` the
    // point reached at the mesh's height, `area` its poly's. False off the mesh.
    bool move_along(const float from[3], const float to[3], unsigned short include, float out[3],
                    unsigned char* area = nullptr) const;

    const dtNavMesh* mesh() const { return mesh_; }

    struct Stats {
        size_t tiles = 0, empty = 0, pending = 0, built = 0, failed = 0;
        double ms_total = 0.0, ms_max = 0.0;
        long long samples = 0;
        long long polys = 0;
        std::string last_error;
    };
    const Stats& stats() const { return stats_; }

private:
    struct Key {
        int x = 0, z = 0;
        bool operator==(const Key& o) const { return x == o.x && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const {
            return std::hash<long long>()((static_cast<long long>(k.x) << 32) ^ (k.z & 0xFFFFFFFFll));
        }
    };
    struct State {
        uint64_t want = 1;         // generation wanted
        uint64_t have = 0;         // generation in the mesh (0: none)
        bool pending = false;      // a job is out
        bool present = false;      // a tile (or a known-empty result) is in
        uint64_t ref = 0;          // dtTileRef, 0 when empty
    };
    struct Job {
        Key key;
        uint64_t gen = 0;
        TileInput in;
    };
    struct Result {
        Key key;
        uint64_t gen = 0;
        TileOutput out;
        bool ok = false;
        std::string error;
    };
    void worker();
    void drop(const Key& k);

    const worldgen::SeedWorld* w_ = nullptr;
    const worldgen::WorldParams* wp_ = nullptr;
    const worldgen::ArchetypeTable* t_ = nullptr;
    float size_ = 64.0f;
    int origin_ = 0;              // tile coordinate of zone 0: zone x + origin_
    dtNavMesh* mesh_ = nullptr;
    dtNavMeshQuery* query_ = nullptr;
    GatherFn gather_;
    ReadyFn ready_fn_;
    Agent agent_;

    std::unordered_map<Key, State, KeyHash> tiles_;
    std::vector<Key> wanted_;
    Stats stats_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<Result> results_;
    std::vector<std::thread> threads_;
    int busy_ = 0;
    bool stop_ = false;
};

}  // namespace nav
}  // namespace engine

#endif
