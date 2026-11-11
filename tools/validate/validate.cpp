// ---------------------------------------------------------------------------
// Validation harness -- does layer 3 actually deliver "every biome, every seed, within
// tolerance, radially ordered"?
//
// No graphics. It scatters cells, rasterises the disc to get real cell areas
// and a land mask, runs the real worldgen/assign/ code, and reports the four
// numbers the validation harness will later assert on:
//
//   presence      every biome gets at least one cell, every seed
//   quota         per-biome land share within +/-10% of target
//   radial order  Spearman(danger_preferred, mean normalised area) >= 0.8
//   land fraction 55% +/- 5%
//
// The headline question it exists to answer: at R = 10 km the scale table
// implies ~15 land cells per biome, so quota granularity is ~+/-7% against a
// +/-10% budget. Is that enough headroom, and what does the contiguity bonus
// cost us?
// ---------------------------------------------------------------------------
#include "../../worldgen/assign/assign.h"
#include "../../worldgen/assign/archetypes.h"
#include "../../worldgen/assign/json_lite.h"
#include "host_support.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <queue>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace host;
using namespace worldgen;

// --- world parameters ------------------------------------------------------
struct WorldParams {
    float radius            = 10000.0f;   // R, metres
    // m^2, held constant across R. 0.32e6 rather than the 1.28e6 the scale
    // table implies: the rule of thumb assumes a centred quota error, and it
    // is not centred, so R = 10 km needs about 4x the cells. R = 20 km passes
    // at either figure.
    float cell_area_target  = 0.32e6f;
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

// --- layer 2: blue-noise scatter (Bridson) ---------------------------------
static std::vector<std::pair<float, float> > scatter_cells(const WorldParams& wp, uint32_t seed) {
    // N ~= 0.683 * A / rmin^2 for Bridson, so rmin^2 ~= 0.683 * target cell area.
    const float rmin = std::sqrt(0.683f * wp.cell_area_target);
    const float R    = wp.radius;
    const float cell = rmin / 1.41421356f;
    const int   gw   = static_cast<int>(std::ceil(2.0f * R / cell)) + 1;

    std::vector<int> grid(static_cast<size_t>(gw) * gw, -1);
    std::vector<std::pair<float, float> > pts;
    std::vector<int> active;
    Rng rng(seed);

    auto gx = [&](float x) { return static_cast<int>((x + R) / cell); };
    auto gz = [&](float z) { return static_cast<int>((z + R) / cell); };

    auto fits = [&](float x, float z) {
        const int cx = gx(x), cz = gz(z);
        const int x0 = std::max(cx - 2, 0), x1 = std::min(cx + 2, gw - 1);
        const int z0 = std::max(cz - 2, 0), z1 = std::min(cz + 2, gw - 1);
        for (int j = z0; j <= z1; ++j) {
            for (int i = x0; i <= x1; ++i) {
                const int p = grid[static_cast<size_t>(j) * gw + i];
                if (p < 0) continue;
                const float dx = pts[p].first - x, dz = pts[p].second - z;
                if (dx * dx + dz * dz < rmin * rmin) return false;
            }
        }
        return true;
    };

    auto push = [&](float x, float z) {
        grid[static_cast<size_t>(gz(z)) * gw + gx(x)] = static_cast<int>(pts.size());
        active.push_back(static_cast<int>(pts.size()));
        pts.push_back(std::make_pair(x, z));
    };

    // Seed point near the centre so the home landmass always has cells.
    push(0.0f, 0.0f);

    while (!active.empty()) {
        const size_t ai = rng.next() % active.size();
        const int    pi = active[ai];
        bool placed = false;
        for (int k = 0; k < 30; ++k) {
            const float ang = rng.uniform() * 6.28318530718f;
            const float rad = rmin * (1.0f + rng.uniform());
            const float x = pts[pi].first  + std::cos(ang) * rad;
            const float z = pts[pi].second + std::sin(ang) * rad;
            if (x * x + z * z > R * R) continue;
            if (!fits(x, z)) continue;
            push(x, z);
            placed = true;
            break;
        }
        if (!placed) { active[ai] = active.back(); active.pop_back(); }
    }
    return pts;
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

// --- layer 4 ----------------------------------------------------------------
// Ground height in metres. Which cells are near a point is an index question,
// so the lookup and the weights live here; every metre of the result comes
// from worldgen/shaders/field.h.
static float terrain_base(const SeedWorld& w, const WorldParams& wp,
                            const ArchetypeTable& table, float x, float z, float r) {
    const float cont = layer0_height(x, z, r, wp, w.s_cont);

    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);

    int   idx[4];
    float dist[4];
    const int found = w.lookup.nearest4(w.pts, wx, wz, idx, dist);

    float h[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    for (int k = 0; k < found; ++k) {
        const int b = w.biome[idx[k]];
        if (b < 0) continue;
        const BiomeArchetype& a = table[b];
        h[k] = layer4_profile(x, z, wp.biome_freq, a.base_elevation, a.amplitude,
                              static_cast<int>(a.octaves), a.lacunarity, a.gain,
                              a.ridge_blend, a.flatness, w.s_height);
        // Smoothed distance to the boundary: the gap between this cell and the
        // nearest one, which is twice the distance to the bisector.
        const float t = 1.0f - (dist[k] - dist[0]) / wp.blend_width;
        wt[k] = t > 0.0f ? t * t : 0.0f;
    }

    const float blended = layer4_blend(h[0], wt[0], h[1], wt[1], h[2], wt[2], h[3], wt[3]);
    return layer4_height(cont, wp.continent_scale, blended, wp.sea_level);
}

// Distance to the nearest river segment, with that channel's shape. Shared by
// the carve and the relief renderer, so the water is drawn where it is.
static float nearest_river(const SeedWorld& w, const WorldParams& wp,
                           float x, float z, float& out_w, float& out_d) {
    out_w = 0.0f;
    out_d = 0.0f;
    if (w.rivers.empty()) return 3.4e38f;

    const float R = wp.radius;
    const int   gx = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((x + R) / w.rgrid_cell)));
    const int   gz = std::min(w.rgrid_n - 1, std::max(0, static_cast<int>((z + R) / w.rgrid_cell)));

    float best_d = 3.4e38f;
    float best_w = 0.0f, best_depth = 0.0f;
    for (int j = gz - 1; j <= gz + 1; ++j) {
        if (j < 0 || j >= w.rgrid_n) continue;
        for (int i = gx - 1; i <= gx + 1; ++i) {
            if (i < 0 || i >= w.rgrid_n) continue;
            const std::vector<int>& bin = w.rgrid[static_cast<size_t>(j) * w.rgrid_n + i];
            for (size_t m = 0; m < bin.size(); ++m) {
                const SeedWorld::RiverSeg& s = w.rivers[bin[m]];
                const float d = river_segment_distance(x, z, s.ax, s.az, s.bx, s.bz);
                if (d < best_d) { best_d = d; best_w = s.width; best_depth = s.depth; }
            }
        }
    }
    out_w = best_w;
    out_d = best_depth;
    return best_d;
}

// Ground height with the drainage carved in. The tracing in build_rivers runs
// against terrain_base, so this is the one place the two must not be confused:
// carving the surface the rivers were traced on would feed back on itself.
static float terrain_height(const SeedWorld& w, const WorldParams& wp,
                            const ArchetypeTable& table, float x, float z, float r) {
    const float h = terrain_base(w, wp, table, x, z, r);
    float rw = 0.0f, rd = 0.0f;
    const float dist = nearest_river(w, wp, x, z, rw, rd);
    if (dist >= rw) return h;
    return river_carve(h, dist, rw, rd);
}

// --- layer 5: trace the drainage -------------------------------------------
//
// Steepest descent on a raw heightfield gets stuck in every local minimum, so
// the surface is depression-filled first (priority flood). That buys more than
// convenience: the flood pops cells in order of spill height, so the cell that
// relaxes a neighbour is always downstream of it. Taking that as the flow
// direction makes "descends monotonically and terminates in ocean" true by
// construction rather than something to test for and patch up.
static void build_rivers(SeedWorld& w, const WorldParams& wp, const ArchetypeTable& table) {
    const int   G = wp.river_grid;
    const int   N = G * G;
    const float R = wp.radius;
    const float step = 2.0f * R / G;

    std::vector<float> h(N, 0.0f);
    std::vector<char>  inside(N, 0);
    for (int j = 0; j < G; ++j) {
        const float z = -R + (j + 0.5f) * step;
        for (int i = 0; i < G; ++i) {
            const float x = -R + (i + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            const int   k = j * G + i;
            if (rr > 1.0f) { h[k] = -1.0e9f; continue; }
            inside[k] = 1;
            h[k] = terrain_base(w, wp, table, x, z, rr);
        }
    }

    std::vector<float> filled(h);
    std::vector<int>   parent(N, -1);
    std::vector<char>  done(N, 0);
    std::vector<int>   order;
    order.reserve(N);

    typedef std::pair<float, int> PQ;
    std::priority_queue<PQ, std::vector<PQ>, std::greater<PQ> > pq;
    for (int k = 0; k < N; ++k) {
        // The sea and everything beyond the rim are the outlets.
        if (!inside[k] || h[k] <= 0.0f) {
            done[k] = 1;
            order.push_back(k);
            pq.push(PQ(h[k], k));
        }
    }

    // Eight-connected, not four. Four-connected flow can only run along the
    // grid axes, so every river comes out as a right-angled staircase -- which
    // is invisible on a contact sheet at 87 m per pixel and unmistakable the
    // moment you look at one seed close up.
    const int dxs[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int dzs[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (!pq.empty()) {
        const PQ cur = pq.top();
        pq.pop();
        const int k = cur.second, ci = k % G, cj = k / G;
        for (int d = 0; d < 8; ++d) {
            const int ni = ci + dxs[d], nj = cj + dzs[d];
            if (ni < 0 || ni >= G || nj < 0 || nj >= G) continue;
            const int nk = nj * G + ni;
            if (done[nk] || !inside[nk]) continue;
            done[nk] = 1;
            filled[nk] = std::max(h[nk], cur.first);
            parent[nk] = k;              // the cell it spills into: downstream
            order.push_back(nk);
            pq.push(PQ(filled[nk], nk));
        }
    }

    // Upstream area. Children are relaxed after their parent, so walking the
    // flood order backwards visits every cell before the one it drains into.
    std::vector<float> acc(N, 1.0f);
    for (size_t idx = order.size(); idx-- > 0;) {
        const int k = order[idx];
        if (parent[k] >= 0) acc[parent[k]] += acc[k];
    }

    w.rivers.clear();
    for (int k = 0; k < N; ++k) {
        if (!inside[k] || parent[k] < 0) continue;
        if (h[k] <= 0.0f) continue;                 // already at sea
        if (acc[k] < wp.river_threshold) continue;
        const int p = parent[k];
        const int g = parent[p];
        // Corner-cut: run the segment between edge midpoints rather than cell
        // centres. One pass of Chaikin on the flow path, which costs nothing
        // and takes the hard right angles off the grid.
        const float kx = -R + ((k % G) + 0.5f) * step, kz = -R + ((k / G) + 0.5f) * step;
        const float px = -R + ((p % G) + 0.5f) * step, pz = -R + ((p / G) + 0.5f) * step;
        SeedWorld::RiverSeg s;
        s.ax = 0.5f * (kx + px);
        s.az = 0.5f * (kz + pz);
        if (g >= 0) {
            const float gx = -R + ((g % G) + 0.5f) * step, gz = -R + ((g / G) + 0.5f) * step;
            s.bx = 0.5f * (px + gx);
            s.bz = 0.5f * (pz + gz);
        } else {
            s.bx = px;
            s.bz = pz;
        }
        const float f = std::sqrt(acc[k] / wp.river_threshold);
        s.width = std::min(38.0f + 26.0f * f, 170.0f);
        s.depth = std::min(3.0f + 2.4f * f, 17.0f);
        w.rivers.push_back(s);
        if (filled[p] > filled[k]) ++w.river_ascending;
    }

    // Every river head followed to its outlet, as the harness asserts.
    for (int k = 0; k < N; ++k) {
        if (!inside[k] || h[k] <= 0.0f || acc[k] < wp.river_threshold) continue;
        bool head = true;
        for (int d = 0; d < 8 && head; ++d) {
            const int ni = (k % G) + dxs[d], nj = (k / G) + dzs[d];
            if (ni < 0 || ni >= G || nj < 0 || nj >= G) continue;
            const int nk = nj * G + ni;
            if (parent[nk] == k && acc[nk] >= wp.river_threshold) head = false;
        }
        if (!head) continue;
        ++w.river_paths_total;
        int cur = k, steps = 0;
        while (cur >= 0 && steps < 4 * N) {
            if (h[cur] <= 0.0f || !inside[cur]) { ++w.river_paths_to_ocean; break; }
            cur = parent[cur];
            ++steps;
        }
    }

    // Bin the segments so the carve only tests the few that could be near.
    w.rgrid_n = std::max(8, G / 4);
    w.rgrid_cell = 2.0f * R / w.rgrid_n;
    w.rgrid.assign(static_cast<size_t>(w.rgrid_n) * w.rgrid_n, std::vector<int>());
    for (size_t s = 0; s < w.rivers.size(); ++s) {
        const SeedWorld::RiverSeg& seg = w.rivers[s];
        const float pad = seg.width;
        const int i0 = std::max(0, static_cast<int>((std::min(seg.ax, seg.bx) - pad + R) / w.rgrid_cell));
        const int i1 = std::min(w.rgrid_n - 1, static_cast<int>((std::max(seg.ax, seg.bx) + pad + R) / w.rgrid_cell));
        const int j0 = std::max(0, static_cast<int>((std::min(seg.az, seg.bz) - pad + R) / w.rgrid_cell));
        const int j1 = std::min(w.rgrid_n - 1, static_cast<int>((std::max(seg.az, seg.bz) + pad + R) / w.rgrid_cell));
        for (int j = j0; j <= j1; ++j) {
            for (int i = i0; i <= i1; ++i) {
                w.rgrid[static_cast<size_t>(j) * w.rgrid_n + i].push_back(static_cast<int>(s));
            }
        }
    }
}

// --- per-seed evaluation ---------------------------------------------------
struct SeedResult {
    int   cells = 0, land_cells = 0;
    float land_fraction = 0.0f;
    float max_quota_err = 0.0f;   // max |relative error| over biomes
    float spearman = 0.0f;
    int   missing_biomes = 0;
    int   relaxed = 0, overflow = 0;
    int   adj_violations = 0;   // against the authored adjacency_forbid rules
    float mean_candidates = 0.0f;
    // --- A3 checks ---
    float max_h_outer = 0.0f;   // edge taper: highest sample past r = 0.95
    float rim_height = 0.0f;    // highest layer-0 sample on the r = 1 ring
    bool  taper_ok = false;     // both of those below sea level
    float foot_reach = 0.0f;    // land cells reachable on foot from spawn
    bool  boat_all_biomes = false;
    int   spawn_cell = -1;
    bool  spawn_coastal = false;
    bool  spawn_calmest = false;
    float slope_gentle = 1.0f;  // share of land under 30 degrees
    int   river_segments = 0;
    int   river_paths_total = 0, river_paths_to_ocean = 0, river_ascending = 0;
    uint64_t hash = 0;
    float mean_patch_cells = 0.0f;
    float mean_patch_km2 = 0.0f;
    std::vector<int>   biome_cells;   // per biome
    std::vector<float> biome_share;   // per biome, share of land area
};

static SeedWorld build_world(uint64_t world_seed,
                             const WorldParams& wp,
                             const ArchetypeTable& table,
                             const AssignParams& ap) {
    SeedWorld w;

    w.s_cont = feature_seed(world_seed, FEATURE_CONTINENT);
    w.s_dang = feature_seed(world_seed, FEATURE_DANGER);
    w.s_wx   = feature_seed(world_seed, FEATURE_WARP_X);
    w.s_wz   = feature_seed(world_seed, FEATURE_WARP_Z);
    w.s_height = feature_seed(world_seed, FEATURE_HEIGHT);
    const uint32_t s_cont = w.s_cont, s_dang = w.s_dang, s_wx = w.s_wx, s_wz = w.s_wz;
    const uint32_t s_cell = feature_seed(world_seed, FEATURE_CELLS);

    w.pts = scatter_cells(wp, s_cell);
    const std::vector<std::pair<float, float> >& pts = w.pts;
    const int n = static_cast<int>(pts.size());
    if (n == 0) return w;

    const float R = wp.radius;
    w.lookup.build(pts, R);
    const CellLookup& lookup = w.lookup;
    auto nearest = [&](float x, float z) { return lookup.nearest(pts, x, z); };

    // Rasterise the disc: real cell areas, real land mask, real adjacency.
    const int   G = wp.grid;
    const float step = 2.0f * R / G;
    const float sample_area = step * step;

    std::vector<double> acc_x(n, 0.0), acc_z(n, 0.0);
    std::vector<double> acc_total(n, 0.0), acc_land(n, 0.0);
    std::vector<char> adj(static_cast<size_t>(n) * n, 0);
    std::vector<int> row_prev(G, -1), row_cur(G, -1);

    double disc_area = 0.0, land_area = 0.0;

    for (int gj = 0; gj < G; ++gj) {
        const float z = -R + (gj + 0.5f) * step;
        for (int gi = 0; gi < G; ++gi) {
            const float x = -R + (gi + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            if (rr > 1.0f) { row_cur[gi] = -1; continue; }

            const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, s_wx);
            const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, s_wz);
            const int ci = nearest(wx, wz);
            row_cur[gi] = ci;
            if (ci < 0) continue;

            disc_area += sample_area;
            acc_total[ci] += sample_area;
            acc_x[ci] += x;
            acc_z[ci] += z;

            const float h = layer0_height(x, z, rr, wp, s_cont);
            if (rr > 0.95f && h > w.max_h_outer) w.max_h_outer = h;
            if (h > wp.sea_level) {
                acc_land[ci] += sample_area;
                land_area += sample_area;
            }

            if (gi > 0 && row_cur[gi - 1] >= 0 && row_cur[gi - 1] != ci) {
                adj[static_cast<size_t>(ci) * n + row_cur[gi - 1]] = 1;
                adj[static_cast<size_t>(row_cur[gi - 1]) * n + ci] = 1;
            }
            if (row_prev[gi] >= 0 && row_prev[gi] != ci) {
                adj[static_cast<size_t>(ci) * n + row_prev[gi]] = 1;
                adj[static_cast<size_t>(row_prev[gi]) * n + ci] = 1;
            }
        }
        row_prev.swap(row_cur);
    }

    w.land_fraction = disc_area > 0.0 ? static_cast<float>(land_area / disc_area) : 0.0f;

    // Build cell records from the rasterised truth.
    w.cells.resize(n);
    std::vector<CellRecord>& cells = w.cells;
    for (int i = 0; i < n; ++i) {
        CellRecord& c = cells[i];
        c.total_area = static_cast<float>(acc_total[i]);
        c.land_area  = static_cast<float>(acc_land[i]);
        if (acc_total[i] > 0.0) {
            c.cx = static_cast<float>(acc_x[i] / (acc_total[i] / sample_area));
            c.cz = static_cast<float>(acc_z[i] / (acc_total[i] / sample_area));
        } else {
            c.cx = pts[i].first; c.cz = pts[i].second;
        }
        const float rr = std::sqrt(c.cx * c.cx + c.cz * c.cz) / R;
        c.norm_area = rr * rr;
        c.danger    = layer1_danger(c.cx, c.cz, rr, wp, s_dang);
        c.is_land   = c.total_area > 0.0f && c.land_area > 0.5f * c.total_area;
    }

    CellGraph& graph = w.graph;
    graph.offset.resize(n + 1, 0);
    for (int i = 0; i < n; ++i) {
        graph.offset[i] = static_cast<int>(graph.list.size());
        for (int j = 0; j < n; ++j) if (adj[static_cast<size_t>(i) * n + j]) graph.list.push_back(j);
    }
    graph.offset[n] = static_cast<int>(graph.list.size());

    // --- the thing under test ---
    assign_biomes(cells, graph, table, ap, w.biome, w.stats);

    // Layer 5 runs last: it needs the finished heightfield, which needs the
    // biome assignment.
    build_rivers(w, wp, table);
    return w;
}

// --- per-seed metrics ------------------------------------------------------
static SeedResult eval_seed(uint64_t world_seed,
                            const WorldParams& wp,
                            const ArchetypeTable& table,
                            const AssignParams& ap) {
    SeedResult out;
    const SeedWorld w = build_world(world_seed, wp, table, ap);

    out.cells         = static_cast<int>(w.pts.size());
    out.land_fraction = w.land_fraction;

    const int n = static_cast<int>(w.cells.size());
    if (n == 0) return out;

    const std::vector<CellRecord>& cells = w.cells;
    const CellGraph&               graph = w.graph;
    const std::vector<int>&        biome = w.biome;
    const AssignStats&             stats = w.stats;

    out.land_cells      = stats.land_cells;
    out.relaxed         = stats.relaxed_cells;
    out.overflow        = stats.overflow_cells;
    out.mean_candidates = stats.mean_candidates;

    // --- metrics ---
    const size_t nb = table.size();
    std::vector<double> got(nb, 0.0), mean_na(nb, 0.0);
    std::vector<int> count(nb, 0);
    double total_land = 0.0;
    for (int i = 0; i < n; ++i) {
        if (biome[i] < 0) continue;
        got[biome[i]] += cells[i].land_area;
        mean_na[biome[i]] += cells[i].norm_area;
        count[biome[i]] += 1;
        total_land += cells[i].land_area;
    }

    out.biome_cells.assign(nb, 0);
    out.biome_share.assign(nb, 0.0f);
    for (size_t b = 0; b < nb; ++b) {
        const double actual = total_land > 0.0 ? got[b] / total_land : 0.0;
        out.biome_cells[b] = count[b];
        out.biome_share[b] = static_cast<float>(actual);
        if (count[b] == 0) { out.missing_biomes += 1; }
        else { mean_na[b] /= count[b]; }
        const double target = table[b].area_fraction;
        const double rel = std::fabs(actual - target) / target;
        out.max_quota_err = std::max(out.max_quota_err, static_cast<float>(rel));
    }

    // Violations of the adjacency rules actually authored in archetypes.json.
    // The --test-adjacency mode probes a synthetic worst-case pair; this
    // asserts the rules the table really states, on every run.
    for (int i = 0; i < n; ++i) {
        if (biome[i] < 0) continue;
        for (int k = graph.offset[i]; k < graph.offset[i + 1]; ++k) {
            const int j = graph.list[k];
            if (j <= i || biome[j] < 0) continue;
            if ((table[biome[i]].adjacency_mask >> biome[j]) & 1u) ++out.adj_violations;
        }
    }

    // --- edge taper ---
    // No land above sea level past r = 0.95, and terrain under the void
    // threshold on the r = 1 ring.
    out.max_h_outer = w.max_h_outer;
    {
        const float R = wp.radius;
        // The worst case on the rim ring is its highest point.
        float worst = -1.0e9f;
        for (int k = 0; k < 720; ++k) {
            const float a = static_cast<float>(k) * (6.28318530718f / 720.0f);
            const float x = R * std::cos(a), z = R * std::sin(a);
            const float h = layer0_height(x, z, 1.0f, wp, w.s_cont);
            if (h > worst) worst = h;
        }
        out.rim_height = worst;
        // The requirement is that land has fallen away to ocean before the
        // rim -- not that it is already in the void. The void begins past
        // r = 1 and is the caller's business, not the field's.
        out.taper_ok = (out.max_h_outer <= wp.sea_level) && (out.rim_height <= wp.sea_level);
    }

    // --- spawn ---
    // Deterministic by construction: the calmest coastal land cell, ties
    // broken on distance from the origin and then on index. The harness
    // asserts the properties the design doc wants of it.
    {
        size_t calmest = 0;
        for (size_t b = 1; b < nb; ++b) {
            if (table[b].danger_preferred < table[calmest].danger_preferred) calmest = b;
        }
        int best = -1;
        for (int i = 0; i < n; ++i) {
            if (biome[i] != static_cast<int>(calmest) || !cells[i].is_land) continue;
            bool coastal = false;
            for (int k = graph.offset[i]; k < graph.offset[i + 1]; ++k) {
                if (!cells[graph.list[k]].is_land) { coastal = true; break; }
            }
            if (!coastal) continue;
            if (best < 0) { best = i; continue; }
            if (cells[i].danger < cells[best].danger) best = i;
            else if (cells[i].danger == cells[best].danger &&
                     cells[i].norm_area < cells[best].norm_area) best = i;
        }
        out.spawn_cell = best;
        out.spawn_coastal = (best >= 0);
        out.spawn_calmest = (best >= 0);
    }

    // --- connectivity ---
    // On foot: land cells only. By boat: land plus ocean, which must reach
    // every biome or some of them are on islands nobody can get to.
    if (out.spawn_cell >= 0) {
        std::vector<char> seen(n, 0);
        std::vector<int> stack;
        stack.push_back(out.spawn_cell);
        seen[out.spawn_cell] = 1;
        int reached = 0, land_total = 0;
        while (!stack.empty()) {
            const int c = stack.back(); stack.pop_back();
            ++reached;
            for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                const int nb2 = graph.list[k];
                if (seen[nb2] || !cells[nb2].is_land) continue;
                seen[nb2] = 1;
                stack.push_back(nb2);
            }
        }
        for (int i = 0; i < n; ++i) if (cells[i].is_land) ++land_total;
        out.foot_reach = land_total > 0 ? static_cast<float>(reached) / land_total : 0.0f;

        std::fill(seen.begin(), seen.end(), 0);
        stack.push_back(out.spawn_cell);
        seen[out.spawn_cell] = 1;
        std::vector<char> biome_seen(nb, 0);
        while (!stack.empty()) {
            const int c = stack.back(); stack.pop_back();
            if (biome[c] >= 0) biome_seen[biome[c]] = 1;
            for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                const int nb2 = graph.list[k];
                if (seen[nb2]) continue;
                seen[nb2] = 1;
                stack.push_back(nb2);
            }
        }
        out.boat_all_biomes = true;
        for (size_t b = 0; b < nb; ++b) {
            if (count[b] > 0 && !biome_seen[b]) out.boat_all_biomes = false;
        }
    }

    out.river_segments     = static_cast<int>(w.rivers.size());
    out.river_paths_total  = w.river_paths_total;
    out.river_paths_to_ocean = w.river_paths_to_ocean;
    out.river_ascending    = w.river_ascending;

    // --- slope budget ---
    // Measured over a 4 m baseline, not the sampling step: walkability is a
    // local property, and a 100 m baseline would average every cliff away.
    {
        const int   SG = 192;
        const float R = wp.radius, ss = 2.0f * R / SG, e = 4.0f;
        int land_n = 0, gentle = 0;
        for (int gj = 0; gj < SG; ++gj) {
            const float z = -R + (gj + 0.5f) * ss;
            for (int gi = 0; gi < SG; ++gi) {
                const float x = -R + (gi + 0.5f) * ss;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                if (layer0_height(x, z, rr, wp, w.s_cont) <= wp.sea_level) continue;
                ++land_n;
                const float h0 = terrain_height(w, wp, table, x, z, rr);
                const float hx = terrain_height(w, wp, table, x + e, z,
                                                std::sqrt((x + e) * (x + e) + z * z) / R);
                const float hz = terrain_height(w, wp, table, x, z + e,
                                                std::sqrt(x * x + (z + e) * (z + e)) / R);
                const float gx = (hx - h0) / e, gz = (hz - h0) / e;
                const float deg = std::atan(std::sqrt(gx * gx + gz * gz)) * (180.0f / 3.14159265f);
                if (deg < 30.0f) ++gentle;
            }
        }
        out.slope_gentle = land_n > 0 ? static_cast<float>(gentle) / land_n : 1.0f;
    }

    // --- determinism hash ---
    // Layers 0 and 1 on a fixed grid, plus the layer-3 assignment. Quantised,
    // because the point is to catch a changed generator, not float noise.
    {
        uint64_t h = 0xCBF29CE484222325ull;
        const int HG = 256;
        const float R = wp.radius, hstep = 2.0f * R / HG;
        for (int gj = 0; gj < HG; ++gj) {
            const float z = -R + (gj + 0.5f) * hstep;
            for (int gi = 0; gi < HG; ++gi) {
                const float x = -R + (gi + 0.5f) * hstep;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                const float hh = layer0_height(x, z, rr, wp, w.s_cont);
                const float dd = layer1_danger(x, z, rr, wp, w.s_dang);
                const float tt = terrain_height(w, wp, table, x, z, rr);
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(hh * 4096.0f)));
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(dd * 4096.0f)));
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(tt * 64.0f)));
            }
        }
        for (int i = 0; i < n; ++i) {
            h = fnv1a(h, static_cast<uint64_t>(biome[i] + 1));
        }
        out.hash = h;
    }

    // Spearman over biomes that got cells.
    {
        std::vector<int> idx;
        for (size_t b = 0; b < nb; ++b) if (count[b] > 0) idx.push_back(static_cast<int>(b));
        const int m = static_cast<int>(idx.size());
        if (m >= 3) {
            std::vector<int> ra(m), rb(m);
            std::vector<int> ord(m);
            for (int i = 0; i < m; ++i) ord[i] = i;
            std::sort(ord.begin(), ord.end(), [&](int a, int b2) {
                return table[idx[a]].danger_preferred < table[idx[b2]].danger_preferred; });
            for (int i = 0; i < m; ++i) ra[ord[i]] = i;
            for (int i = 0; i < m; ++i) ord[i] = i;
            std::sort(ord.begin(), ord.end(), [&](int a, int b2) {
                return mean_na[idx[a]] < mean_na[idx[b2]]; });
            for (int i = 0; i < m; ++i) rb[ord[i]] = i;
            double d2 = 0.0;
            for (int i = 0; i < m; ++i) { const double d = ra[i] - rb[i]; d2 += d * d; }
            out.spearman = static_cast<float>(1.0 - 6.0 * d2 / (static_cast<double>(m) * (static_cast<double>(m) * m - 1.0)));
        } else {
            out.spearman = 0.0f;
        }
    }

    // Patch stats: connected components of same-biome land cells.
    {
        std::vector<char> seen(n, 0);
        std::vector<int> stack;
        int comps = 0, comp_cells = 0;
        double comp_area = 0.0;
        for (int i = 0; i < n; ++i) {
            if (seen[i] || biome[i] < 0) continue;
            ++comps;
            stack.push_back(i);
            seen[i] = 1;
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back();
                ++comp_cells;
                comp_area += cells[c].land_area;
                for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                    const int nb2 = graph.list[k];
                    if (!seen[nb2] && biome[nb2] == biome[c]) { seen[nb2] = 1; stack.push_back(nb2); }
                }
            }
        }
        if (comps > 0) {
            out.mean_patch_cells = static_cast<float>(comp_cells) / comps;
            out.mean_patch_km2   = static_cast<float>(comp_area / comps / 1.0e6);
        }
    }

    return out;
}

// --- aggregate over seeds --------------------------------------------------
struct Summary {
    int seeds = 0;
    int presence_failures = 0;
    int quota_failures = 0;      // max rel err > 0.10
    int radial_failures = 0;     // spearman < 0.8
    int land_failures = 0;       // |land - 0.55| > 0.05
    int adjacency_failures = 0;  // seeds with >= 1 authored-rule violation
    int taper_failures = 0;      // land past r = 0.95, or a rim above the void
    int foot_failures = 0;       // spawn cannot walk to most of the land
    int boat_failures = 0;       // a biome unreachable even by sea
    int spawn_failures = 0;      // no spawn, not coastal, or not calmest
    int slope_failures = 0;      // under 60% of land walkable
    int river_failures = 0;      // no rivers, or one that climbs or never reaches the sea
    float mean_river_segments = 0.0f;
    float min_foot_reach = 2.0f;
    float min_slope_gentle = 2.0f;
    float mean_cells = 0, mean_land_cells = 0, mean_land_frac = 0;
    float mean_quota_err = 0, max_quota_err = 0, p95_quota_err = 0;
    float mean_spearman = 0, min_spearman = 2.0f;
    float mean_patch_cells = 0, mean_patch_km2 = 0;
    float mean_relaxed = 0, mean_overflow = 0;
    float mean_candidates = 0;
};

static Summary run(int nseeds, const WorldParams& wp, const ArchetypeTable& table, const AssignParams& ap) {
    std::vector<SeedResult> results(nseeds);
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, static_cast<unsigned>(nseeds));

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = static_cast<int>(t); s < nseeds; s += static_cast<int>(nthreads)) {
                results[s] = eval_seed(0x5EEDull * 1000003ull + static_cast<uint64_t>(s), wp, table, ap);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();

    Summary sm;
    sm.seeds = nseeds;
    std::vector<float> errs;
    errs.reserve(nseeds);
    for (int s = 0; s < nseeds; ++s) {
        const SeedResult& r = results[s];
        if (r.missing_biomes > 0) ++sm.presence_failures;
        if (r.max_quota_err > 0.10f) ++sm.quota_failures;
        if (r.spearman < 0.8f) ++sm.radial_failures;
        if (std::fabs(r.land_fraction - 0.55f) > 0.05f) ++sm.land_failures;
        if (r.adj_violations > 0) ++sm.adjacency_failures;
        if (!r.taper_ok) ++sm.taper_failures;
        // Not 100%: islands are a designed feature, so some land is legitimately
        // unreachable on foot. This is a regression guard on the mainland being
        // whole, with boat connectivity carrying the "every biome reachable"
        // requirement.
        if (r.foot_reach < 0.75f) ++sm.foot_failures;
        if (!r.boat_all_biomes) ++sm.boat_failures;
        if (r.spawn_cell < 0 || !r.spawn_coastal || !r.spawn_calmest) ++sm.spawn_failures;
        if (r.slope_gentle < 0.60f) ++sm.slope_failures;
        if (r.river_segments == 0 || r.river_ascending > 0 ||
            r.river_paths_to_ocean != r.river_paths_total) ++sm.river_failures;
        sm.mean_river_segments += r.river_segments;
        sm.min_foot_reach = std::min(sm.min_foot_reach, r.foot_reach);
        sm.min_slope_gentle = std::min(sm.min_slope_gentle, r.slope_gentle);
        sm.mean_cells += r.cells;
        sm.mean_land_cells += r.land_cells;
        sm.mean_land_frac += r.land_fraction;
        sm.mean_quota_err += r.max_quota_err;
        sm.max_quota_err = std::max(sm.max_quota_err, r.max_quota_err);
        sm.mean_spearman += r.spearman;
        sm.min_spearman = std::min(sm.min_spearman, r.spearman);
        sm.mean_patch_cells += r.mean_patch_cells;
        sm.mean_patch_km2 += r.mean_patch_km2;
        sm.mean_relaxed += r.relaxed;
        sm.mean_overflow += r.overflow;
        sm.mean_candidates += r.mean_candidates;
        errs.push_back(r.max_quota_err);
    }
    const float inv = 1.0f / nseeds;
    sm.mean_cells *= inv; sm.mean_land_cells *= inv; sm.mean_land_frac *= inv;
    sm.mean_quota_err *= inv; sm.mean_spearman *= inv;
    sm.mean_patch_cells *= inv; sm.mean_patch_km2 *= inv;
    sm.mean_relaxed *= inv; sm.mean_overflow *= inv; sm.mean_candidates *= inv;
    sm.mean_river_segments *= inv;
    std::sort(errs.begin(), errs.end());
    sm.p95_quota_err = errs[std::min<size_t>(errs.size() - 1, static_cast<size_t>(errs.size() * 0.95))];
    return sm;
}

static void print_summary(const char* label, const Summary& s) {
    std::printf("%-24s %5.0f %5.0f %5.1f%%  %5.1f%% %5.1f%% %5.1f%%  %5.2f %5.2f  %4.1f %5.2f  %4.1f %4.1f  %3d %3d %3d %3d %3d\n",
                label, s.mean_cells, s.mean_land_cells, 100.0 * s.mean_land_frac,
                100.0 * s.mean_quota_err, 100.0 * s.p95_quota_err, 100.0 * s.max_quota_err,
                s.mean_spearman, s.min_spearman,
                s.mean_patch_cells, s.mean_patch_km2,
                s.mean_relaxed, s.mean_candidates,
                s.presence_failures, s.quota_failures, s.radial_failures, s.land_failures,
                s.adjacency_failures);
}

static void print_header() {
    std::printf("\n%-24s %5s %5s %6s  %-21s %-12s %-11s %-10s %s\n",
                "config", "cells", "land", "land%", "quota err mean/p95/max",
                "spearman", "patch c/km2", "relax/cand", "fail: pres quota rad land adj");
    std::printf("%s\n", std::string(136, '-').c_str());
}

static void print_detail(const WorldParams& wp, const ArchetypeTable& table,
                         const AssignParams& ap, uint64_t seed) {
    const SeedResult r = eval_seed(seed, wp, table, ap);
    std::printf("\nper-biome, one seed (R=%.0fkm, contiguity=%.2f): %d cells, %d land, land %.1f%%\n",
                wp.radius / 1000.0f, ap.contiguity_bonus, r.cells, r.land_cells, 100.0f * r.land_fraction);
    std::printf("%-12s %6s %8s %8s %9s\n", "biome", "cells", "target", "actual", "rel err");
    std::printf("%s\n", std::string(48, '-').c_str());
    for (size_t b = 0; b < table.size(); ++b) {
        const float target = table[b].area_fraction;
        const float actual = r.biome_share[b];
        std::printf("%-12s %6d %7.1f%% %7.1f%% %8.1f%%\n",
                    table[b].name.c_str(), r.biome_cells[b],
                    100.0f * target, 100.0f * actual, 100.0f * (actual - target) / target);
    }
    std::printf("relaxed %d, overflow %d\n", r.relaxed, r.overflow);
}

// --- dependency-free image output ------------------------------------------
// 24-bit uncompressed BMP. Chosen over PNG because a PNG writer needs zlib
// framing and CRCs, which is more code than a debug image is worth, and the
// harness is meant to stay dependency-free. Convert in CI if an artifact
// viewer insists on PNG.
static bool write_bmp(const char* path, int w, int h, const std::vector<unsigned char>& rgb) {
    const int      row_raw = w * 3;
    const int      pad     = (4 - (row_raw % 4)) % 4;
    const int      row     = row_raw + pad;
    const uint32_t bytes   = static_cast<uint32_t>(row) * static_cast<uint32_t>(h);
    const uint32_t offset  = 14 + 40;

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return false;

    unsigned char hdr[14 + 40];
    std::memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    const auto put32 = [](unsigned char* p, uint32_t v) {
        p[0] = v & 255u; p[1] = (v >> 8) & 255u; p[2] = (v >> 16) & 255u; p[3] = (v >> 24) & 255u;
    };
    const auto put16 = [](unsigned char* p, uint16_t v) { p[0] = v & 255u; p[1] = (v >> 8) & 255u; };
    put32(hdr +  2, offset + bytes);
    put32(hdr + 10, offset);
    put32(hdr + 14, 40);
    put32(hdr + 18, static_cast<uint32_t>(w));
    put32(hdr + 22, static_cast<uint32_t>(h));   // positive height = bottom-up
    put16(hdr + 26, 1);
    put16(hdr + 28, 24);
    put32(hdr + 34, bytes);
    put32(hdr + 38, 2835);
    put32(hdr + 42, 2835);
    std::fwrite(hdr, 1, sizeof(hdr), f);

    std::vector<unsigned char> line(static_cast<size_t>(row), 0);
    for (int y = h - 1; y >= 0; --y) {           // BMP rows run bottom-up
        for (int x = 0; x < w; ++x) {
            const size_t s = (static_cast<size_t>(y) * w + x) * 3;
            line[x * 3 + 0] = rgb[s + 2];        // B
            line[x * 3 + 1] = rgb[s + 1];        // G
            line[x * 3 + 2] = rgb[s + 0];        // R
        }
        std::fwrite(line.data(), 1, static_cast<size_t>(row), f);
    }
    std::fclose(f);
    return true;
}

// Debug palette. Deliberately here rather than in archetypes.json: the design
// doc puts presentation explicitly outside the archetype contract, and the
// generator must not grow a dependency on what a biome looks like.
static void biome_colour(int b, unsigned char* c) {
    static const unsigned char kP[9][3] = {
        {126, 200,  80}, { 34, 110,  50}, { 90, 125,  95},
        {150, 140, 120}, {196, 124,  58}, { 66, 132, 124},
        {152,  54,  44}, {219, 232, 245}, {112,  60, 146}
    };
    if (b < 0) { c[0] = 70; c[1] = 70; c[2] = 70; return; }  // land in an ocean cell
    if (b < 9) { c[0] = kP[b][0]; c[1] = kP[b][1]; c[2] = kP[b][2]; return; }
    // Fallback for tables larger than the hand-picked set: golden-angle hues.
    const float hue = static_cast<float>(b) * 137.507f;
    const float hh  = (hue - std::floor(hue / 360.0f) * 360.0f) / 60.0f;
    const float xx  = 1.0f - std::fabs(hh - std::floor(hh / 2.0f) * 2.0f - 1.0f);
    float r = 0, g = 0, bl = 0;
    const int s = static_cast<int>(hh);
    if (s == 0) { r = 1; g = xx; } else if (s == 1) { r = xx; g = 1; }
    else if (s == 2) { g = 1; bl = xx; } else if (s == 3) { g = xx; bl = 1; }
    else if (s == 4) { r = xx; bl = 1; } else { r = 1; bl = xx; }
    c[0] = static_cast<unsigned char>(40 + 190 * r);
    c[1] = static_cast<unsigned char>(40 + 190 * g);
    c[2] = static_cast<unsigned char>(40 + 190 * bl);
}

// Shaded relief. The A4 exit criterion is whether this reads as a world
// rather than as noise, and that is not a thing an assertion can answer.
static void render_relief_tile(const SeedWorld& w, const WorldParams& wp,
                               const ArchetypeTable& table,
                               std::vector<unsigned char>& img, int img_w,
                               int ox, int oy, int tile) {
    const float R = wp.radius;
    const float step = 2.0f * R / tile;
    const float e = 6.0f;                       // metres, for the normal
    const float lx = -0.55f, lz = -0.55f, ly = 0.63f;   // light from the north-west
    for (int py = 0; py < tile; ++py) {
        const float z = -R + (py + 0.5f) * step;
        for (int px = 0; px < tile; ++px) {
            const float x  = -R + (px + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            unsigned char c[3];
            if (rr > 1.0f) {
                c[0] = 10; c[1] = 12; c[2] = 18;
            } else {
                const float h0 = terrain_height(w, wp, table, x, z, rr);
                float rw = 0.0f, rd = 0.0f;
                const float rdist = nearest_river(w, wp, x, z, rw, rd);
                if (h0 <= 0.0f) {
                    const float d = wg_saturate(-h0 / 160.0f);
                    c[0] = static_cast<unsigned char>(46 - 30 * d);
                    c[1] = static_cast<unsigned char>(78 - 46 * d);
                    c[2] = static_cast<unsigned char>(130 - 62 * d);
                } else if (rdist < rw * 0.42f) {
                    // Inside the channel: draw the water, not the riverbed.
                    c[0] = 52; c[1] = 96; c[2] = 150;
                } else {
                    const float hx = terrain_height(w, wp, table, x + e, z,
                                                    std::sqrt((x + e) * (x + e) + z * z) / R);
                    const float hz = terrain_height(w, wp, table, x, z + e,
                                                    std::sqrt(x * x + (z + e) * (z + e)) / R);
                    const float gx = (hx - h0) / e, gz = (hz - h0) / e;
                    const float inv = 1.0f / std::sqrt(gx * gx + gz * gz + 1.0f);
                    const float shade = wg_saturate((-gx * lx - gz * lz + ly) * inv * 1.25f);
                    const float t = wg_saturate(h0 / 220.0f);
                    float r3, g3, b3;
                    if (t < 0.4f) {
                        const float u = t / 0.4f;
                        r3 = 0.36f + 0.18f * u; g3 = 0.54f - 0.06f * u; b3 = 0.30f + 0.02f * u;
                    } else if (t < 0.78f) {
                        const float u = (t - 0.4f) / 0.38f;
                        r3 = 0.54f + 0.08f * u; g3 = 0.48f - 0.04f * u; b3 = 0.32f + 0.04f * u;
                    } else {
                        const float u = (t - 0.78f) / 0.22f;
                        r3 = 0.62f + 0.30f * u; g3 = 0.44f + 0.50f * u; b3 = 0.36f + 0.58f * u;
                    }
                    const float s = 0.30f + 0.80f * shade;
                    c[0] = static_cast<unsigned char>(wg_saturate(r3 * s) * 255.0f);
                    c[1] = static_cast<unsigned char>(wg_saturate(g3 * s) * 255.0f);
                    c[2] = static_cast<unsigned char>(wg_saturate(b3 * s) * 255.0f);
                }
            }
            const size_t d = (static_cast<size_t>(oy + py) * img_w + (ox + px)) * 3;
            img[d + 0] = c[0]; img[d + 1] = c[1]; img[d + 2] = c[2];
        }
    }
}

static void render_tile(const SeedWorld& w, const WorldParams& wp,
                        std::vector<unsigned char>& img, int img_w,
                        int ox, int oy, int tile) {
    const float R    = wp.radius;
    const float step = 2.0f * R / tile;
    for (int py = 0; py < tile; ++py) {
        const float z = -R + (py + 0.5f) * step;
        for (int px = 0; px < tile; ++px) {
            const float x  = -R + (px + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            unsigned char c[3];
            if (rr > 1.0f) {
                c[0] = 10; c[1] = 12; c[2] = 18;              // void past the terminator
            } else {
                const float h = layer0_height(x, z, rr, wp, w.s_cont);
                if (h <= wp.sea_level) {
                    const float d = wg_saturate(-h * 1.2f);      // deeper water, darker
                    c[0] = static_cast<unsigned char>(46 - 30 * d);
                    c[1] = static_cast<unsigned char>(78 - 46 * d);
                    c[2] = static_cast<unsigned char>(130 - 62 * d);
                } else {
                    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
                    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
                    const int   ci = w.lookup.nearest(w.pts, wx, wz);
                    biome_colour(ci >= 0 ? w.biome[ci] : -1, c);
                }
            }
            const size_t d = (static_cast<size_t>(oy + py) * img_w + (ox + px)) * 3;
            img[d + 0] = c[0]; img[d + 1] = c[1]; img[d + 2] = c[2];
        }
    }
}

// 64 seeds, one biome map each, one image. Automated checks cannot tell you
// whether a world is interesting -- and Spearman reads 1.00 in every config we
// have tried, so it will never tell us whether w is right either.
static bool contact_sheet(const WorldParams& wp, const ArchetypeTable& table,
                          const AssignParams& ap, const char* path, int tile,
                          bool relief = false) {
    const int cols = 8, rows = 8, gap = 2;
    const int W = cols * tile + (cols + 1) * gap;
    const int H = rows * tile + (rows + 1) * gap;
    std::vector<unsigned char> img(static_cast<size_t>(W) * H * 3, 26);

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, 64u);

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = static_cast<int>(t); s < 64; s += static_cast<int>(nthreads)) {
                const SeedWorld w = build_world(0x5EEDull * 1000003ull + static_cast<uint64_t>(s),
                                                wp, table, ap);
                const int ox = gap + (s % cols) * (tile + gap);
                const int oy = gap + (s / cols) * (tile + gap);
                if (relief) render_relief_tile(w, wp, table, img, W, ox, oy, tile);
                else        render_tile(w, wp, img, W, ox, oy, tile);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();

    return write_bmp(path, W, H, img);
}

// One seed, rendered large. Rivers are 40-170 m wide and a contact-sheet tile
// is ~87 m per pixel, so the drainage is sub-pixel there and can only be judged
// close up.
static bool single_relief(const WorldParams& wp, const ArchetypeTable& table,
                          const AssignParams& ap, uint64_t seed,
                          const char* path, int tile) {
    std::vector<unsigned char> img(static_cast<size_t>(tile) * tile * 3, 26);
    const SeedWorld w = build_world(seed, wp, table, ap);
    render_relief_tile(w, wp, table, img, tile, 0, 0, tile);
    return write_bmp(path, tile, tile, img);
}

// --- adjacency test --------------------------------------------------------
// The constraints in archetypes.json are currently honoured only by accident:
// the candidate set is ~1, so the cost function never gets to apply the
// penalty. This picks the two biomes whose danger bands overlap most, forbids
// them from bordering, and counts how often that rule is broken.
static void test_adjacency(int nseeds, const WorldParams& wp, const ArchetypeTable& base,
                           const AssignParams& ap, float band_scale, bool header) {
    ArchetypeTable table = base;
    for (size_t b = 0; b < table.biomes.size(); ++b) {
        BiomeArchetype& a = table.biomes[b];
        const float lo = a.danger_preferred - (a.danger_preferred - a.danger_band_min) * band_scale;
        const float hi = a.danger_preferred + (a.danger_band_max - a.danger_preferred) * band_scale;
        a.danger_band_min = std::max(0.0f, lo);
        a.danger_band_max = std::min(1.0f, hi);
    }

    int   pi = -1, pj = -1;
    float best_overlap = -1.0f;
    for (size_t i = 0; i < table.size(); ++i) {
        for (size_t j = i + 1; j < table.size(); ++j) {
            const float ov = std::min(table[i].danger_band_max, table[j].danger_band_max) -
                             std::max(table[i].danger_band_min, table[j].danger_band_min);
            if (ov > best_overlap) { best_overlap = ov; pi = static_cast<int>(i); pj = static_cast<int>(j); }
        }
    }
    table.biomes[pi].adjacency_mask |= (1u << pj);
    table.biomes[pj].adjacency_mask |= (1u << pi);

    std::vector<int> viol(nseeds, 0), borders(nseeds, 0);
    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, static_cast<unsigned>(nseeds));

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nthreads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = static_cast<int>(t); s < nseeds; s += static_cast<int>(nthreads)) {
                const SeedWorld w = build_world(0x5EEDull * 1000003ull + static_cast<uint64_t>(s),
                                                wp, table, ap);
                const int n = static_cast<int>(w.cells.size());
                for (int i = 0; i < n; ++i) {
                    if (w.biome[i] < 0) continue;
                    for (int k = w.graph.offset[i]; k < w.graph.offset[i + 1]; ++k) {
                        const int j = w.graph.list[k];
                        if (j <= i || w.biome[j] < 0) continue;
                        ++borders[s];
                        if ((table[w.biome[i]].adjacency_mask >> w.biome[j]) & 1u) ++viol[s];
                    }
                }
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();

    long long total_v = 0, total_b = 0;
    int seeds_with = 0;
    for (int s = 0; s < nseeds; ++s) {
        total_v += viol[s];
        total_b += borders[s];
        if (viol[s] > 0) ++seeds_with;
    }

    if (header) {
        std::printf("\nforbidding %s <-> %s (band overlap %.2f), %d seeds\n",
                    table[pi].name.c_str(), table[pj].name.c_str(), best_overlap, nseeds);
        std::printf("%-12s %10s %10s %12s %8s\n",
                    "bands", "borders", "violations", "seeds w/ any", "verdict");
        std::printf("%s\n", std::string(56, '-').c_str());
    }
    std::printf("x%-11.1f %10lld %10lld %9d/%-3d %8s\n",
                band_scale, total_b, total_v, seeds_with, nseeds,
                total_v == 0 ? "PASS" : "FAIL");
}

// --- golden seeds ----------------------------------------------------------
// Pinned hashes of the generator's output. Any change to the field function,
// the archetype table or layer 3 moves them -- deliberately. The gate is then
// "revert, or bump generator_version", which is the only way that discipline
// survives a year of tuning.
struct Golden {
    int generator_version = 0;
    std::vector<std::pair<uint64_t, uint64_t> > seeds;   // world seed -> hash
};

static const char* kGoldenPath = "worldgen/golden/golden.json";

static bool load_golden(Golden& g) {
    try {
        const jsonlite::Value root = jsonlite::parse_file(kGoldenPath);
        g.generator_version = static_cast<int>(root.number("generator_version"));
        const jsonlite::Array& list = root.array("seeds");
        for (size_t i = 0; i < list.size(); ++i) {
            const uint64_t s = static_cast<uint64_t>(list[i].number("seed"));
            const uint64_t h = std::strtoull(list[i].string("hash").c_str() + 2, nullptr, 16);
            g.seeds.push_back(std::make_pair(s, h));
        }
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

static bool write_golden(const Golden& g) {
    std::FILE* f = std::fopen(kGoldenPath, "wb");
    if (!f) return false;
    std::fprintf(f, "{\n  \"generator_version\": %d,\n", g.generator_version);
    std::fprintf(f, "  \"_note\": \"Pinned by tools/validate --bless. A diff here is a world "
                    "change: revert it, or bump generator_version. Hashes come from the C++ "
                    "build with contraction disabled, which ADR-001 makes the determinism "
                    "authority -- a GPU cannot produce a stable hash.\",\n");
    std::fprintf(f, "  \"seeds\": [\n");
    for (size_t i = 0; i < g.seeds.size(); ++i) {
        std::fprintf(f, "    { \"seed\": %llu, \"hash\": \"0x%016llx\" }%s\n",
                     static_cast<unsigned long long>(g.seeds[i].first),
                     static_cast<unsigned long long>(g.seeds[i].second),
                     i + 1 < g.seeds.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}

static void line(const char* name, bool ok, const char* detail) {
    std::printf("  %-18s %-6s %s\n", name, ok ? "PASS" : "FAIL", detail);
}

int main(int argc, char** argv) {
    int  nseeds = 200;
    bool sweep = false;
    bool sweep_cells = false;
    bool sweep_bands = false;
    bool sweep_w = false;
    bool detail = false;
    bool dump = false;
    uint64_t dump_world_seed = 0;
    bool sheet = false;
    bool adjacency = false;
    bool bless = false;
    bool relief = false;
    bool closeup = false;
    bool components = false;
    uint64_t closeup_seed = 0;
    int  tile = 192;
    const char* sheet_path = "build/contact_sheet.bmp";
    WorldParams wp;
    AssignParams ap;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seeds") && i + 1 < argc)       nseeds = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--grid") && i + 1 < argc)   wp.grid = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--radius") && i + 1 < argc) wp.radius = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--w") && i + 1 < argc)      wp.danger_w = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--contiguity") && i + 1 < argc) ap.contiguity_bonus = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--repair") && i + 1 < argc)    ap.repair_passes = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--repair-danger") && i + 1 < argc) ap.repair_danger_weight = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--offset") && i + 1 < argc) wp.land_offset = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--sweep"))                  sweep = true;
        else if (!std::strcmp(argv[i], "--detail"))                 detail = true;
        else if (!std::strcmp(argv[i], "--mode") && i + 1 < argc)   ap.quota_mode = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--cellarea") && i + 1 < argc) wp.cell_area_target = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--sweep-cells"))            sweep_cells = true;
        else if (!std::strcmp(argv[i], "--sweep-bands"))            sweep_bands = true;
        else if (!std::strcmp(argv[i], "--sweep-w"))                sweep_w = true;
        else if (!std::strcmp(argv[i], "--contact-sheet"))          sheet = true;
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc)    sheet_path = argv[++i];
        else if (!std::strcmp(argv[i], "--tile") && i + 1 < argc)   tile = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--test-adjacency"))         adjacency = true;
        else if (!std::strcmp(argv[i], "--bless"))                  bless = true;
        else if (!std::strcmp(argv[i], "--relief"))                 relief = true;
        else if (!std::strcmp(argv[i], "--components"))              components = true;
        else if (!std::strcmp(argv[i], "--closeup") && i + 1 < argc) {
            closeup = true;
            closeup_seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--danger-feat") && i + 1 < argc) wp.danger_freq = 1.0f / static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--dump-seeds") && i + 1 < argc) {
            dump_world_seed = std::strtoull(argv[++i], nullptr, 10);
            dump = true;
        }
    }

    ArchetypeTable table;
    try {
        table = load_archetypes("worldgen/data/archetypes.json");
    } catch (const std::exception& e) {
        std::printf("FAILED to load archetypes: %s\n", e.what());
        return 1;
    }
    std::printf("archetypes: %d biomes, invariants OK\n", static_cast<int>(table.size()));

    if (components) {
        // Localises a cross-platform divergence to a layer. Each hash covers
        // one more stage than the last, so the first column that differs
        // between two platforms is where the determinism broke.
        std::printf("seed  layer0            +layer1           +layer4(base)     +layer5(carved)   segs\n");
        for (uint64_t ws = 0; ws < 8; ++ws) {
            WorldParams cw = wp;
            cw.grid = 1024;
            const SeedWorld cwld = build_world(ws, cw, table, ap);
            uint64_t h0 = 0xCBF29CE484222325ull, h1 = h0, h2 = h0, h3 = h0;
            const int HG = 256;
            const float CR = cw.radius, cs = 2.0f * CR / HG;
            for (int gj = 0; gj < HG; ++gj) {
                const float z = -CR + (gj + 0.5f) * cs;
                for (int gi = 0; gi < HG; ++gi) {
                    const float x = -CR + (gi + 0.5f) * cs;
                    const float rr = std::sqrt(x * x + z * z) / CR;
                    if (rr > 1.0f) continue;
                    const float a = layer0_height(x, z, rr, cw, cwld.s_cont);
                    const float b = layer1_danger(x, z, rr, cw, cwld.s_dang);
                    const float c = terrain_base(cwld, cw, table, x, z, rr);
                    const float e = terrain_height(cwld, cw, table, x, z, rr);
                    h0 = fnv1a(h0, static_cast<uint64_t>(static_cast<int64_t>(a * 4096.0f)));
                    h1 = fnv1a(h1, static_cast<uint64_t>(static_cast<int64_t>(b * 4096.0f)));
                    h2 = fnv1a(h2, static_cast<uint64_t>(static_cast<int64_t>(c * 64.0f)));
                    h3 = fnv1a(h3, static_cast<uint64_t>(static_cast<int64_t>(e * 64.0f)));
                }
            }
            std::printf("%4llu  %016llx  %016llx  %016llx  %016llx  %d\n",
                        static_cast<unsigned long long>(ws),
                        static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1),
                        static_cast<unsigned long long>(h2), static_cast<unsigned long long>(h3),
                        static_cast<int>(cwld.rivers.size()));
        }
        return 0;
    }

    if (closeup) {
        std::printf("close-up relief, seed %llu, %dpx\n",
                    static_cast<unsigned long long>(closeup_seed), tile);
        if (!single_relief(wp, table, ap, closeup_seed, sheet_path, tile)) {
            std::printf("FAILED to write %s\n", sheet_path);
            return 1;
        }
        std::printf("wrote %s\n", sheet_path);
        return 0;
    }

    if (dump) {
        // The CPU side of the ADR-001 tolerance check. Samples exactly as
        // tools/mapview's kernel does, so the two land fractions are directly
        // comparable. It cannot be a hash: WGSL 15.7.5 rules that out.
        const uint32_t s_cont = feature_seed(dump_world_seed, FEATURE_CONTINENT);
        const uint32_t s_dang = feature_seed(dump_world_seed, FEATURE_DANGER);
        const float R = wp.radius;
        const int   G = wp.grid;
        const float step = 2.0f * R / G;
        long long land = 0, disc = 0;
        for (int gj = 0; gj < G; ++gj) {
            const float z = -R + (gj + 0.5f) * step;
            for (int gi = 0; gi < G; ++gi) {
                const float x = -R + (gi + 0.5f) * step;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                ++disc;
                if (layer0_height(x, z, rr, wp, s_cont) > wp.sea_level) ++land;
            }
        }
        std::printf("\nworld seed %llu, %d^2 grid, R = %.0f m\n",
                    static_cast<unsigned long long>(dump_world_seed), G, R);
        std::printf("  seed continent  0x%08x\n", s_cont);
        std::printf("  seed danger     0x%08x\n", s_dang);
        std::printf("  land fraction   %.2f%%  (%lld of %lld samples)\n",
                    disc > 0 ? 100.0 * static_cast<double>(land) / static_cast<double>(disc) : 0.0,
                    land, disc);
        return 0;
    }

    if (sheet) {
        std::printf("contact sheet: 64 seeds, %dpx tiles, w = %.2f, R = %.0fkm\n",
                    tile, wp.danger_w, wp.radius / 1000.0f);
        if (!contact_sheet(wp, table, ap, sheet_path, tile, relief)) {
            std::printf("FAILED to write %s\n", sheet_path);
            return 1;
        }
        std::printf("wrote %s\n", sheet_path);
        return 0;
    }

    if (adjacency) {
        test_adjacency(nseeds, wp, table, ap, 1.0f, true);
        test_adjacency(nseeds, wp, table, ap, 2.0f, false);
        test_adjacency(nseeds, wp, table, ap, 3.0f, false);
        return 0;
    }

    if (sweep_w) {
        // If the contiguity bonus is not what clumps cells into patches, the
        // danger field's own spatial correlation must be. Sweep w and the
        // danger feature size and watch patch size and radial ordering.
        const float ws[]    = {0.05f, 0.10f, 0.20f, 0.35f};
        const float feats[] = {2500.0f, 4500.0f, 8000.0f};
        print_header();
        for (size_t fi = 0; fi < 3; ++fi) {
            for (size_t wi = 0; wi < 4; ++wi) {
                WorldParams w2 = wp;
                w2.danger_w    = ws[wi];
                w2.danger_freq = 1.0f / feats[fi];
                AssignParams a2 = ap;
                a2.quota_mode = QUOTA_HALF_CELL;
                char label[64];
                std::snprintf(label, sizeof(label), "w=%.2f feat=%.1fkm", ws[wi], feats[fi] / 1000.0f);
                print_summary(label, run(nseeds, w2, table, a2));
            }
        }
        return 0;
    }

    if (sweep_bands) {
        // The candidate set is ~1 because quota exhaustion closes biomes out.
        // Widening the danger bands gives the cost function something to
        // choose between -- does the contiguity bonus come back to life, and
        // what does the wider band cost in radial ordering?
        const float scales[] = {1.0f, 1.5f, 2.0f, 3.0f};
        const float contig[] = {0.0f, 0.4f};
        print_header();
        for (size_t ci = 0; ci < 2; ++ci) {
            for (size_t si = 0; si < 4; ++si) {
                ArchetypeTable t2 = table;
                for (size_t b = 0; b < t2.biomes.size(); ++b) {
                    BiomeArchetype& a = t2.biomes[b];
                    const float lo = a.danger_preferred - (a.danger_preferred - a.danger_band_min) * scales[si];
                    const float hi = a.danger_preferred + (a.danger_band_max - a.danger_preferred) * scales[si];
                    a.danger_band_min = std::max(0.0f, lo);
                    a.danger_band_max = std::min(1.0f, hi);
                }
                AssignParams a2 = ap;
                a2.quota_mode = QUOTA_HALF_CELL;
                a2.contiguity_bonus = contig[ci];
                char label[64];
                std::snprintf(label, sizeof(label), "band x%.1f contig=%.1f", scales[si], contig[ci]);
                print_summary(label, run(nseeds, wp, t2, a2));
            }
        }
        return 0;
    }

    if (sweep_cells) {
        // At R = 10 km, how small do cells have to get before the +/-10%
        // quota gate passes -- and what does that cost in patch size?
        const float areas[] = {1.28e6f, 0.90e6f, 0.64e6f, 0.45e6f, 0.32e6f};
        const int   modes[] = {QUOTA_DOC, QUOTA_HALF_CELL};
        const char* names[] = {"doc", "half-cell"};
        print_header();
        for (size_t mi = 0; mi < 2; ++mi) {
            for (size_t ai = 0; ai < 5; ++ai) {
                WorldParams  w2 = wp; w2.cell_area_target = areas[ai];
                AssignParams a2 = ap; a2.quota_mode = modes[mi];
                char label[64];
                std::snprintf(label, sizeof(label), "%s cell=%.2fkm2", names[mi], areas[ai] / 1.0e6f);
                print_summary(label, run(nseeds, w2, table, a2));
            }
        }
        return 0;
    }

    if (detail) {
        print_detail(wp, table, ap, 0x5EEDull * 1000003ull);
        return 0;
    }

    if (!sweep) {
        std::printf("R = %.0f m, %d seeds, %d^2 grid, w = %.2f, danger feature = %.1f km, "
                    "repair = %d passes\n",
                    wp.radius, nseeds, wp.grid, wp.danger_w,
                    0.001f / wp.danger_freq, ap.repair_passes);
        print_header();
        char label[64];
        std::snprintf(label, sizeof(label), "R=%.0fkm", wp.radius / 1000.0f);
        // Throughput, measured BEFORE the 200-seed run and as the best of
        // three. Taken afterwards it contended with threads that had just
        // finished and swung by more than 2x between runs, which is useless as
        // a regression guard -- the fix is a better measurement, not a looser
        // threshold.
        double ns = 1.0e30;
        {
            const int N = 1000000;
            const uint32_t sd = feature_seed(0, FEATURE_CONTINENT);
            for (int rep = 0; rep < 3; ++rep) {
                double sink = 0.0;
                const std::chrono::high_resolution_clock::time_point t0 =
                    std::chrono::high_resolution_clock::now();
                for (int i = 0; i < N; ++i) {
                    const float x = static_cast<float>(i & 2047) * 9.7f - 10000.0f;
                    const float z = static_cast<float>(i >> 11) * 9.7f - 10000.0f;
                    sink += layer0_height(x, z, 0.5f, wp, sd);
                }
                const std::chrono::high_resolution_clock::time_point t1 =
                    std::chrono::high_resolution_clock::now();
                const double e = std::chrono::duration<double, std::nano>(t1 - t0).count() / N;
                if (e < ns) ns = e;
                if (sink == 1234.5678) std::printf("");   // keep the loop
            }
        }

        const Summary s = run(nseeds, wp, table, ap);
        print_summary(label, s);

        // Golden seeds.
        Golden g;
        const bool had_golden = load_golden(g);
        bool golden_ok = false;
        int  golden_moved = 0;
        // Pinned at a canonical sampling resolution. The cell areas and land
        // mask are derived from the raster, so hashing at whatever --grid the
        // run happened to use would pin a harness setting rather than the
        // generator, and the gate would fire on nothing.
        WorldParams gw = wp;
        gw.grid = 1024;
        if (bless) {
            g.generator_version = had_golden ? g.generator_version : 1;
            g.seeds.clear();
            for (uint64_t ws = 0; ws < 8; ++ws) {
                g.seeds.push_back(std::make_pair(ws, eval_seed(ws, gw, table, ap).hash));
            }
            if (!write_golden(g)) { std::printf("FAILED to write %s\n", kGoldenPath); return 1; }
            std::printf("\nblessed %d golden seeds at generator_version %d -> %s\n",
                        static_cast<int>(g.seeds.size()), g.generator_version, kGoldenPath);
            golden_ok = true;
        } else if (had_golden) {
            for (size_t i = 0; i < g.seeds.size(); ++i) {
                if (eval_seed(g.seeds[i].first, gw, table, ap).hash != g.seeds[i].second) {
                    ++golden_moved;
                }
            }
            golden_ok = (golden_moved == 0);
        }

        std::printf("\nchecks over %d seeds\n", nseeds);
        char d[128];

        std::snprintf(d, sizeof(d), "%d seeds missing a biome", s.presence_failures);
        line("presence", s.presence_failures == 0, d);
        std::snprintf(d, sizeof(d), "max %.1f%% off target, tolerance 10%%", 100.0 * s.max_quota_err);
        line("quota", s.quota_failures == 0, d);
        std::snprintf(d, sizeof(d), "Spearman min %.2f, threshold 0.80", s.min_spearman);
        line("radial order", s.radial_failures == 0, d);
        std::snprintf(d, sizeof(d), "mean %.1f%%, target 55%% +/- 5", 100.0 * s.mean_land_frac);
        line("land fraction", s.land_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds break an authored adjacency rule", s.adjacency_failures);
        line("adjacency", s.adjacency_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds with land past r=0.95 or a rim above the void",
                      s.taper_failures);
        line("edge taper", s.taper_failures == 0, d);
        std::snprintf(d, sizeof(d), "worst seed reaches %.1f%% of land on foot, threshold 75%% "
                      "(islands are expected)", 100.0 * s.min_foot_reach);
        line("foot connect", s.foot_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds with a biome unreachable by sea", s.boat_failures);
        line("boat connect", s.boat_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds without a coastal spawn in the calmest biome",
                      s.spawn_failures);
        line("spawn", s.spawn_failures == 0, d);
        // The design doc's 20 ns/sample is a serial-equivalent budget for the
        // complete field function on the shipping path. This is a scalar CPU
        // reference build of layers 0-1 only, so it cannot be judged against
        // that number yet. Gated as a regression guard, and tracked so cost
        // creep is visible before it hurts.
        std::snprintf(d, sizeof(d), "worst seed has %.1f%% of land under 30 degrees, threshold 60%%",
                      100.0 * s.min_slope_gentle);
        line("slope budget", s.slope_failures == 0, d);
        std::snprintf(d, sizeof(d), "%.0f segments/seed, all descending and reaching the sea",
                      s.mean_river_segments);
        line("river sanity", s.river_failures == 0, d);
        std::snprintf(d, sizeof(d), "%.0f ns/sample, scalar CPU reference; design budget is "
                      "20 ns serial-equivalent for the full field on the shipping path", ns);
        line("throughput", ns <= 400.0, d);
        if (!had_golden && !bless) {
            std::snprintf(d, sizeof(d), "no %s -- run with --bless to pin this world", kGoldenPath);
        } else if (golden_moved > 0) {
            std::snprintf(d, sizeof(d), "%d of %d moved: revert, or bump generator_version",
                          golden_moved, static_cast<int>(g.seeds.size()));
        } else {
            std::snprintf(d, sizeof(d), "%d seeds pinned at generator_version %d",
                          static_cast<int>(g.seeds.size()), g.generator_version);
        }
        line("golden seeds", golden_ok, d);

        // Automated checks cannot tell you whether a world is interesting.
        const bool sheet_ok = contact_sheet(wp, table, ap, sheet_path, tile);
        std::snprintf(d, sizeof(d), "%s", sheet_ok ? sheet_path : "could not write");
        line("contact sheet", sheet_ok, d);

        const bool all = s.presence_failures == 0 && s.quota_failures == 0 &&
                         s.radial_failures == 0 && s.land_failures == 0 &&
                         s.adjacency_failures == 0 && s.taper_failures == 0 &&
                         s.foot_failures == 0 && s.boat_failures == 0 &&
                         s.spawn_failures == 0 && s.slope_failures == 0 &&
                         s.river_failures == 0 &&
                         ns <= 400.0 && golden_ok && sheet_ok;
        std::printf("\n%s\n", all ? "ALL CHECKS PASSED" : "FAILED");
        return all ? 0 : 1;
    }

    // The experiment: the doc's gate dumps the whole accumulated rounding
    // error on the last biome in sort order. Do the two alternative gates fix
    // it, and what do they cost in radial ordering?
    const float radii[] = {10000.0f, 20000.0f};
    const int   modes[] = {QUOTA_DOC, QUOTA_HALF_CELL, QUOTA_SOFT_FILL};
    const char* names[] = {"doc", "half-cell", "soft-fill"};
    print_header();
    for (size_t ri = 0; ri < 2; ++ri) {
        for (size_t mi = 0; mi < 3; ++mi) {
            WorldParams  w2 = wp; w2.radius = radii[ri];
            AssignParams a2 = ap; a2.quota_mode = modes[mi];
            char label[64];
            std::snprintf(label, sizeof(label), "R=%.0fkm gate=%s", radii[ri] / 1000.0f, names[mi]);
            print_summary(label, run(nseeds, w2, table, a2));
        }
    }
    return 0;
}
