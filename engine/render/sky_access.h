#pragma once
// ---------------------------------------------------------------------------
// Sky access: how far underground air is from the open sky, by path.
//
// The underground GI plan's base layer under the near field's probes: a CPU
// flood over 1 m cells giving every air cell under the top surface the
// length of the shortest path through air to the sky -- to air with open sky
// straight above it -- capped at the reach (archetypes.json `underground`
// sky_reach_m, 96 m). Past the reach, or sealed, it has none (kNoSky). The
// near field turns that into an access exp(-path / L) (L, sky_falloff_m,
// tunable live: how fast daylight dies down a tunnel) and uses it where its
// own rays cannot see: a ray that leaves its 128 m volume brings back the
// heightfield's sky times the access of the air it left by, a camera in air
// with little access sees no daylit haze past the volume, and a probe in air
// with none and no light near does not trace.
//
// Step 1 labelled air only open or sealed. With v10's cave densities, caves
// reached through layer 7's shafts, air 1,163 m by path from any opening
// read as open: the deepest tunnel lit at noon, dark at midnight.
//
// The cells are the field as the near field's GPU fill evaluates it -- the
// CPU's terrain_density at a 1 m voxel -- and the player's edits at the 1 m
// points. A requested block's distances are EXACT, from a forward search
// through air from all its unknown cells out to the reach, recording every
// sky cell met, then a search back from those over the cells it reached: any
// shortest path from a block cell to the sky within the reach lies inside
// what the forward search saw. The work is the air within the reach of the
// block, however big the cave.
//
// ON ITS OWN THREAD. The main thread posts requests (blocks, the camera's
// cell, an edit) and takes results; the worker owns every cell. Results
// carry the generation of the last edit they saw, the main thread drops
// older ones, and the worker skips requests made before a later edit (they
// are asked again). The edit store is read under a shared lock only while a
// batch of bricks is evaluated; the writers (a dig, a load) take it
// exclusively. An edit forgets the distances of cells within the reach of
// it and the bricks and sky columns it touched; nothing else.
//
// No caves and no edits: nothing to search -- the near field asks nothing,
// so the shipping world (caves off) pays only where the player has dug.
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace engine {

class BrickStore;
class WorldGpu;

class SkyAccess {
public:
    static const int kBrick = 8;                       // cells a side (1 m), the near field's block
    static const int kCells = kBrick * kBrick * kBrick;
    static const uint8_t kNoSky = 255;                 // past the reach, or sealed

    struct Result {
        int b[3];                     // world block, 8 m units
        uint64_t gen;                 // the edit generation it was made under
        uint8_t dist[kCells];         // cell (x + 8 y + 64 z): metres to the sky by path, kNoSky; 0 rock or sky
    };

    ~SkyAccess() { shutdown(); }
    // Starts the worker. `edits` is read under the edit lock only. `reach_m`
    // caps the paths (at most 254 m): the cost is the air within it.
    void init(const WorldGpu* world, const BrickStore* edits, float reach_m);
    void shutdown();
    float reach() const { return float(reach_); }

    // --- the main thread ---------------------------------------------------
    // Held by whoever writes the edit store, for the write.
    std::unique_lock<std::shared_mutex> edit_lock() { return std::unique_lock<std::shared_mutex>(edits_mutex_); }
    // The edits changed inside a world box. Results made before it are stale.
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();
    uint64_t generation() const { return gen_; }
    // Whether block (bx, by, bz) can hold air away from the sky at all,
    // without the field: caves anywhere in the world, or an edit in it. The
    // caller is the edit store's writer, so it reads it without the lock.
    bool may_seal_cheap(int bx, int by, int bz) const;
    void request_block(int bx, int by, int bz);
    // The camera's cell, coalesced: only the latest is answered.
    void request_camera(float x, float y, float z);
    void take_results(std::vector<Result>& out);
    // The camera's cell's distance at the last answer under the current
    // generation; 0 (open) until one has come back.
    uint8_t camera_dist() const { return cam_gen_.load() == gen_ ? cam_dist_.load() : uint8_t(0); }
    // Until every request posted so far is answered (a still).
    void wait_idle();
    size_t queued() const;

    // Diagnostics over the life of the object (approximate while it runs).
    size_t bricks() const { return n_bricks_.load(); }
    unsigned long long searches() const { return searches_.load(); }          // batches searched
    unsigned long long cells_searched() const { return cells_searched_.load(); }
    unsigned long long cells_evaluated() const { return cells_evaluated_.load(); }
    unsigned long long cells_no_sky() const { return no_sky_.load(); }      // cells answered kNoSky
    unsigned long long cells_reset() const { return cells_reset_.load(); }
    double ms() const { return double(us_.load()) / 1000.0; }

private:
    // st: kRock, kAir (evaluated), kSkyFixed (over the overhang's reach and
    // anything built: open by construction, never evaluated).
    enum : uint8_t { kRock = 0, kAir = 1, kSkyFixed = 2 };
    struct Brick {
        uint8_t st[kCells];
        uint8_t dist[kCells];
        uint64_t known[kCells / 64];
        bool has_air = false;
    };
    struct Column {
        float h[kBrick * kBrick];   // the column's height at a 1 m voxel (terrain_height)
        float reach = 0.0f;         // the overhang term's largest reach here, + 1 m
        float hmin = 0.0f, hmax = 0.0f;
        float cave_lo = 3.4e38f;    // lowest placed-cave air under the column, or 3.4e38
    };
    struct Cmd {
        enum Kind { kBlock, kInvalidate, kInvalidateAll } kind;
        int b[3];
        float lo[3], hi[3];
        uint64_t gen;
    };
    static int64_t key(int bx, int by, int bz) {
        return (int64_t(bx + (1 << 20)) << 42) | (int64_t(by + (1 << 20)) << 21) | int64_t(bz + (1 << 20));
    }

    // --- the worker --------------------------------------------------------
    void run();
    void answer(std::vector<Cmd>& blocks, bool camera, const float cam[3]);
    void distances(const std::vector<int64_t>& sources);   // cells, packed (cell_key)
    bool may_seal(int bx, int by, int bz);
    void forget(const float lo[3], const float hi[3]);
    const Column& column(int bx, int bz);
    Brick* brick(int bx, int by, int bz);   // evaluated on first use
    void evaluate_batch(const std::vector<int>& bxyz);   // several bricks, in parallel
    Brick* brick_of(int x, int y, int z, int& idx);       // evaluating it if needed
    bool is_sky(int x, int y, int z);
    int sky_floor(int x, int z);
    float density(int x, int y, int z) const;
    bool has_edits_locked(int bx, int by, int bz) const;

    const WorldGpu* world_ = nullptr;
    bool placed_ = false;   // the world has placed caves (layer 7 since v19)
    const BrickStore* edits_ = nullptr;
    bool caves_ = false;
    int reach_ = 96;
    float cave_depth_ = 0.0f;              // the deepest cave_depth_max of any caving biome, + margin
    std::unordered_map<int64_t, Brick> bricks_;      // the worker's
    std::unordered_map<int64_t, Column> columns_;    // the worker's
    std::unordered_map<int64_t, int> sky_floor_;     // the worker's: per 1 m column, the lowest cell with sky straight above
    uint64_t worker_gen_ = 0;                        // the worker's: the last invalidation it applied

    mutable std::shared_mutex edits_mutex_;
    mutable std::mutex mutex_;                       // the queue, the results, the camera request
    std::condition_variable cv_, idle_cv_;
    std::deque<Cmd> queue_;
    std::vector<Result> results_;
    bool cam_pending_ = false;
    float cam_[3] = {0.0f, 0.0f, 0.0f};
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<uint64_t> gen_{1};                   // the main thread's: bumped by every invalidation
    std::atomic<uint64_t> cam_gen_{0};
    std::atomic<uint8_t> cam_dist_{0};
public:
    // Diagnostics: the camera cell's state (0 rock, 1 air, 2 open by
    // construction, -1 not looked at) and its column's sky floor.
    std::atomic<int> dbg_cam_st_{-1}, dbg_cam_floor_{0};
private:

    std::atomic<size_t> n_bricks_{0};
    std::atomic<unsigned long long> searches_{0}, cells_searched_{0}, cells_evaluated_{0}, no_sky_{0}, cells_reset_{0};
    std::atomic<unsigned long long> us_{0};
};

}  // namespace engine
