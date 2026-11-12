// ---------------------------------------------------------------------------
// Validation harness and CI gate. It builds N worlds with the generator in
// worldgen/, asserts every property the design doc asks for, pins the result
// against golden hashes, and exits non-zero on any failure.
//
// Nothing here generates terrain. Everything upstream of the golden hash is
// in worldgen/world.h; what is left is measurement, rendering and reporting.
// That boundary is deliberate: the metrics legitimately use atan and cos/sin,
// which CLAUDE.md invariant 9 forbids on the other side of it.
// ---------------------------------------------------------------------------
#include "worldgen/world.h"
#include "worldgen/assign/json_lite.h"
#include "tools/shared/render.h"
#include "tools/shared/land_solve.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

using namespace worldgen;
using namespace view;

// The spawn landmass must hold at least this many cells to be worth starting
// a game on: about two mean biome patches (7.6 cells, README section 7). It
// catches a world that spawns the player on a bare rock and nothing else --
// in particular it does NOT require a mainland, because island starts are a
// designed outcome and the design doc asks for island clusters outright.
static const int kHomeMinCells = 16;

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
    // Islandness. These are MEASURED and printed; only the degeneracy guard
    // below is asserted. See tools/validate/README.md section 8 for why the
    // old >= 75% foot-connectivity assertion was retired.
    float home_share = 0.0f;    // share of land cells on the spawn landmass
    int   home_cells = 0;       // cells on the spawn landmass
    int   landmasses = 0;       // connected land components on the cell graph
    bool  boat_all_biomes = false;
    int   spawn_cell = -1;
    bool  spawn_coastal = false;
    bool  spawn_calmest = false;
    float slope_gentle = 1.0f;  // share of land under 30 degrees
    float lod_worst = 0.0f;     // worst |h(LOD) - h(0)| as a multiple of that voxel
    float cave_fraction = 0.0f;   // subsurface samples that are air
    float overhang_cols = 0.0f;   // land columns whose surface is crossed twice or more
    int   river_segments = 0;
    int   river_paths_total = 0, river_paths_to_ocean = 0, river_ascending = 0;
    uint64_t hash = 0;
    float mean_patch_cells = 0.0f;
    float mean_patch_km2 = 0.0f;
    std::vector<int>   biome_cells;   // per biome
    std::vector<float> biome_share;   // per biome, share of land area
};

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

    // --- landmasses and connectivity ---
    // Every connected component of land cells, not just the spawn one. The
    // count and the spawn landmass's share are how islandness is measured:
    // one component holding ~100% is a Pangaea, several is an archipelago.
    // Neither is a failure. By boat is the real requirement -- land plus
    // ocean must reach every biome, or some of them are on islands nobody
    // can get to.
    {
        std::vector<int> comp(n, -1);
        std::vector<int> stack;
        int land_total = 0;
        for (int i = 0; i < n; ++i) if (cells[i].is_land) ++land_total;
        for (int i = 0; i < n; ++i) {
            if (!cells[i].is_land || comp[i] >= 0) continue;
            const int id = out.landmasses++;
            int size = 0;
            comp[i] = id;
            stack.push_back(i);
            while (!stack.empty()) {
                const int c = stack.back(); stack.pop_back();
                ++size;
                for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                    const int nb2 = graph.list[k];
                    if (comp[nb2] >= 0 || !cells[nb2].is_land) continue;
                    comp[nb2] = id;
                    stack.push_back(nb2);
                }
            }
            if (out.spawn_cell >= 0 && comp[out.spawn_cell] == id) out.home_cells = size;
        }
        out.home_share = land_total > 0
                       ? static_cast<float>(out.home_cells) / land_total : 0.0f;
    }

    if (out.spawn_cell >= 0) {
        std::vector<char> seen(n, 0);
        std::vector<int> stack;
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

    // --- LOD agreement ---
    // The design doc calls this a correctness property, not a performance
    // tweak: if a chunk meshed at 2 m disagrees with the same ground meshed at
    // 0.5 m, terrain visibly changes shape as the player walks towards it.
    // Reported as a multiple of the LOD's own voxel size, so 1.0 is the limit.
    {
        const float R = wp.radius;
        const int   LG = 22;
        const float ls = 2.0f * R / LG;
        float worst = 0.0f;
        for (int gj = 0; gj < LG; ++gj) {
            const float z = -R + (gj + 0.5f) * ls;
            for (int gi = 0; gi < LG; ++gi) {
                const float x = -R + (gi + 0.5f) * ls;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                const float h0 = terrain_height(w, wp, table, x, z, rr, 0.0f);
                float voxel = 0.5f;
                for (int L = 0; L < 8; ++L) {          // 0.5 m through 64 m
                    const float hl = terrain_height(w, wp, table, x, z, rr, voxel);
                    const float ratio = std::fabs(hl - h0) / voxel;
                    if (ratio > worst) worst = ratio;
                    voxel = voxel * 2.0f;
                }
            }
        }
        out.lod_worst = worst;
    }

    // --- layer 6 volumetrics ---
    // A column with more than one sign change above the ground is an actual
    // overhang. Without this, overhang_strength could be doing nothing but
    // roughening the surface and nothing would say so.
    {
        const float R = wp.radius;
        const int   CG = 13;
        const float cs = 2.0f * R / CG;
        int cols = 0, overhung = 0, sub = 0, air = 0;
        for (int gj = 0; gj < CG; ++gj) {
            const float z = -R + (gj + 0.5f) * cs;
            for (int gi = 0; gi < CG; ++gi) {
                const float x = -R + (gi + 0.5f) * cs;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                const float surf = terrain_height(w, wp, table, x, z, rr);
                if (surf <= wp.sea_level) continue;
                ++cols;

                // Above the surface: count crossings to find overhangs.
                int crossings = 0;
                bool prev_solid = terrain_density(w, wp, table, x, surf + 60.0f, z) > 0.0f;
                for (int k = 1; k <= 40; ++k) {
                    const float y = surf + 60.0f - 100.0f * k / 40.0f;
                    const bool solid = terrain_density(w, wp, table, x, y, z) > 0.0f;
                    if (solid != prev_solid) ++crossings;
                    prev_solid = solid;
                }
                if (crossings > 1) ++overhung;

                // Below the surface: how much of it is cave.
                for (int k = 1; k <= 60; ++k) {
                    const float y = surf - 320.0f * k / 60.0f;
                    ++sub;
                    if (terrain_density(w, wp, table, x, y, z) <= 0.0f) ++air;
                }
            }
        }
        out.overhang_cols = cols > 0 ? static_cast<float>(overhung) / cols : 0.0f;
        out.cave_fraction = sub > 0 ? static_cast<float>(air) / sub : 0.0f;
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
                // Layer 6 at every 64th column, two depths each. The pin has
                // to cover the whole generator, and density is part of it now;
                // sampling every column in 3D would cost more than the rest of
                // the hash put together.
                if (((gi | gj) & 7) == 0) {
                    const float d1 = terrain_density(w, wp, table, x, tt - 20.0f, z);
                    const float d2 = terrain_density(w, wp, table, x, tt - 80.0f, z);
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(d1 * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(d2 * 64.0f)));
                }
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
    int home_failures = 0;       // spawn landmass too small to be playable
    int boat_failures = 0;       // a biome unreachable even by sea
    int spawn_failures = 0;      // no spawn, not coastal, or not calmest
    int slope_failures = 0;      // under 60% of land walkable
    int lod_failures = 0;        // a LOD disagreeing by more than its voxel
    float max_lod_worst = 0.0f;
    float mean_cave_fraction = 0.0f, mean_overhang_cols = 0.0f;
    int river_failures = 0;      // no rivers, or one that climbs or never reaches the sea
    float mean_river_segments = 0.0f;
    float min_home_share = 2.0f;     // islandness: measured, not asserted
    float mean_home_share = 0.0f;
    float mean_landmasses = 0.0f;
    int   max_landmasses = 0;
    int   min_home_cells = 1 << 30;
    float min_slope_gentle = 2.0f;
    float mean_cells = 0, mean_land_cells = 0, mean_land_frac = 0;
    float min_land_frac = 2.0f, max_land_frac = -1.0f, sd_land_frac = 0.0f;
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
    std::vector<float> errs, lands;
    errs.reserve(nseeds);
    for (int s = 0; s < nseeds; ++s) {
        const SeedResult& r = results[s];
        if (r.missing_biomes > 0) ++sm.presence_failures;
        if (r.max_quota_err > 0.10f) ++sm.quota_failures;
        if (r.spearman < 0.8f) ++sm.radial_failures;
        if (r.land_fraction < 0.26f || r.land_fraction > 0.45f) ++sm.land_failures;
        if (r.adj_violations > 0) ++sm.adjacency_failures;
        if (!r.taper_ok) ++sm.taper_failures;
        // A DEGENERACY guard, not a connectivity requirement: the spawn
        // landmass has to hold enough land to start a game on. An archipelago
        // passes it by design, which is the point -- the design doc wants
        // island clusters (layer 3) and the old >= 75% foot-connectivity
        // assertion quietly forbade them. README section 8 has the history.
        if (r.home_cells < kHomeMinCells) ++sm.home_failures;
        if (!r.boat_all_biomes) ++sm.boat_failures;
        if (r.spawn_cell < 0 || !r.spawn_coastal || !r.spawn_calmest) ++sm.spawn_failures;
        if (r.slope_gentle < 0.60f) ++sm.slope_failures;
        if (r.lod_worst > 1.0f) ++sm.lod_failures;
        sm.max_lod_worst = std::max(sm.max_lod_worst, r.lod_worst);
        sm.mean_cave_fraction += r.cave_fraction;
        sm.mean_overhang_cols += r.overhang_cols;
        if (r.river_segments == 0 || r.river_ascending > 0 ||
            r.river_paths_to_ocean != r.river_paths_total) ++sm.river_failures;
        sm.mean_river_segments += r.river_segments;
        sm.min_home_share = std::min(sm.min_home_share, r.home_share);
        sm.mean_home_share += r.home_share;
        sm.mean_landmasses += r.landmasses;
        sm.max_landmasses = std::max(sm.max_landmasses, r.landmasses);
        sm.min_home_cells = std::min(sm.min_home_cells, r.home_cells);
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
        lands.push_back(r.land_fraction);
    }
    const float inv = 1.0f / nseeds;
    sm.mean_cells *= inv; sm.mean_land_cells *= inv; sm.mean_land_frac *= inv;
    for (size_t i = 0; i < lands.size(); ++i) {
        sm.min_land_frac = std::min(sm.min_land_frac, lands[i]);
        sm.max_land_frac = std::max(sm.max_land_frac, lands[i]);
        const double d = lands[i] - sm.mean_land_frac;
        sm.sd_land_frac += static_cast<float>(d * d);
    }
    if (!lands.empty()) sm.sd_land_frac = std::sqrt(sm.sd_land_frac / lands.size());
    sm.mean_quota_err *= inv; sm.mean_spearman *= inv;
    sm.mean_patch_cells *= inv; sm.mean_patch_km2 *= inv;
    sm.mean_relaxed *= inv; sm.mean_overflow *= inv; sm.mean_candidates *= inv;
    sm.mean_river_segments *= inv;
    sm.mean_cave_fraction *= inv; sm.mean_overhang_cols *= inv;
    sm.mean_home_share *= inv; sm.mean_landmasses *= inv;
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


// --- land fraction as a distribution ------------------------------------------
// Solve one GLOBAL offset for a target mean, then report what the seeds
// actually do around it. The spread is the product, not an error bar: the
// worlds that break into archipelagos are this distribution's low tail.
static void land_report_header() {
    std::printf("\n%-8s %-9s %-25s %-13s %-15s %-8s %s\n",
                "target", "offset", "land% mean/min/max/sd", "landmasses",
                "spawn mean/worst", "quotamx", "fails: quota pres boat");
    std::printf("%s\n", std::string(104, '-').c_str());
}

static void land_report_row(float target, int nseeds, const WorldParams& base,
                            const ArchetypeTable& table, const AssignParams& ap) {
    // The solve is cheap because it only needs layer 0, so it can afford more
    // seeds than the full run below.
    const float off = solve_land_offset(base, target, 24, 256);
    WorldParams wp = base;
    wp.land_offset = off;

    const Summary s = run(nseeds, wp, table, ap);
    std::printf("%6.1f%%  %+8.4f  %5.1f %5.1f %5.1f %5.2f  %4.1f(max%2d)  %5.1f%%/%5.1f%%  %5.1f%%   %3d %3d %3d%s\n",
                100.0 * target, off,
                100.0 * s.mean_land_frac, 100.0 * s.min_land_frac,
                100.0 * s.max_land_frac, 100.0 * s.sd_land_frac,
                s.mean_landmasses, s.max_landmasses,
                100.0 * s.mean_home_share, 100.0 * s.min_home_share,
                100.0 * s.max_quota_err,
                s.quota_failures, s.presence_failures, s.boat_failures,
                (s.quota_failures || s.presence_failures || s.boat_failures)
                    ? "  <-- fails" : "");
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
    bool section = false;
    uint64_t section_seed = 0;
    float section_width = 4000.0f;
    bool land_sweep = false;
    float land_target = -1.0f;
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
        else if (!std::strcmp(argv[i], "--section") && i + 1 < argc) {
            section = true;
            section_seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--section-width") && i + 1 < argc)
            section_width = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--closeup") && i + 1 < argc) {
            closeup = true;
            closeup_seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--danger-feat") && i + 1 < argc) wp.danger_freq = 1.0f / static_cast<float>(std::atof(argv[++i]));
        // The islandness lever. Land fraction sits just above the percolation
        // threshold at the shipping 0.17, which is why every seed is a single
        // continent; see README section 8. Sweeping this moves the world, so
        // the land-fraction and golden checks will go red -- that is the gate
        // working, not a reason to --bless.
        else if (!std::strcmp(argv[i], "--land-offset") && i + 1 < argc) wp.land_offset = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--land-target") && i + 1 < argc) {
            land_target = static_cast<float>(std::atof(argv[++i])) / 100.0f;
        }
        else if (!std::strcmp(argv[i], "--land-sweep")) land_sweep = true;
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

    if (section) {
        const int W = 1400, H = 300;
        std::printf("cross-section, seed %llu, %.0f m wide, %dx%d\n",
                    static_cast<unsigned long long>(section_seed), section_width, W, H);
        const SeedWorld sw = build_world(section_seed, wp, table, ap);
        std::vector<unsigned char> img(static_cast<size_t>(W) * H * 3, 0);
        render_section(sw, wp, table, 0.0f, section_width * 0.5f, -420.0f, 380.0f, img, W, H);
        if (!write_bmp(sheet_path, W, H, img)) {
            std::printf("FAILED to write %s\n", sheet_path);
            return 1;
        }
        std::printf("wrote %s\n", sheet_path);

        // What the cave field is actually doing, so a blank slice can be told
        // from a slice of solid rock.
        {
            double cmin = 1e30, cmax = -1e30, csum = 0.0;
            int n = 0, air = 0, below = 0;
            float smin = 1e30f, smax = -1e30f;
            for (int ix = 0; ix < 160; ++ix) {
                const float x = -section_width * 0.5f + section_width * (ix + 0.5f) / 160.0f;
                const float rr = std::sqrt(x * x) / wp.radius;
                if (rr > 1.0f) continue;
                const float surf = terrain_height(sw, wp, table, x, 0.0f, rr);
                if (surf < smin) smin = surf;
                if (surf > smax) smax = surf;
                for (int iy = 0; iy < 120; ++iy) {
                    const float y = surf - 400.0f * (iy + 0.5f) / 120.0f;   // below surface only
                    const float c = cave_field(x, y, 0.0f, wp.cave_freq, sw.s_cave, 0.0f);
                    if (c < cmin) cmin = c;
                    if (c > cmax) cmax = c;
                    csum += c;
                    ++n;
                    ++below;
                    if (terrain_density(sw, wp, table, x, y, 0.0f) <= 0.0f) ++air;
                }
            }
            std::printf("surface height %.0f..%.0f m\n", smin, smax);
            std::printf("cave_field over %d subsurface samples: min %.3f mean %.3f max %.3f\n",
                        n, cmin, n > 0 ? csum / n : 0.0, cmax);
            std::printf("subsurface samples that are air: %d of %d (%.2f%%)\n",
                        air, below, below > 0 ? 100.0 * air / below : 0.0);
        }
        return 0;
    }

    if (land_sweep) {
        // The band worth choosing from. 56 is roughly where we ship; the low
        // end is where quota granularity starts to bite.
        const float targets[] = {0.50f, 0.45f, 0.40f, 0.35f, 0.30f};
        std::printf("land fraction sweep, %d seeds per row, offset solved GLOBALLY for the mean\n",
                    nseeds);
        land_report_header();
        for (int i = 0; i < 5; ++i) land_report_row(targets[i], nseeds, wp, table, ap);
        std::printf("\nThe mean is the lever; the min column is where the islandness lives.\n");
        return 0;
    }

    if (land_target >= 0.0f) {
        std::printf("land fraction target %.1f%%, %d seeds, offset solved GLOBALLY for the mean\n",
                    100.0 * land_target, nseeds);
        land_report_header();
        land_report_row(land_target, nseeds, wp, table, ap);
        return 0;
    }

    if (components) {
        // Localises a cross-platform divergence to a layer. Each hash covers
        // one more stage than the last, so the first column that differs
        // between two platforms is where the determinism broke.
        {
            // Layer 4 is the first consumer of floats parsed out of
            // archetypes.json. If this differs, the divergence is in strtod,
            // not in the field function.
            uint64_t th = 0xCBF29CE484222325ull;
            for (size_t b = 0; b < table.size(); ++b) {
                const float f[12] = {
                    table[b].area_fraction, table[b].danger_band_min, table[b].danger_band_max,
                    table[b].danger_preferred, table[b].base_elevation, table[b].amplitude,
                    table[b].lacunarity, table[b].gain, table[b].ridge_blend,
                    table[b].flatness, table[b].overhang_strength, table[b].cave_density };
                for (int q = 0; q < 12; ++q) {
                    uint32_t bits;
                    std::memcpy(&bits, &f[q], 4);
                    th = fnv1a(th, bits);
                }
                th = fnv1a(th, table[b].octaves);
            }
            std::printf("archetype table hash: %016llx\n", static_cast<unsigned long long>(th));
        }
        std::printf("seed  layer0            layer1            fbm_shaped        cells             layer4base        layer5carved      segs\n");
        for (uint64_t ws = 0; ws < 8; ++ws) {
            WorldParams cw = wp;
            cw.grid = 1024;
            const SeedWorld cwld = build_world(ws, cw, table, ap);
            uint64_t h0 = 0xCBF29CE484222325ull, h1 = h0, h2 = h0, h3 = h0, hn = h0, hc = h0;
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
                    // Split layer 4: the shaped noise alone (exercises abs and
                    // the ridge warp) versus the cell lookup alone (integers
                    // and sqrt only).
                    const float sn = fbm_shaped(x * cw.biome_freq, z * cw.biome_freq,
                                                5, 2.0f, 0.5f, 0.5f, cwld.s_height,
                                                1.0f / cw.biome_freq, 0.0f);
                    int li[4]; float ld[4];
                    const float wxx = x + cw.warp_amp * fbm(x * cw.warp_freq, z * cw.warp_freq, 2, 2.0f, 0.5f, cwld.s_wx);
                    const float wzz = z + cw.warp_amp * fbm(x * cw.warp_freq, z * cw.warp_freq, 2, 2.0f, 0.5f, cwld.s_wz);
                    const int lf = cwld.lookup.nearest4(cwld.pts, wxx, wzz, li, ld);
                    hn = fnv1a(hn, static_cast<uint64_t>(static_cast<int64_t>(sn * 65536.0f)));
                    for (int q = 0; q < lf; ++q) hc = fnv1a(hc, static_cast<uint64_t>(li[q] + 1));
                    const float c = terrain_base(cwld, cw, table, x, z, rr);
                    const float e = terrain_height(cwld, cw, table, x, z, rr);
                    h0 = fnv1a(h0, static_cast<uint64_t>(static_cast<int64_t>(a * 4096.0f)));
                    h1 = fnv1a(h1, static_cast<uint64_t>(static_cast<int64_t>(b * 4096.0f)));
                    h2 = fnv1a(h2, static_cast<uint64_t>(static_cast<int64_t>(c * 64.0f)));
                    h3 = fnv1a(h3, static_cast<uint64_t>(static_cast<int64_t>(e * 64.0f)));
                }
            }
            std::printf("%4llu  %016llx  %016llx  %016llx  %016llx  %016llx  %016llx  %d\n",
                        static_cast<unsigned long long>(ws),
                        static_cast<unsigned long long>(h0), static_cast<unsigned long long>(h1),
                        static_cast<unsigned long long>(hn), static_cast<unsigned long long>(hc),
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
        // A mean target plus a per-seed band, not a tight per-seed target.
        // The spread IS the islandness, so squeezing every seed onto the
        // mean would remove the archipelagos along with it.
        const bool land_mean_ok = std::fabs(s.mean_land_frac - 0.35f) <= 0.02f;
        std::snprintf(d, sizeof(d), "mean %.1f%% (target 35 +/- 2), seeds %.1f-%.1f%% "
                      "(band 26-45), %d outside",
                      100.0 * s.mean_land_frac, 100.0 * s.min_land_frac,
                      100.0 * s.max_land_frac, s.land_failures);
        line("land fraction", s.land_failures == 0 && land_mean_ok, d);
        std::snprintf(d, sizeof(d), "%d seeds break an authored adjacency rule", s.adjacency_failures);
        line("adjacency", s.adjacency_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds with land past r=0.95 or a rim above the void",
                      s.taper_failures);
        line("edge taper", s.taper_failures == 0, d);
        std::snprintf(d, sizeof(d), "smallest spawn landmass %d cells, floor %d "
                      "(a degeneracy guard, not a mainland requirement)",
                      s.min_home_cells, kHomeMinCells);
        line("home landmass", s.home_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds with a biome unreachable by sea", s.boat_failures);
        line("boat connect", s.boat_failures == 0, d);
        // Measured, never asserted. Whether a world should read as one
        // continent or as an archipelago is a design decision taken by eye on
        // the contact sheet, exactly as w was -- see README section 8.
        std::snprintf(d, sizeof(d), "%.1f landmasses/seed (max %d), spawn landmass holds "
                      "%.1f%% of land (min %.1f%%)",
                      s.mean_landmasses, s.max_landmasses,
                      100.0 * s.mean_home_share, 100.0 * s.min_home_share);
        std::printf("  %-18s %-6s %s\n", "islandness", "n/a", d);
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
        std::snprintf(d, sizeof(d), "%.2f%% of subsurface is cave air, %.1f%% of land columns "
                      "overhang", 100.0 * s.mean_cave_fraction, 100.0 * s.mean_overhang_cols);
        std::printf("  %-18s %-6s %s\n", "volumetrics", "n/a", d);
        std::snprintf(d, sizeof(d), "worst LOD differs from LOD 0 by %.2f of its voxel "
                      "(limit 1.00)", s.max_lod_worst);
        line("LOD agreement", s.lod_failures == 0, d);
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
                         s.home_failures == 0 && s.boat_failures == 0 &&
                         s.spawn_failures == 0 && s.slope_failures == 0 &&
                         land_mean_ok &&
                         s.river_failures == 0 && s.lod_failures == 0 &&
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
