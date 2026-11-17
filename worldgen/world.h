// ---------------------------------------------------------------------------
// The generator. Everything upstream of the golden hash lives here: the field
// function, the per-feature seeding, the layer-2 cell scatter, the world build
// and the carved terrain.
//
// It is here rather than in tools/validate because both the harness and the
// viewer need it, and because keeping hash-feeding code in one place is what
// makes CLAUDE.md invariant 9 checkable -- the metrics that legitimately use
// atan and cos/sin stay on the other side of this boundary.
//
// worldgen/ imports nothing from tools/ or engine/ (invariant 1).
// ---------------------------------------------------------------------------
#pragma once

#include "worldgen/assign/assign.h"
#include "worldgen/assign/archetypes.h"
#include "worldgen/shaders/field.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace worldgen {

// Per-feature seeds: hash64(world_seed ^ FEATURE_ID). Retuning one layer must
// not reshuffle any other layer (invariant 4).
enum FeatureId : uint32_t {
    FEATURE_CONTINENT = 1u,
    FEATURE_DANGER    = 2u,
    FEATURE_CELLS     = 3u,
    FEATURE_WARP_X    = 4u,
    FEATURE_WARP_Z    = 5u,
    FEATURE_HEIGHT    = 6u,
    FEATURE_OVERHANG  = 7u,
    FEATURE_CAVE      = 8u,
    FEATURE_ENTRANCE  = 9u,
    FEATURE_TREES     = 10u,   // which cells hold a tree, and its rolls
    FEATURE_TREE_PATCH = 11u,  // the patch / clearing noise
    FEATURE_ROCKS     = 12u,   // which cells hold a rock, and its rolls
    FEATURE_ROCK_PATCH = 13u   // the rock patch noise
};

inline uint64_t hash64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

inline uint32_t feature_seed(uint64_t world_seed, uint32_t feature_id) {
    return static_cast<uint32_t>(hash64(world_seed ^ static_cast<uint64_t>(feature_id)) >> 32);
}

// Deterministic RNG for the cell scatter. Host-only: layer 2 is a world-init
// preprocess, not a per-sample function.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t s) : state(hash64(s)) {}
    uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        const uint32_t xs  = static_cast<uint32_t>(((state >> 18) ^ state) >> 27);
        const uint32_t rot = static_cast<uint32_t>(state >> 59);
        return (xs >> rot) | (xs << ((32u - rot) & 31u));
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }
};

// --- world parameters ------------------------------------------------------
struct WorldParams {
    float radius            = 10000.0f;   // R, metres
    // m^2, held constant across R. 0.32e6 rather than the 1.28e6 the scale
    // table implies: the rule of thumb assumes a centred quota error, and it
    // is not centred, so R = 10 km needs about 4x the cells. R = 20 km passes
    // at either figure.
    float cell_area_target  = 0.12e6f;
    int   grid              = 1024;       // sample grid per side

    // layer 0 stand-in
    float continent_freq    = 1.0f / 5200.0f;
    int   continent_octaves = 5;
    float centre_floor      = 0.20f;      // home-landmass floor, not a lift
    float edge_drop         = 1.60f;      // B
    float sea_level         = 0.0f;
    // Stand-in only: shifts the continent mask to hit the 55% land target.
    // In the real generator this is sea level, which is a world constant.
    // Solved globally for a 35% MEAN land fraction, then baked. Seeds run
    // about 31-41% around it, and that spread is where the islandness is:
    // the archipelagos are its low tail, not a separate mode.
    float land_offset       = -0.0248f;

    // layer 1
    float danger_w          = 0.60f;      // the feel parameter; see tools/validate/README.md
    float danger_freq       = 1.0f / 2000.0f;
    int   danger_octaves    = 3;

    // layer 2
    float warp_amp          = 1300.0f;    // ~one cell radius
    float warp_freq         = 1.0f / 6000.0f;

    // layer 4
    float continent_scale   = 140.0f;     // metres per unit of continent mask
    float biome_freq        = 1.0f / 900.0f;
    float blend_width       = 500.0f;     // metres of cross-boundary height blend

    // layer 5
    int   river_grid        = 256;        // the doc's figure; ample over the disc
    float river_threshold   = 70.0f;      // upstream cells before it is a river

    // layer 6
    float overhang_freq     = 1.0f / 50.0f;
    float overhang_scale    = 30.0f;      // metres at strength 1; the ratio to the feature size is what makes an overhang possible
    float cave_freq         = 1.0f / 260.0f;
    float cave_carve        = 10.0f;      // metres of air margin inside a tunnel

    // layer 7 -- the way in. See the header comment in shaders/field.h for why
    // the caves need one at all.
    int   entrance_target    = 24;        // shafts per world, when candidates allow
    int   entrance_tries     = 240;       // candidate mouths before giving up on the target
    float entrance_step      = 4.0f;      // metres per step of the descent walk
    int   entrance_max_steps = 90;        // 360 m of path before the candidate is discarded
    // Air shallower than this is an overhang, not a cave. Without it a shaft
    // terminates one step in, under the lip it started on.
    float entrance_min_depth = 10.0f;
    float entrance_mouth     = 8.0f;      // bowl radius where it breaks the surface
    float entrance_throat    = 2.4f;
    float entrance_bowl      = 1.8f;      // extra radius at a chamber
    float entrance_bowls     = 3.5f;      // chambers along the shaft
    float entrance_falloff   = 1.5f;      // metres of feathering at the tube wall
    float entrance_branch_p  = 0.6f;      // chance of a dead-end side passage
};

// Thin adapters onto the canonical field function in worldgen/shaders/field.h.
// The arithmetic lives there so the harness, the engine and mapview cannot drift
// apart -- these only unpack WorldParams into the scalars the dialect takes.
// voxel is the LOD sample spacing in metres; 0 means full detail.
static inline float layer0_height(float x, float z, float r, const WorldParams& wp, uint32_t seed, float voxel = 0.0f) {
    return ::layer0_height(x, z, r, wp.continent_freq, wp.continent_octaves,
                           wp.centre_floor, wp.edge_drop, wp.land_offset, seed, voxel);
}

static inline float layer1_danger(float x, float z, float r, const WorldParams& wp, uint32_t seed) {
    return ::layer1_danger(x, z, r, wp.danger_w, wp.danger_freq, wp.danger_octaves, seed);
}

// --- cell lookup -----------------------------------------------------------
// The design doc's bucket grid, sized for ~2 cells per bucket. Shared by the
// rasteriser and the contact-sheet renderer so the two cannot disagree about
// which cell a point falls in.
struct CellLookup {
    int   B = 0;
    float bucket = 0.0f, R = 0.0f;
    std::vector<std::vector<int> > buckets;

    void build(const std::vector<std::pair<float, float> >& p, float radius) {
        R = radius;
        const int n = static_cast<int>(p.size());
        B = std::max(4, static_cast<int>(std::ceil(std::sqrt(n / 2.0))));
        bucket = 2.0f * R / B;
        buckets.assign(static_cast<size_t>(B) * B, std::vector<int>());
        for (int i = 0; i < n; ++i) {
            const int bx = std::min(B - 1, std::max(0, static_cast<int>((p[i].first  + R) / bucket)));
            const int bz = std::min(B - 1, std::max(0, static_cast<int>((p[i].second + R) / bucket)));
            buckets[static_cast<size_t>(bz) * B + bx].push_back(i);
        }
    }

    // The four nearest cells, for the layer-4 height blend. Returns how many
    // were found. A 5x5 bucket neighbourhood spans several cell spacings at
    // ~2 cells per bucket, so it reaches far enough; the ring search below is
    // the fallback when it does not.
    //
    // `dist5`, when asked for, is the distance to the FIFTH nearest. The blend
    // needs it: see blend_weight in field.h for why four cells were never
    // enough on their own.
    int nearest4(const std::vector<std::pair<float, float> >& pts, float x, float z,
                 int* idx, float* dist, float* dist5 = nullptr) const {
        float best_d[5] = {3.4e38f, 3.4e38f, 3.4e38f, 3.4e38f, 3.4e38f};
        int   best_i[5] = {-1, -1, -1, -1, -1};
        const int bx = std::min(B - 1, std::max(0, static_cast<int>((x + R) / bucket)));
        const int bz = std::min(B - 1, std::max(0, static_cast<int>((z + R) / bucket)));
        for (int j = bz - 2; j <= bz + 2; ++j) {
            if (j < 0 || j >= B) continue;
            for (int i = bx - 2; i <= bx + 2; ++i) {
                if (i < 0 || i >= B) continue;
                const std::vector<int>& bk = buckets[static_cast<size_t>(j) * B + i];
                for (size_t m = 0; m < bk.size(); ++m) {
                    const float dx = pts[bk[m]].first - x, dz = pts[bk[m]].second - z;
                    const float d2 = dx * dx + dz * dz;
                    for (int s = 0; s < 5; ++s) {
                        if (d2 < best_d[s]) {
                            for (int t = 4; t > s; --t) {
                                best_d[t] = best_d[t - 1];
                                best_i[t] = best_i[t - 1];
                            }
                            best_d[s] = d2;
                            best_i[s] = bk[m];
                            break;
                        }
                    }
                }
            }
        }
        int found = 0;
        for (int s = 0; s < 4; ++s) {
            if (best_i[s] < 0) continue;
            idx[found] = best_i[s];
            dist[found] = std::sqrt(best_d[s]);
            ++found;
        }
        if (dist5) *dist5 = best_i[4] >= 0 ? std::sqrt(best_d[4]) : 3.4e38f;
        if (found == 0) {
            const int one = nearest(pts, x, z);
            if (one >= 0) {
                idx[0] = one;
                const float dx = pts[one].first - x, dz = pts[one].second - z;
                dist[0] = std::sqrt(dx * dx + dz * dz);
                found = 1;
            }
        }
        return found;
    }

    // pts passed in rather than held, so a copied or moved CellLookup cannot
    // end up pointing at a dead vector.
    int nearest(const std::vector<std::pair<float, float> >& pts, float x, float z) const {
        const int bx = std::min(B - 1, std::max(0, static_cast<int>((x + R) / bucket)));
        const int bz = std::min(B - 1, std::max(0, static_cast<int>((z + R) / bucket)));
        int   best = -1;
        float best_d2 = 3.4e38f;
        for (int ring = 0; ring < B; ++ring) {
            if (best >= 0) {
                const float safe = static_cast<float>(ring - 1) * bucket;
                if (safe > 0.0f && safe * safe > best_d2) break;
            }
            const int x0 = bx - ring, x1 = bx + ring, z0 = bz - ring, z1 = bz + ring;
            for (int j = z0; j <= z1; ++j) {
                if (j < 0 || j >= B) continue;
                for (int i = x0; i <= x1; ++i) {
                    if (i < 0 || i >= B) continue;
                    // only the ring shell
                    if (ring > 0 && i != x0 && i != x1 && j != z0 && j != z1) continue;
                    const std::vector<int>& bk = buckets[static_cast<size_t>(j) * B + i];
                    for (size_t m = 0; m < bk.size(); ++m) {
                        const float dx = pts[bk[m]].first - x, dz = pts[bk[m]].second - z;
                        const float d2 = dx * dx + dz * dz;
                        if (d2 < best_d2) { best_d2 = d2; best = bk[m]; }
                    }
                }
            }
        }
        return best;
    }
};

// Everything one seed produces, up to and including the assignment. The
// metrics, the contact sheet and the adjacency test all read this rather than
// rebuilding the world three different ways.
struct SeedWorld {
    std::vector<std::pair<float, float> > pts;
    CellLookup              lookup;
    std::vector<CellRecord> cells;
    CellGraph               graph;
    std::vector<int>        biome;
    AssignStats             stats;
    uint32_t s_cont = 0, s_dang = 0, s_wx = 0, s_wz = 0, s_height = 0;
    uint32_t s_overhang = 0, s_cave = 0, s_entrance = 0;
    uint32_t s_trees = 0, s_tree_patch = 0;
    uint32_t s_rocks = 0, s_rock_patch = 0;
    float    land_fraction = 0.0f;

    // --- layer 5 ---
    // The drainage network as segments, each from a grid cell to the one it
    // flows into, plus a uniform grid binning them so the carve only tests
    // the few that could be near a sample.
    struct RiverSeg {
        float ax, az, bx, bz;
        float width, depth;
    };
    std::vector<RiverSeg> rivers;
    int   rgrid_n = 0;
    float rgrid_cell = 0.0f;
    std::vector<std::vector<int> > rgrid;
    int   river_paths_to_ocean = 0;
    int   river_paths_total = 0;
    int   river_ascending = 0;   // segments that climb on the filled surface
    // --- layer 7 ---
    // Shafts from the surface into the cave system, as 3D polylines with a
    // radius at each vertex, binned in XZ the way the rivers are. A trunk is
    // only stored if its walk ended in cave air, so entrance_count is a count
    // of connections, not of attempts.
    struct EntranceSeg {
        float ax, ay, az, ra;
        float bx, by, bz, rb;
    };
    std::vector<EntranceSeg> entrances;
    // Where each trunk breaks the surface, and how deep it had to go to find
    // air. The segment list cannot answer this after the fact: a shaft that
    // drifts sideways under lower ground has mid-shaft vertices above the
    // local surface too, and they look exactly like mouths.
    struct EntranceMouth {
        float x, y, z;
        float reach;               // metres below the surface where it broke through
        int   seg_begin, seg_count;  // this trunk's run in entrances[]; branches follow it
    };
    std::vector<EntranceMouth> entrance_mouths;
    int   entrance_count = 0;      // trunks; each reaches cave air by construction
    int   entrance_tries_used = 0;
    int   egrid_n = 0;
    float egrid_cell = 0.0f;
    std::vector<std::vector<int> > egrid;

    // Highest layer-0 sample beyond r = 0.95. The edge taper assertion: land
    // must have fallen away before the rim, on every seed.
    float    max_h_outer = -1.0e9f;
};

// FNV-1a. The determinism hash is a CPU-build artifact by ADR-001: WGSL
// permits reassociation and FMA fusion, so a GPU result cannot be pinned.
inline uint64_t fnv1a(uint64_t h, uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        h ^= (v >> (b * 8)) & 0xFFull;
        h *= 0x100000001B3ull;
    }
    return h;
}

// --- generator entry points -------------------------------------------------

std::vector<std::pair<float, float> > scatter_cells(const WorldParams& wp, uint32_t seed);

// Ground height before the drainage is carved. build_rivers traces against
// this; carving the surface the rivers were traced on would feed back.
float terrain_base(const SeedWorld& w, const WorldParams& wp,
                   const ArchetypeTable& table, float x, float z, float r,
                   float voxel = 0.0f);

// Distance to the nearest river segment, with that channel's shape.
float nearest_river(const SeedWorld& w, const WorldParams& wp,
                    float x, float z, float& out_w, float& out_d);

// Ground height with the drainage carved in.
float terrain_height(const SeedWorld& w, const WorldParams& wp,
                     const ArchetypeTable& table, float x, float z, float r,
                     float voxel = 0.0f);

void build_rivers(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table);

// Signed distance to the nearest entrance shaft: negative inside one. Large
// positive when the world has none near the sample.
float nearest_entrance(const SeedWorld& w, const WorldParams& wp,
                       float x, float y, float z);

// Whether any biome carves caves. Off (every cave_density 0) means layer 6
// carves nothing and layer 7 builds no shafts: a shaft is only kept if its
// walk ends in cave air, so with no caves all 240 candidate walks would run
// their 360 m and find nothing. Read by validate and the engine too, so the
// whole pipeline agrees on what "caves off" means.
bool caves_enabled(const ArchetypeTable& table);

// Layer 7. Runs after the drainage, because the walk descends against the
// finished heightfield, and writes into w.entrances only once every walk is
// done -- a half-built buffer would let one shaft terminate in another.
// Builds nothing when !caves_enabled(table).
void build_entrances(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table);

// The biomes near a point and their normalised blend weights -- the same
// weights layer 4 mixes heights with, divided by blend_divisor, so a biome
// property blended through this fades at a coast exactly as the height does.
// Returns how many entries are filled; biome < 0 entries never appear.
int biome_mix(const SeedWorld& w, const WorldParams& wp, float x, float z,
              int* biome, float* weight);

// --- layer 8: trees -----------------------------------------------------------
// A tree is a pure function of its candidate cell (see field.h), so the id
// below is all a save needs to name one. Rotation is stored as a fraction of
// a turn: turning it into sin and cos is presentation, on the far side of the
// hash.
struct TreeInstance {
    float    x, y, z;        // trunk base on the ground, metres
    float    height;         // metres, stunting included
    float    yaw01;          // [0, 1) of a full turn
    float    lean01;         // [0, 1), the engine's to interpret
    uint32_t id;             // tree_id(i, j)
    uint16_t species;        // index into TreeTable::species
    uint16_t variant;        // which of the engine's meshes for the species, 0..255
};

inline uint32_t tree_id(int i, int j) {
    return (static_cast<uint32_t>(i + 32768) << 16) | static_cast<uint32_t>(j + 32768);
}
inline void tree_cell(uint32_t id, int& i, int& j) {
    i = static_cast<int>(id >> 16) - 32768;
    j = static_cast<int>(id & 0xFFFFu) - 32768;
}

// Every tree whose candidate cell (i, j) lies in [i0, i1) x [j0, j1), appended
// to `out` in cell order. Cell (i, j) covers [i*cell, (i+1)*cell) in x, so
// tiles that are whole multiples of the cell never share or miss one.
void trees_in_cells(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                    int i0, int j0, int i1, int j1, std::vector<TreeInstance>& out);

// Why a candidate cell holds no tree. validate --tree-reasons tabulates these
// per biome; a density that comes out far below the table's is otherwise a
// guess between seven different filters.
enum TreeReject {
    TREE_KEPT = 0, TREE_OUTSIDE, TREE_NO_LAND, TREE_CLEARING, TREE_ROLL,
    TREE_SHORE, TREE_SLOPE, TREE_RIVER, TREE_ALTITUDE, TREE_FIT_ROLL, TREE_SURFACE,
    TREE_REJECT_COUNT
};

// The one tree a cell holds, if any. Same answer trees_in_cells gives.
bool tree_at_cell(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                  int i, int j, TreeInstance& out, int* reason = nullptr);

// --- layer 9: rocks -------------------------------------------------------------
// Boulders and outcrops, each a pure function of its candidate cell like a
// tree, and pinned the same way. Two classes on their own grids: boulders
// (kind 0) on small cells, outcrops (kind 1) on large ones that want a slope.
// The engine draws, textures (by `biome`'s rock scan) and collides them.
struct RockInstance {
    float    x, y, z;        // the rock's origin: centre of its base, set `sink` below its footprint's lowest ground
    float    ground;         // the real surface at (x, z)
    float    size;           // metres across
    float    yaw01;          // [0, 1) of a full turn
    float    tilt01;         // [0, 1), the engine's to interpret
    uint32_t id;             // rock_id(kind, i, j)
    uint8_t  kind;           // 0 boulder, 1 outcrop
    uint8_t  variant;        // which of the engine's meshes, 0..255
    uint16_t biome;          // the dominant biome where it stands: its material
};

inline uint32_t rock_id(int kind, int i, int j) {
    return (static_cast<uint32_t>(kind & 1) << 31) |
           ((static_cast<uint32_t>(i + 16384) & 0x7FFFu) << 16) |
           (static_cast<uint32_t>(j + 32768) & 0xFFFFu);
}

enum RockReject {
    ROCK_KEPT = 0, ROCK_OUTSIDE, ROCK_NO_LAND, ROCK_CLEARING, ROCK_ROLL, ROCK_SHORE,
    ROCK_SLOPE, ROCK_RIVER, ROCK_SURFACE, ROCK_TREE, ROCK_OUTCROP,
    ROCK_REJECT_COUNT
};

// The one rock of `kind` a cell holds, if any.
bool rock_at_cell(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                  int kind, int i, int j, RockInstance& out, int* reason = nullptr);

// Every rock of `kind` whose cell lies in [i0, i1) x [j0, j1), in cell order.
void rocks_in_cells(const SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table,
                    int kind, int i0, int j0, int i1, int j1, std::vector<RockInstance>& out);

// Layer 6. Positive is rock, negative is air; the sign change is the surface.
float terrain_density(const SeedWorld& w, const WorldParams& wp,
                      const ArchetypeTable& table, float x, float y, float z,
                      float voxel = 0.0f);

// One seed, from scatter through quota assignment to drainage.
SeedWorld build_world(uint64_t world_seed, const WorldParams& wp,
                      const ArchetypeTable& table, const AssignParams& ap);

}  // namespace worldgen
