#pragma once
// ---------------------------------------------------------------------------
// Sea access (ADR-006 stage 3): which air below sea level is SEA.
//
// The sea was a column rule -- water wherever the GENERATED height is below
// sea level -- so it ignored every edit and every cave: a channel dug in
// from the coast met a wall of water at the old shoreline, a hole dug
// through into the sea stayed dry, a tunnel breached from the seabed stayed
// dry. This labels 0.5 m cells below sea level instead, near where the player
// has dug or a placed cave runs, on its own thread, as sky_access.h labels
// air by its path to the sky:
//
//  - THE EDIT LATTICE ITSELF. A cell is one 0.5 m lattice point, air or
//    rock as the LOD 0 mesher reads it there (the edit, else the field at a
//    0.5 m voxel), and water passes between two neighbouring points that are
//    both air -- so a wall the mesh draws stops it and a gap the mesh shows
//    lets it through: the ADR's "solid blocks water completely, no
//    seepage", at the resolution the player digs at. The ADR's first plan,
//    1 m cells open only where all 27 lattice points in them were air, was
//    built and measured: a 3 m shaft dug off the grid held no such cell,
//    and the water could not get down it.
//  - THE OPEN SEA is where it always was: air below sea level with nothing
//    but air straight above it to the open water, in a column whose
//    generated height is below sea level (the shader's own rule). Those
//    cells are sea by construction and the search stops at them, so it
//    never walks the whole seabed.
//  - CONNECTION. From a cell, its component of air is followed (6-connected,
//    at any height, through caves and up shafts) until it meets the open sea
//    -- then it is connected -- or exhausts. Open air ABOVE sea level stops
//    the search too: it can hold no water, only let air out.
//  - VENTING. Water fills a connected cell only if the air in it can get
//    out: if from an opening -- the open sea's surface, or air open to the
//    sky -- there is a way to it that never climbs, i.e. a way for its air
//    to rise to the opening. A dome breached from below keeps its air: a
//    pocket to breathe in. A tunnel dug from the land and breached at the
//    far end floods to sea level, its air out through the shaft.
//  - DISTANCE, by path through the water to the open sea, for calming the
//    waves in a channel or a flooded tunnel.
//
// A label: 0 dry (air that is not sea, or above sea level), 255 solid,
// 1..254 sea, its path to the open sea + 1 (capped). A component bigger than
// the cap is left dry: kMaxComponent.
//
// Requests are blocks (8 m), from the main thread, near the player; results
// carry the edit generation, and an edit forgets every label (a breach can
// change a component of any size) and re-evaluates only the bricks it
// touched. No edits and no caves below sea level near the player: nothing is
// asked, and the column rule stands. Presentation and physics only; nothing
// here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

class BrickStore;
class WorldGpu;

class SeaAccess {
public:
    static const int kBrick = 16;                      // cells a side: 8 m
    static const int kCells = kBrick * kBrick * kBrick;
    static constexpr float kCellM = 0.5f;              // the edit lattice
    static const uint8_t kDry = 0;
    static const uint8_t kSolid = 255;
    static const size_t kMaxComponent = 1000000;

    struct Result {
        int b[3];
        uint64_t gen;
        uint8_t cell[kCells];   // (x + 16 y + 256 z): kDry, kSolid, or 1 + cells (0.5 m) to the open sea
        // THE FILL (ADR-006, the shown fill): when the water reaches a wet
        // cell after the edit that let it in, in kArriveStep seconds from the
        // moment the answer lands (255: that or later). 0 for water that was
        // there before the edit, and for anything labelled away from the
        // latest edit (approached, loaded): only a breach is watched filling.
        uint8_t arrive[kCells];
    };
    static constexpr float kArriveStep = 0.1f;
    // Seconds for the water to cross one 0.5 m cell: falling, spreading
    // along a floor, and rising (which a cell does once the water has pooled
    // below it -- approximated by a slower step up). A Dijkstra search from
    // the breach over the wet cells with these step costs.
    static constexpr float kFillDown = 0.05f, kFillSide = 0.08f, kFillUp = 0.16f;
    // Those are for a breach of kBreachRef m^2 (a 2 m hole). The flow in
    // goes as the breach's open area, so every step is scaled by
    // kBreachRef / area, held to [kBreachFast, kBreachSlow]: a pinhole
    // trickles, a collapsed seabed floods at once.
    static constexpr float kBreachRef = 4.0f, kBreachFast = 0.25f, kBreachSlow = 8.0f;

    ~SeaAccess() { shutdown(); }
    void init(const WorldGpu* world, const BrickStore* edits, float sea_level);
    void shutdown();
    float sea_level() const { return sea_; }

    // --- the main thread ---------------------------------------------------
    std::unique_lock<std::shared_mutex> edit_lock() { return std::unique_lock<std::shared_mutex>(edits_mutex_); }
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();
    uint64_t generation() const { return gen_; }
    void request_block(int bx, int by, int bz);
    void take_results(std::vector<Result>& out);
    void wait_idle();
    size_t queued() const;
    // Whether the world has placed caves at all (a column's caves are then
    // asked of worldgen by the caller).
    bool placed_caves() const { return placed_; }

    // Diagnostics over the life of the object.
    unsigned long long components() const { return components_.load(); }
    unsigned long long animated() const { return animated_.load(); }   // components whose fill was timed
    // The last timed breach: its open area (m^2) and the time scale it gave.
    float last_breach_m2() const { return last_breach_m2_.load(); }
    float last_breach_scale() const { return last_breach_scale_.load(); }
    unsigned long long capped() const { return capped_.load(); }
    unsigned long long cells_searched() const { return cells_searched_.load(); }
    unsigned long long points_evaluated() const { return points_evaluated_.load(); }
    double ms() const { return double(us_.load()) / 1000.0; }

private:
    enum : uint8_t { kRock = 0, kAir = 1, kSeaFixed = 2, kSkyFixed = 3 };
    struct Brick {
        uint8_t st[kCells];
        uint8_t label[kCells];
        uint8_t arrive[kCells];
        uint64_t known[kCells / 64];
    };
    struct Column {
        float h[kBrick * kBrick];
        float reach = 0.0f;
        float hmin = 0.0f, hmax = 0.0f;
        float cave_lo = 3.4e38f;
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

    void run();
    void answer(std::vector<Cmd>& blocks);
    void component(int x, int y, int z);
    void forget(const float lo[3], const float hi[3]);
    const Column& column(int bx, int bz);
    Brick* brick(int bx, int by, int bz);
    Brick* brick_of(int x, int y, int z, int& idx);
    void evaluate_batch(const std::vector<int>& bxyz);
    float lattice(int lx, int ly, int lz) const;
    bool has_edits_locked(int bx, int by, int bz) const;
    int open_floor(int x, int z);     // the lowest cell of the column with air straight above to the open
    bool below_sea(int y) const { return float(y) * kCellM + 0.25f <= sea_; }
    float column_h(int x, int z);

    const WorldGpu* world_ = nullptr;
    const BrickStore* edits_ = nullptr;
    float sea_ = 0.0f;
    bool placed_ = false;
    bool caves_ = false;
    float cave_depth_ = 0.0f;
    std::unordered_map<int64_t, Brick> bricks_;
    std::unordered_map<int64_t, Column> columns_;
    std::unordered_map<int64_t, int> floor_;
    uint64_t worker_gen_ = 0;
    // The fill: cells wet before the latest edit (their water is already
    // there: arrival 0), and the edit itself (only a component touching it
    // is watched filling; any other is shown whole at once).
    std::unordered_set<int64_t> was_wet_;
    bool  have_edit_ = false;
    float edit_lo_[3] = {0, 0, 0}, edit_hi_[3] = {0, 0, 0};

    mutable std::shared_mutex edits_mutex_;
    mutable std::mutex mutex_;
    std::condition_variable cv_, idle_cv_;
    std::deque<Cmd> queue_;
    std::vector<Result> results_;
    bool busy_ = false;
    bool stop_ = false;
    std::thread thread_;
    std::atomic<uint64_t> gen_{1};

    std::atomic<unsigned long long> animated_{0};
    std::atomic<float> last_breach_m2_{0.0f}, last_breach_scale_{1.0f};
    std::atomic<unsigned long long> components_{0}, capped_{0}, cells_searched_{0}, points_evaluated_{0};
    std::atomic<unsigned long long> us_{0};
};

}  // namespace engine
