#pragma once
// ---------------------------------------------------------------------------
// Sky access: which underground air can reach the open sky at all.
//
// Step 1 of the underground GI plan (a flood over air as the base layer, the
// near field's probes as the detail): a CPU flood over 1 m cells that labels
// every air cell under the top surface OPEN (connected through air to open
// sky) or SEALED (not). The near field reads it per 8 m block as a mask
// (near_field.h): a ray that leaves its 128 m volume through sealed air
// carries back darkness instead of the heightfield's sky, a camera in sealed
// air sees no daylit mist, and a probe in sealed air with no light near does
// not trace.
//
// The cells are the field as the near field's GPU fill evaluates it -- the
// CPU's terrain_density at a 1 m voxel -- and the player's edits at the 1 m
// points, so the two agree to the field's CPU/GPU tolerance. Air higher than
// the column's height plus the overhang term's largest reach there (and over
// anything the player built) is open by construction: layer 6 can put no
// rock above it. A search from an unlabelled air cell goes highest-first
// through connected air; reaching open air labels everything it visited
// open, exhausting the component labels it sealed, and a component bigger
// than kMaxSearch without an opening is called sealed (dark: deep unlit air
// would be anyway).
//
// ON ITS OWN THREAD. It ran in NearField::plan, and a cave world's first
// sealed cave cost the frame 0.2-0.8 s. Now the main thread posts requests
// (a block's mask, the camera's cell, an edit) and takes results; the worker
// owns every label. Results carry the generation of the last edit they saw,
// and the main thread drops any older than its own. The edit store is read
// only while a batch of bricks is evaluated, under a shared lock that the
// two writers (a dig, a load) take exclusively for their moment.
//
// An edit forgets only what it can have changed: the labels of every
// component of air within a cell of it (a dig can join one to the sky, a
// fill can seal one), found by a fill through the labels themselves, and the
// bricks it touched, evaluated again. It used to forget every label, so each
// dig beside a big sealed cave searched the whole cave again.
//
// No caves and no edits: nothing to search -- the near field asks nothing,
// so the shipping world (caves off) pays only where the player has dug.
//
// Not done (step 2 of the plan): directional sky visibility with distance,
// on the GPU, built as regions stream in.
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
    static const int kMaskWords = kCells / 32;
    static const size_t kMaxSearch = 4u << 20;         // cells one search may visit

    struct Result {
        int b[3];                     // world block, 8 m units
        uint64_t gen;                 // the edit generation it was made under
        uint32_t mask[kMaskWords];    // bit (x + 8 y + 64 z): sealed air
    };

    ~SkyAccess() { shutdown(); }
    // Starts the worker. `edits` is read under the edit lock only.
    void init(const WorldGpu* world, const BrickStore* edits);
    void shutdown();

    // --- the main thread ---------------------------------------------------
    // Held by whoever writes the edit store, for the write.
    std::unique_lock<std::shared_mutex> edit_lock() { return std::unique_lock<std::shared_mutex>(edits_mutex_); }
    // The edits changed inside a world box. Results made before it are stale.
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();
    uint64_t generation() const { return gen_; }
    // Whether block (bx, by, bz) can hold sealed air at all, without the
    // field: caves anywhere in the world, or an edit in it. The caller is
    // the edit store's writer, so it reads it without the lock.
    bool may_seal_cheap(int bx, int by, int bz) const;
    void request_block(int bx, int by, int bz);
    // The camera's cell, coalesced: only the latest is answered.
    void request_camera(float x, float y, float z);
    void take_results(std::vector<Result>& out);
    // Whether the camera stood in sealed air, at the last answer; false
    // until one under the current generation has come back.
    bool camera_sealed() const { return cam_gen_.load() == gen_ && cam_sealed_.load(); }
    // Until every request posted so far is answered (a still).
    void wait_idle();
    size_t queued() const;

    // Diagnostics over the life of the object (approximate while it runs).
    size_t bricks() const { return n_bricks_.load(); }
    unsigned long long searches() const { return searches_.load(); }
    unsigned long long cells_searched() const { return cells_searched_.load(); }
    unsigned long long cells_evaluated() const { return cells_evaluated_.load(); }
    unsigned long long sealed_components() const { return sealed_.load(); }
    unsigned long long cells_reset() const { return cells_reset_.load(); }
    double ms() const { return double(us_.load()) / 1000.0; }

private:
    // kOpenFixed: open by construction (over the reach); never forgotten.
    enum : uint8_t { kRock = 0, kUnknown = 1, kOpen = 2, kSealed = 3, kSearching = 4, kOpenFixed = 5 };
    struct Brick {
        uint8_t st[kCells];
        bool has_air = false;
    };
    struct Column {
        float h[kBrick * kBrick];   // the column's height at a 1 m voxel (terrain_height)
        float reach = 0.0f;         // the overhang term's largest reach here, + 1 m
        float hmin = 0.0f, hmax = 0.0f;
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
    void block_mask(int bx, int by, int bz, uint32_t mask[kMaskWords]);
    bool may_seal(int bx, int by, int bz);
    bool sealed_at(float x, float y, float z);
    void forget(const float lo[3], const float hi[3]);
    const Column& column(int bx, int bz);
    Brick* brick(int bx, int by, int bz);   // evaluated on first use
    void evaluate_batch(const std::vector<int>& bxyz);   // several bricks, in parallel
    uint8_t* cell(int x, int y, int z);                   // evaluating its brick if needed
    void search(int x, int y, int z);
    float density(int x, int y, int z) const;
    bool has_edits_locked(int bx, int by, int bz) const;

    const WorldGpu* world_ = nullptr;
    const BrickStore* edits_ = nullptr;
    bool caves_ = false;
    float cave_depth_ = 0.0f;              // the deepest cave_depth_max of any caving biome, + margin
    std::unordered_map<int64_t, Brick> bricks_;      // the worker's
    std::unordered_map<int64_t, Column> columns_;    // the worker's
    uint64_t worker_gen_ = 0;                        // the worker's: the last invalidation it applied

    mutable std::shared_mutex edits_mutex_;
    mutable std::mutex mutex_;                       // the queue, the results, the camera request
    std::condition_variable cv_, idle_cv_;
    std::deque<Cmd> queue_;
    std::vector<Result> results_;
    bool cam_pending_ = false;
    float cam_[3] = {0.0f, 0.0f, 0.0f};
    uint64_t cam_req_gen_ = 0;
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<uint64_t> gen_{1};                   // the main thread's: bumped by every invalidation
    std::atomic<uint64_t> cam_gen_{0};
    std::atomic<bool> cam_sealed_{false};

    std::atomic<size_t> n_bricks_{0};
    std::atomic<unsigned long long> searches_{0}, cells_searched_{0}, cells_evaluated_{0}, sealed_{0}, cells_reset_{0};
    std::atomic<unsigned long long> us_{0};
};

}  // namespace engine
