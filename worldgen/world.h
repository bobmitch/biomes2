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
    FEATURE_HEIGHT    = 6u
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
    float cell_area_target  = 0.27e6f;
    int   grid              = 1024;       // sample grid per side

    // layer 0 stand-in
    float continent_freq    = 1.0f / 5200.0f;
    int   continent_octaves = 5;
    float centre_floor      = 0.20f;      // home-landmass floor, not a lift
    float edge_drop         = 1.60f;      // B
    float sea_level         = 0.0f;
    // Stand-in only: shifts the continent mask to hit the 55% land target.
    // In the real generator this is sea level, which is a world constant.
    float land_offset       = 0.17f;   // calibrated to the 55% land target

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
};

// Thin adapters onto the canonical field function in worldgen/shaders/field.h.
// The arithmetic lives there so the harness, the engine and mapview cannot drift
// apart -- these only unpack WorldParams into the scalars the dialect takes.
static inline float layer0_height(float x, float z, float r, const WorldParams& wp, uint32_t seed) {
    return ::layer0_height(x, z, r, wp.continent_freq, wp.continent_octaves,
                           wp.centre_floor, wp.edge_drop, wp.land_offset, seed);
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
    int nearest4(const std::vector<std::pair<float, float> >& pts, float x, float z,
                 int* idx, float* dist) const {
        float best_d[4] = {3.4e38f, 3.4e38f, 3.4e38f, 3.4e38f};
        int   best_i[4] = {-1, -1, -1, -1};
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
                    for (int s = 0; s < 4; ++s) {
                        if (d2 < best_d[s]) {
                            for (int t = 3; t > s; --t) {
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
                   const ArchetypeTable& table, float x, float z, float r);

// Distance to the nearest river segment, with that channel's shape.
float nearest_river(const SeedWorld& w, const WorldParams& wp,
                    float x, float z, float& out_w, float& out_d);

// Ground height with the drainage carved in.
float terrain_height(const SeedWorld& w, const WorldParams& wp,
                     const ArchetypeTable& table, float x, float z, float r);

void build_rivers(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table);

// One seed, from scatter through quota assignment to drainage.
SeedWorld build_world(uint64_t world_seed, const WorldParams& wp,
                      const ArchetypeTable& table, const AssignParams& ap);

}  // namespace worldgen
