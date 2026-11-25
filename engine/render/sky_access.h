#pragma once
// ---------------------------------------------------------------------------
// Sky access: which underground air can reach the open sky at all.
//
// Step 1 of the underground GI plan (a flood over air as the base layer, the
// near field's probes as the detail): a CPU flood over 1 m cells that labels
// every air cell under the top surface OPEN (connected through air to open
// sky) or SEALED (not). The near field reads it per 8 m block as a mask
// (near_field.h): a ray that leaves its 128 m volume through sealed air
// carries back darkness instead of the heightfield's sky. Before this, a
// sealed cave longer than the volume was lit by daylight through the
// volume's edge (the 42 m chamber of the cave measurement: 4 nits at noon,
// 0 at 23:00, in air a flood fill proved sealed).
//
// The cells are the field as the near field's GPU fill evaluates it -- the
// CPU's terrain_density at a 1 m voxel -- and the player's edits at the 1 m
// points, so the two agree to the field's CPU/GPU tolerance. Air higher than
// the column's height plus the overhang term's largest reach there is open
// by construction: layer 6 can put no rock above it. A search from an
// unresolved air cell goes highest-first through connected air; reaching
// open air labels everything it visited open, exhausting the component
// labels it sealed, and a component bigger than kMaxSearch without an
// opening is called sealed (dark: deep unlit air would be anyway).
//
// No caves and no edits: nothing to search -- a block's mask is zero
// without touching the field, so the shipping world (caves off) pays only
// where the player has dug.
//
// Not done (step 2 of the plan): directional sky visibility with distance,
// on the GPU, built as regions stream in; a local re-flood after an edit
// (an edit forgets every label, and searches run synchronously in plan, so
// a dig beside a big sealed cave hitches while it is searched again).
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <cstdint>
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

    void init(const WorldGpu* world);
    // The edits changed inside a world box: its bricks are evaluated again
    // and every label is forgotten (connectivity may have changed anywhere).
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();
    // Bumped by every invalidation: the near field re-marks what it holds.
    uint64_t generation() const { return generation_; }

    // World block (bx, by, bz), 8 m units: bit (x + 8 y + 64 z) set where
    // that 1 m cell is air with no path to the open sky. Resolves whatever
    // it has to, synchronously. Returns false (mask zero) when the block can
    // hold no sealed air without looking (no caves, no edits).
    bool block_mask(int bx, int by, int bz, const BrickStore* edits, uint32_t mask[kMaskWords]);
    // Whether the 1 m cell nearest a point is sealed air (rock and open air:
    // false). A line of sight from sealed air stays in it until it meets
    // rock, so a camera in sealed air sees nothing else -- the mist and the
    // haze past the near field's volume take that (near_field.h constants).
    bool sealed_at(float x, float y, float z, const BrickStore* edits);

    // Diagnostics over the life of the object.
    size_t bricks() const { return bricks_.size(); }
    unsigned long long searches() const { return searches_; }
    unsigned long long cells_searched() const { return cells_searched_; }
    unsigned long long cells_evaluated() const { return cells_evaluated_; }
    unsigned long long sealed_components() const { return sealed_; }
    double ms() const { return ms_; }

private:
    enum : uint8_t { kRock = 0, kUnknown = 1, kOpen = 2, kSealed = 3, kSearching = 4 };
    struct Brick {
        uint8_t st[kCells];
        bool has_air = false;
    };
    struct Column {
        float h[kBrick * kBrick];   // the column's height at a 1 m voxel (terrain_height)
        float reach = 0.0f;         // the overhang term's largest reach here, + 1 m
        float hmin = 0.0f, hmax = 0.0f;
    };
    static int64_t key(int bx, int by, int bz) {
        return (int64_t(bx + (1 << 20)) << 42) | (int64_t(by + (1 << 20)) << 21) | int64_t(bz + (1 << 20));
    }
    // False when block (bx, by, bz) can hold no sealed air without looking:
    // no caves and no edits, or wholly above or below where they reach.
    bool may_seal(int bx, int by, int bz, const BrickStore* edits);
    const Column& column(int bx, int bz);
    Brick* brick(int bx, int by, int bz);   // evaluated on first use
    void evaluate(int bx, int by, int bz, Brick& b);
    void evaluate_batch(const std::vector<int>& bxyz);   // several bricks, in parallel
    uint8_t* cell(int x, int y, int z);                   // evaluating its brick if needed
    void search(int x, int y, int z);
    float density(int x, int y, int z) const;

    const WorldGpu* world_ = nullptr;
    const BrickStore* edits_ = nullptr;   // this call's (block_mask)
    bool caves_ = false;
    float cave_depth_ = 0.0f;              // the deepest cave_depth_max of any caving biome, + margin
    float max_strength_ = 0.0f;
    std::unordered_map<int64_t, Brick> bricks_;
    std::unordered_map<int64_t, Column> columns_;
    uint64_t generation_ = 1;
    unsigned long long searches_ = 0, cells_searched_ = 0, cells_evaluated_ = 0, sealed_ = 0;
    double ms_ = 0.0;
};

}  // namespace engine
