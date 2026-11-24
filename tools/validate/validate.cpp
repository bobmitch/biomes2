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
#include "tools/shared/reach.h"

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

#include "worldgen/generator_version.h"

using namespace worldgen;
using namespace view;

// The spawn landmass must hold at least this many cells to be worth starting
// a game on: about two mean biome patches (7.6 cells, README section 7). It
// catches a world that spawns the player on a bare rock and nothing else --
// in particular it does NOT require a mainland, because island starts are a
// designed outcome and the design doc asks for island clusters outright.
static const int kHomeMinCells = 16;

// Land fraction: the MEAN over seeds within 2 points of this, and every seed
// in [kLandSeedMin, 45%]. v18's layer 0 measured 29.4%, seeds 25.5-33.3%,
// over 200 seeds (32% and 26% through v17).
static const float kLandMeanTarget = 0.294f;
static const float kLandSeedMin = 0.25f;

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
    // Layer 0 prototype readings (docs/worldgen-design.md, "Layer 0,
    // proposed"). Printed, never asserted: the reach is never aimed.
    float raw_land = 0.0f, knee_land = 0.0f, knee_ms = 0.0f;  // the knee's grid
    float reach_share = 0.0f;   // land reached on foot from find_spawn
    int   reach_biomes = 0;     // biomes on it
    int   biomes_present = 0;
    float shelf_km2 = 0.0f;     // water under 0.85 m
    float regime_size = 0.0f, regime_mean = 0.0f, regime_amp = 0.0f;   // this seed's draws
    int   spawn_cell = -1;
    bool  spawn_coastal = false;
    bool  spawn_calmest = false;
    float slope_gentle = 1.0f;  // share of land under 30 degrees
    float lod_worst = 0.0f;     // worst |h(LOD) - h(0)| as a multiple of that voxel
    float seam_h = 0.0f;        // worst heightfield STEP where the nearest-4 cell set changes
    float seam_t = 0.0f;        // the same for the layer-6 3D term
    int   seam_crossings = 0;   // places measured
    float cave_fraction = 0.0f;   // subsurface samples that are air
    float overhang_cols = 0.0f;   // land columns whose surface is crossed twice or more
    int   entrance_trunks = 0;      // shafts that reached cave air
    int   entrance_segs = 0;        // every segment, branches included
    int   entrance_mouth_sealed = 0;
    int   entrance_pinched = 0;     // sampled shaft segments still solid inside
    float entrance_reach = 0.0f;    // mean metres down to the cave it found
    int   river_segments = 0;
    int   river_paths_total = 0, river_paths_to_ocean = 0, river_ascending = 0;
    // Layer 8, sampled: a 32 m square of candidate cells at every land cell's
    // site, counted per biome.
    std::vector<double> tree_count;   // per biome
    std::vector<double> tree_area;    // m^2 sampled per biome
    int   golden_trees = 0;           // trees that went into the hash
    // Layer 9, sampled the same way per class (0 boulders, 1 outcrops).
    std::vector<double> rock_count[2];
    std::vector<double> rock_area[2];
    int   golden_rocks = 0;           // rocks that went into the hash
    int   rocks_unseated = 0;         // pinned rocks whose ground is not air over rock
    // Layer 5b: flat sites, per biome, and how level they came out.
    std::vector<int> flat_by_biome;
    int   flat_count = 0;
    float flat_worst_pad = 0.0f;      // metres of height range over any pad
    float flat_worst_rim = 0.0f;      // steepest rise over run across any shoulder
    float flat_mean_radius = 0.0f;
    uint64_t hash = 0;
    // The same hash, split by layer, computed alongside it: grid (layers 0,
    // 1, 4-6 sampled), biomes (layer 3), entrances (7), trees (8), rocks (9). When the
    // gate fails these say WHICH layer moved; on a two-OS split that is the
    // difference between a day of guessing and one CI run.
    uint64_t layer_hash[5] = {0, 0, 0, 0, 0};
    uint64_t river_hash = 0;          // the layer-5 segment buffer, bit-exact
    int      river_count = 0;
    std::string tree_dump;            // the first trees hashed, bit-exact
    std::string tile_dump;            // per hashed tile: tree count and hash
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
            const float h = layer0_height(x, z, 1.0f, w.l0);
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

    // --- layer 0 prototype readings ---
    {
        out.raw_land = w.knee_raw_land;
        out.knee_land = w.knee_land;
        out.knee_ms = w.knee_ms;
        out.regime_size = w.l0.regime_freq > 0.0f ? 1.0f / w.l0.regime_freq : 0.0f;
        out.regime_mean = w.l0.regime_mean;
        out.regime_amp = w.l0.regime_amp;
        const SpawnPoint sp = find_spawn(w, wp, table);
        const FootReach fr = foot_reach(w, wp, sp.x, sp.z, 384);
        out.reach_share = fr.reach_share;
        out.reach_biomes = fr.biomes_reached;
        out.biomes_present = fr.biomes_present;
        out.shelf_km2 = fr.shelf_km2;
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

    // --- layer 7: entrances ---
    {
        out.entrance_trunks = w.entrance_count;
        out.entrance_segs = static_cast<int>(w.entrances.size());
        double reach = 0.0;
        for (size_t m = 0; m < w.entrance_mouths.size(); ++m) {
            const SeedWorld::EntranceMouth& em = w.entrance_mouths[m];
            reach += em.reach;
            // One metre under the lip, not a column scan: a shaft is tilted up
            // to 0.85 lateral metres per metre of drop, so it leaves a plumb
            // line within a few metres and the rock beside it is not a seal.
            if (terrain_density(w, wp, table, em.x, em.y - 1.0f, em.z) > 0.0f) {
                ++out.entrance_mouth_sealed;
            }
        }
        if (!w.entrance_mouths.empty()) {
            out.entrance_reach = static_cast<float>(reach / w.entrance_mouths.size());
        }
        // Every 16th segment, sampled at its midpoint. Exhaustive costs a
        // minute over 200 seeds; --entrances does it exhaustively.
        for (size_t m = 0; m < w.entrances.size(); m += 16) {
            const SeedWorld::EntranceSeg& e = w.entrances[m];
            const float mx = 0.5f * (e.ax + e.bx);
            const float my = 0.5f * (e.ay + e.by);
            const float mz = 0.5f * (e.az + e.bz);
            const float rr = std::sqrt(mx * mx + mz * mz) / wp.radius;
            if (my > terrain_height(w, wp, table, mx, mz, rr)) continue;
            if (terrain_density(w, wp, table, mx, my, mz) > 0.0f) ++out.entrance_pinched;
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
                if (layer0_height(x, z, rr, w.l0) <= wp.sea_level) continue;
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

    // --- blend seams ---
    // The field blends biome properties over the 4 nearest cells. Up to v8
    // a fifth could still carry weight, and where the 4th and 5th traded
    // places that weight was cut: straight-line steps in the heightfield up
    // to 63 m, and overhang walls along the same lines. The golden hash
    // samples one point per 156 m and never saw a feature a few metres wide,
    // which is how it lasted eight generator versions.
    //
    // So this walks straight lines across the disc, finds every place the
    // nearest-4 set changes, and measures the STEP there: the change across
    // 2 mm less a tenth of the change across 2 cm. A slope scales with the
    // distance and cancels; a step does not. Metres, worst over the seed.
    {
        const float R = wp.radius;
        auto key_at = [&](float x, float z) {
            const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
            const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
            int id[4] = {-1, -1, -1, -1};
            float dd[4];
            w.lookup.nearest4(w.pts, wx, wz, id, dd);
            std::sort(id, id + 4);
            return (static_cast<uint64_t>(id[0] & 0xFFFF)) | (static_cast<uint64_t>(id[1] & 0xFFFF) << 16) |
                   (static_cast<uint64_t>(id[2] & 0xFFFF) << 32) | (static_cast<uint64_t>(id[3] & 0xFFFF) << 48);
        };
        // Layer 4's base, BEFORE the river carve: the carve has its own
        // discontinuity where two segments of different widths meet (up to
        // 0.37 m, measured while building this check), which is layer 5's
        // and would otherwise be charged to the blend.
        auto hgt = [&](float x, float z) {
            return terrain_base(w, wp, table, x, z, std::sqrt(x * x + z * z) / R);
        };
        // The 3D term at a FIXED height, so anything that moves the
        // heightfield cancels and what is left is the overhang/cave blend.
        auto term = [&](float x, float y, float z) {
            const float h = terrain_height(w, wp, table, x, z, std::sqrt(x * x + z * z) / R);
            return terrain_density(w, wp, table, x, y, z) - (h - y);
        };
        // Two headings, neither axis-aligned, eight lines each.
        const float dirs[2][2] = {{0.8f, 0.6f}, {-0.6f, 0.8f}};
        float worst_h = 0.0f, worst_t = 0.0f;
        int crossings = 0;
        for (int dh = 0; dh < 2; ++dh) {
            const float dx = dirs[dh][0], dz = dirs[dh][1];
            const float px = -dz, pz = dx;      // perpendicular, for the offsets
            for (int li = 0; li < 8; ++li) {
                const float off = (static_cast<float>(li) / 7.0f * 2.0f - 1.0f) * 0.7f * R;
                const float half = std::sqrt(0.81f * R * R - off * off);
                const float ox = px * off, oz = pz * off;
                const float step = 4.0f;
                uint64_t prev = key_at(ox - dx * half, oz - dz * half);
                for (float s = -half + step; s <= half; s += step) {
                    const uint64_t cur = key_at(ox + dx * s, oz + dz * s);
                    if (cur == prev) continue;
                    float a = s - step, b = s;
                    for (int it = 0; it < 28; ++it) {
                        const float m = 0.5f * (a + b);
                        if (key_at(ox + dx * m, oz + dz * m) == prev) a = m; else b = m;
                    }
                    const float c = 0.5f * (a + b);
                    const float cx = ox + dx * c, cz = oz + dz * c;
                    const float r1 = 0.001f, r2 = 0.01f;
                    const float h1 = hgt(cx + dx * r1, cz + dz * r1) - hgt(cx - dx * r1, cz - dz * r1);
                    const float h2 = hgt(cx + dx * r2, cz + dz * r2) - hgt(cx - dx * r2, cz - dz * r2);
                    const float y0 = hgt(cx, cz);
                    const float t1 = term(cx + dx * r1, y0, cz + dz * r1) - term(cx - dx * r1, y0, cz - dz * r1);
                    const float t2 = term(cx + dx * r2, y0, cz + dz * r2) - term(cx - dx * r2, y0, cz - dz * r2);
                    const float sh = std::fabs(h1 - h2 * 0.1f) / 0.9f;
                    const float st = std::fabs(t1 - t2 * 0.1f) / 0.9f;
                    worst_h = std::max(worst_h, sh);
                    worst_t = std::max(worst_t, st);
                    ++crossings;
                    prev = cur;
                }
            }
        }
        out.seam_h = worst_h;
        out.seam_t = worst_t;
        out.seam_crossings = crossings;
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

    // --- layer 8: trees, sampled per biome ---
    // Each candidate cell is credited to the DOMINANT biome at its own
    // position. Not to the biome of the site it was sampled around: the
    // biome lookup is domain-warped by up to 1.3 km, so a cell's site in
    // world space is usually in some other biome, and crediting by site put
    // 14 trees a hectare in rift, which plants none.
    {
        out.tree_count.assign(nb, 0.0);
        out.tree_area.assign(nb, 0.0);
        const float cell = table.trees.cell;
        for (int i = 0; i < n; ++i) {
            if (biome[i] < 0) continue;
            const int ci = static_cast<int>(std::floor(w.pts[i].first / cell));
            const int cj = static_cast<int>(std::floor(w.pts[i].second / cell));
            for (int jj = cj - 4; jj < cj + 4; ++jj) {
                for (int ii = ci - 4; ii < ci + 4; ++ii) {
                    const float cx = (ii + 0.5f) * cell, cz = (jj + 0.5f) * cell;
                    int bb[4];
                    float bw[4];
                    const int m = biome_mix(w, wp, cx, cz, bb, bw);
                    if (m == 0) continue;
                    int dom = 0;
                    for (int k = 1; k < m; ++k) if (bw[k] > bw[dom]) dom = k;
                    TreeInstance t;
                    if (tree_at_cell(w, wp, table, ii, jj, t)) out.tree_count[bb[dom]] += 1.0;
                    out.tree_area[bb[dom]] += cell * cell;
                }
            }
        }
    }

    // --- layer 9: rocks, sampled per biome, as the trees are ---
    {
        for (int kind = 0; kind < 2; ++kind) {
            out.rock_count[kind].assign(nb, 0.0);
            out.rock_area[kind].assign(nb, 0.0);
            if (!table.rocks.present) continue;
            const float cell = kind == 0 ? table.rocks.boulder.cell : table.rocks.outcrop.cell;
            const int half = kind == 0 ? 4 : 1;
            for (int i = 0; i < n; ++i) {
                if (biome[i] < 0) continue;
                const int ci = static_cast<int>(std::floor(w.pts[i].first / cell));
                const int cj = static_cast<int>(std::floor(w.pts[i].second / cell));
                for (int jj = cj - half; jj < cj + half; ++jj) {
                    for (int ii = ci - half; ii < ci + half; ++ii) {
                        const float cx = (ii + 0.5f) * cell, cz = (jj + 0.5f) * cell;
                        int bb[4];
                        float bw[4];
                        const int m = biome_mix(w, wp, cx, cz, bb, bw);
                        if (m == 0) continue;
                        int dom = 0;
                        for (int k = 1; k < m; ++k) if (bw[k] > bw[dom]) dom = k;
                        RockInstance rk;
                        if (rock_at_cell(w, wp, table, kind, ii, jj, rk)) out.rock_count[kind][bb[dom]] += 1.0;
                        out.rock_area[kind][bb[dom]] += cell * cell;
                    }
                }
            }
        }
    }

    // --- determinism hash ---
    // Layers 0 and 1 on a fixed grid, plus the layer-3 assignment. Quantised,
    // because the point is to catch a changed generator, not float noise.
    {
        uint64_t h = 0xCBF29CE484222325ull;
        uint64_t lh[5] = {0xCBF29CE484222325ull, 0xCBF29CE484222325ull,
                          0xCBF29CE484222325ull, 0xCBF29CE484222325ull,
                          0xCBF29CE484222325ull};
        const int HG = 256;
        const float R = wp.radius, hstep = 2.0f * R / HG;
        for (int gj = 0; gj < HG; ++gj) {
            const float z = -R + (gj + 0.5f) * hstep;
            for (int gi = 0; gi < HG; ++gi) {
                const float x = -R + (gi + 0.5f) * hstep;
                const float rr = std::sqrt(x * x + z * z) / R;
                if (rr > 1.0f) continue;
                const float hh = layer0_height(x, z, rr, w.l0);
                const float dd = layer1_danger(x, z, rr, wp, w.s_dang);
                const float tt = terrain_height(w, wp, table, x, z, rr);
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(hh * 4096.0f)));
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(dd * 4096.0f)));
                h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(tt * 64.0f)));
                lh[0] = fnv1a(lh[0], static_cast<uint64_t>(static_cast<int64_t>(hh * 4096.0f)));
                lh[0] = fnv1a(lh[0], static_cast<uint64_t>(static_cast<int64_t>(dd * 4096.0f)));
                lh[0] = fnv1a(lh[0], static_cast<uint64_t>(static_cast<int64_t>(tt * 64.0f)));
                // Layer 6 at every 64th column, two depths each. The pin has
                // to cover the whole generator, and density is part of it now;
                // sampling every column in 3D would cost more than the rest of
                // the hash put together.
                if (((gi | gj) & 7) == 0) {
                    const float d1 = terrain_density(w, wp, table, x, tt - 20.0f, z);
                    const float d2 = terrain_density(w, wp, table, x, tt - 80.0f, z);
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(d1 * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(d2 * 64.0f)));
                    lh[0] = fnv1a(lh[0], static_cast<uint64_t>(static_cast<int64_t>(d1 * 64.0f)));
                    lh[0] = fnv1a(lh[0], static_cast<uint64_t>(static_cast<int64_t>(d2 * 64.0f)));
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            h = fnv1a(h, static_cast<uint64_t>(biome[i] + 1));
            lh[1] = fnv1a(lh[1], static_cast<uint64_t>(biome[i] + 1));
        }

        // Layer 7 goes in by its buffer rather than by sampling. A grid sample
        // lands in a shaft about never, so the sampled part of this hash is
        // blind to the layer: adding the whole of it moved none of the eight
        // golden hashes, which would have left every entrance free to drift
        // through CI unnoticed.
        //
        // Quantised at 1/64 m to match the density samples above. That is far
        // coarser than an ULP and deliberately so -- the failure mode here is
        // not a position creeping, it is the walk's terminate-on-air test
        // flipping and the shaft going somewhere else entirely, which shows up
        // in metres.
        h = fnv1a(h, static_cast<uint64_t>(w.entrance_count));
        lh[2] = fnv1a(lh[2], static_cast<uint64_t>(w.entrance_count));
        for (size_t m = 0; m < w.entrances.size(); ++m) {
            const SeedWorld::EntranceSeg& e = w.entrances[m];
            h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(e.ax * 64.0f)));
            h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(e.ay * 64.0f)));
            h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(e.az * 64.0f)));
            h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(e.ra * 64.0f)));
            lh[2] = fnv1a(lh[2], static_cast<uint64_t>(static_cast<int64_t>(e.ax * 64.0f)));
        }
        // Layer 5b by its buffer too, for the same reason: a site is tens of
        // metres across and the grid samples every 156 m. In the entrances'
        // per-layer slot (2), which is "the buffers of small features".
        h = fnv1a(h, static_cast<uint64_t>(w.flats.size()));
        lh[2] = fnv1a(lh[2], static_cast<uint64_t>(w.flats.size()));
        for (size_t m = 0; m < w.flats.size(); ++m) {
            const SeedWorld::FlatSite& f = w.flats[m];
            const uint64_t q[5] = {
                static_cast<uint64_t>(f.id),
                static_cast<uint64_t>(static_cast<int64_t>(f.x * 64.0f)),
                static_cast<uint64_t>(static_cast<int64_t>(f.z * 64.0f)),
                static_cast<uint64_t>(static_cast<int64_t>(f.radius * 64.0f)),
                static_cast<uint64_t>(static_cast<int64_t>(f.level * 64.0f))};
            for (int k = 0; k < 5; ++k) { h = fnv1a(h, q[k]); lh[2] = fnv1a(lh[2], q[k]); }
        }
        // Layer 8 by its instances, like layer 7: a tree is a few metres
        // across and the grid above samples every 156 m, so sampling would
        // be blind to it. A 64 m tile at each of the first 24 land cells'
        // sites, every tree's cell, species, variant and placement. A flipped
        // keep test shows up as a tree appearing or vanishing, which moves
        // the count and the sequence alike.
        {
            const float cell = table.trees.cell;
            std::vector<TreeInstance> ts;
            int tiles = 0;
            for (int i = 0; i < n && tiles < 24; ++i) {
                if (biome[i] < 0) continue;
                ++tiles;
                const int ci = static_cast<int>(std::floor(w.pts[i].first / cell));
                const int cj = static_cast<int>(std::floor(w.pts[i].second / cell));
                ts.clear();
                // trees_in_cells, unrolled to keep each cell's reject reason:
                // same order (rows of j, then i), so the same trees in the
                // same sequence. The reasons go in the tile report.
                int why_count[TREE_REJECT_COUNT] = {0};
                for (int jj = cj - 8; jj < cj + 8; ++jj) {
                    for (int ii = ci - 8; ii < ci + 8; ++ii) {
                        TreeInstance t1;
                        int why = 0;
                        if (tree_at_cell(w, wp, table, ii, jj, t1, &why)) ts.push_back(t1);
                        ++why_count[why];
                    }
                }
                h = fnv1a(h, static_cast<uint64_t>(ts.size()));
                lh[3] = fnv1a(lh[3], static_cast<uint64_t>(ts.size()));
                for (size_t m = 0; m < ts.size(); ++m) {
                    const TreeInstance& t = ts[m];
                    lh[3] = fnv1a(lh[3], static_cast<uint64_t>(t.id));
                    lh[3] = fnv1a(lh[3], static_cast<uint64_t>(static_cast<int64_t>(t.y * 64.0f)));
                    if (out.tree_dump.size() < 2400) {
                        // Bit patterns, not printed floats: the question is
                        // whether two builds agree to the bit, and where not.
                        uint32_t bx, by, bz, bh;
                        std::memcpy(&bx, &t.x, 4); std::memcpy(&by, &t.y, 4);
                        std::memcpy(&bz, &t.z, 4); std::memcpy(&bh, &t.height, 4);
                        char line[160];
                        std::snprintf(line, sizeof line, "      tile %d tree %08x sp %u var %u x %08x y %08x z %08x h %08x\n",
                                      tiles, t.id, unsigned(t.species), unsigned(t.variant), bx, by, bz, bh);
                        out.tree_dump += line;
                    }
                    h = fnv1a(h, static_cast<uint64_t>(t.id));
                    h = fnv1a(h, (static_cast<uint64_t>(t.species) << 16) | t.variant);
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(t.x * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(t.y * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(t.z * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(t.height * 64.0f)));
                    h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(t.yaw01 * 4096.0f)));
                }
                out.golden_trees += static_cast<int>(ts.size());
                {
                    uint64_t th = 0xCBF29CE484222325ull;
                    for (size_t m = 0; m < ts.size(); ++m) {
                        uint32_t bx, by, bz, bh;
                        std::memcpy(&bx, &ts[m].x, 4); std::memcpy(&by, &ts[m].y, 4);
                        std::memcpy(&bz, &ts[m].z, 4); std::memcpy(&bh, &ts[m].height, 4);
                        th = fnv1a(th, ts[m].id);
                        th = fnv1a(th, (uint64_t(bx) << 32) | by);
                        th = fnv1a(th, (uint64_t(bz) << 32) | bh);
                        th = fnv1a(th, (uint64_t(ts[m].species) << 16) | ts[m].variant);
                    }
                    char line[256];
                    int at = std::snprintf(line, sizeof line, "      tile %2d at cell (%d, %d): %3zu trees, %016llx, by outcome",
                                           tiles, ci, cj, ts.size(), static_cast<unsigned long long>(th));
                    for (int r = 0; r < TREE_REJECT_COUNT && at < 240; ++r)
                        at += std::snprintf(line + at, sizeof line - at, " %d", why_count[r]);
                    out.tile_dump += line;
                    out.tile_dump += "\n";
                }
            }
        }
        // Layer 9 by its instances, as layer 8: at the same first 24 land
        // sites, boulders over a 64 m tile and outcrops over 3 x 3 of their
        // cells. Each pinned rock's ground must also be air over rock: the
        // bisection put it there, and a sign error would bury or float it.
        if (table.rocks.present) {
            std::vector<RockInstance> rs;
            int tiles = 0;
            for (int i = 0; i < n && tiles < 24; ++i) {
                if (biome[i] < 0) continue;
                ++tiles;
                for (int kind = 0; kind < 2; ++kind) {
                    const RockClass& rc = kind == 0 ? table.rocks.boulder : table.rocks.outcrop;
                    const int half = kind == 0 ? 4 : 1;
                    const int ci = static_cast<int>(std::floor(w.pts[i].first / rc.cell));
                    const int cj = static_cast<int>(std::floor(w.pts[i].second / rc.cell));
                    rs.clear();
                    rocks_in_cells(w, wp, table, kind, ci - half, cj - half, ci + half, cj + half, rs);
                    h = fnv1a(h, static_cast<uint64_t>(rs.size()));
                    lh[4] = fnv1a(lh[4], static_cast<uint64_t>(rs.size()));
                    for (size_t m = 0; m < rs.size(); ++m) {
                        const RockInstance& rk = rs[m];
                        const uint64_t q[6] = {
                            static_cast<uint64_t>(rk.id),
                            (static_cast<uint64_t>(rk.variant) << 16) | rk.biome,
                            static_cast<uint64_t>(static_cast<int64_t>(rk.x * 64.0f)),
                            static_cast<uint64_t>(static_cast<int64_t>(rk.y * 64.0f)),
                            static_cast<uint64_t>(static_cast<int64_t>(rk.z * 64.0f)),
                            static_cast<uint64_t>(static_cast<int64_t>(rk.size * 64.0f))};
                        for (int k = 0; k < 6; ++k) { h = fnv1a(h, q[k]); lh[4] = fnv1a(lh[4], q[k]); }
                        h = fnv1a(h, static_cast<uint64_t>(static_cast<int64_t>(rk.yaw01 * 4096.0f)));
                        const float gy = rk.ground;
                        if (terrain_density(w, wp, table, rk.x, gy + 0.05f, rk.z) > 0.0f ||
                            terrain_density(w, wp, table, rk.x, gy - 0.05f, rk.z) <= 0.0f) ++out.rocks_unseated;
                    }
                    out.golden_rocks += static_cast<int>(rs.size());
                }
            }
        }
        out.hash = h;
        for (int k = 0; k < 5; ++k) out.layer_hash[k] = lh[k];
        // Rivers by their buffer, NOT in the pinned hash: a diagnostic. The
        // grid samples every 78 m and can miss a channel 38 m wide.
        {
            uint64_t rh = 0xCBF29CE484222325ull;
            for (size_t m = 0; m < w.rivers.size(); ++m) {
                const SeedWorld::RiverSeg& r = w.rivers[m];
                uint32_t b[6];
                std::memcpy(&b[0], &r.ax, 4); std::memcpy(&b[1], &r.az, 4);
                std::memcpy(&b[2], &r.bx, 4); std::memcpy(&b[3], &r.bz, 4);
                std::memcpy(&b[4], &r.width, 4); std::memcpy(&b[5], &r.depth, 4);
                for (int k = 0; k < 6; ++k) rh = fnv1a(rh, b[k]);
            }
            out.river_hash = rh;
            out.river_count = static_cast<int>(w.rivers.size());
        }
    }

    // Layer 5b: every site's pad must be level, and its shoulder a slope a
    // player can walk. Sixteen directions, sampled through terrain_height --
    // the function everything else reads -- not the stored level.
    {
        out.flat_by_biome.assign(table.size(), 0);
        out.flat_count = static_cast<int>(w.flats.size());
        const float c1 = 0.92387953f, s1 = 0.38268343f, dg = 0.70710678f;
        const float dx16[16] = {1.0f, c1, dg, s1, 0.0f, -s1, -dg, -c1, -1.0f, -c1, -dg, -s1, 0.0f, s1, dg, c1};
        const float dz16[16] = {0.0f, s1, dg, c1, 1.0f, c1, dg, s1, 0.0f, -s1, -dg, -c1, -1.0f, -c1, -dg, -s1};
        const float R = wp.radius;
        auto hat = [&](float x, float z) {
            return terrain_height(w, wp, table, x, z, std::sqrt(x * x + z * z) / R);
        };
        double rsum = 0.0;
        for (size_t m = 0; m < w.flats.size(); ++m) {
            const SeedWorld::FlatSite& f = w.flats[m];
            if (f.biome >= 0 && static_cast<size_t>(f.biome) < table.size()) ++out.flat_by_biome[static_cast<size_t>(f.biome)];
            rsum += f.radius;
            const float hc = hat(f.x, f.z);
            float lo = hc, hi = hc;
            for (int a = 0; a < 16; ++a) {
                for (int ring = 1; ring <= 2; ++ring) {
                    const float rr = f.radius * (ring == 1 ? 0.5f : 0.98f);
                    const float h = hat(f.x + dx16[a] * rr, f.z + dz16[a] * rr);
                    lo = std::min(lo, h);
                    hi = std::max(hi, h);
                }
                // The shoulder in eight steps, plus one past it.
                float prev = hat(f.x + dx16[a] * f.radius, f.z + dz16[a] * f.radius);
                const float step = f.shoulder / 8.0f;
                for (int k = 1; k <= 9; ++k) {
                    const float rr = f.radius + step * static_cast<float>(k);
                    const float h = hat(f.x + dx16[a] * rr, f.z + dz16[a] * rr);
                    const float g = std::fabs(h - prev) / step;
                    if (g > out.flat_worst_rim) out.flat_worst_rim = g;
                    prev = h;
                }
            }
            if (hi - lo > out.flat_worst_pad) out.flat_worst_pad = hi - lo;
        }
        out.flat_mean_radius = w.flats.empty() ? 0.0f : static_cast<float>(rsum / w.flats.size());
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
    int   seam_failures = 0;     // a blend step over 5 cm anywhere measured
    float max_seam_h = 0.0f, max_seam_t = 0.0f;
    long  seam_crossings = 0;
    float mean_cave_fraction = 0.0f, mean_overhang_cols = 0.0f;
    int river_failures = 0;      // no rivers, or one that climbs or never reaches the sea
    float mean_river_segments = 0.0f;
    int   entrance_failures = 0;
    int   entrance_mouths_sealed = 0, entrance_segs_pinched = 0;
    int   min_entrance_trunks = 1 << 30;
    float mean_entrance_trunks = 0.0f, mean_entrance_reach = 0.0f;
    std::vector<double> tree_per_ha;  // per biome, mean over seeds that have it
    std::vector<int>    tree_seeds;   // seeds each biome appeared in
    int   tree_failures = 0;          // set after the loop; see the trees check
    float tree_spearman = 0.0f;
    int   tree_bare_seeds = 0;        // seeds where a treed biome grew nothing
    float mean_golden_trees = 0.0f;
    std::vector<double> rock_per_ha[2];  // per biome and class, mean over seeds that have it
    std::vector<int>    rock_seeds[2];
    int   rock_failures = 0;
    int   rocks_unseated = 0;
    float mean_golden_rocks = 0.0f;
    std::vector<double> flat_per_seed;  // per biome, mean sites a seed
    double mean_flats = 0.0;
    float  flat_worst_pad = 0.0f, flat_worst_rim = 0.0f, flat_mean_radius = 0.0f;
    int    flat_failures = 0;
    // Layer 0 prototype readings, all measured and none asserted.
    float mean_raw_land = 0.0f, max_raw_land = 0.0f, mean_knee_land = 0.0f, max_knee_land = 0.0f;
    float mean_knee_ms = 0.0f, max_knee_ms = 0.0f;
    float mean_reach = 0.0f, min_reach = 2.0f, max_reach = 0.0f;
    float mean_shelf_km2 = 0.0f, max_shelf_km2 = 0.0f;
    int   reach_biome_hist[16] = {0};   // seeds by biomes reachable on foot
    int   reach_all_biomes = 0;         // seeds where every present biome is reachable
    float min_rsize = 1e30f, max_rsize = 0.0f, min_rmean = 1e30f, max_rmean = -1e30f;
    float min_ramp = 1e30f, max_ramp = -1e30f;
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
        // v18: a 29.4% mean, seeds 25.5-33.3% over 200 (32% and 26% before).
        if (r.land_fraction < kLandSeedMin || r.land_fraction > 0.45f) ++sm.land_failures;
        if (r.adj_violations > 0) ++sm.adjacency_failures;
        if (!r.taper_ok) ++sm.taper_failures;
        // A DEGENERACY guard, not a connectivity requirement: the spawn
        // landmass has to hold enough land to start a game on. An archipelago
        // passes it by design, which is the point -- the design doc wants
        // island clusters (layer 3) and the old >= 75% foot-connectivity
        // assertion quietly forbade them. README section 8 has the history.
        // v18: a READING, not a check. With no centre floor a start can be a
        // bare islet (the layer 0 proposal: how lucky a start is is never
        // fenced). Counted and printed; find_spawn existing is still asserted
        // by the spawn check.
        if (r.home_cells < kHomeMinCells) ++sm.home_failures;
        if (!r.boat_all_biomes) ++sm.boat_failures;
        if (r.spawn_cell < 0 || !r.spawn_coastal || !r.spawn_calmest) ++sm.spawn_failures;
        if (r.slope_gentle < 0.60f) ++sm.slope_failures;
        if (r.lod_worst > 1.0f) ++sm.lod_failures;
        sm.max_lod_worst = std::max(sm.max_lod_worst, r.lod_worst);
        // 5 cm: a tenth of a LOD 0 voxel, and two orders of magnitude above
        // what the continuous blend measures (worst 7 mm, which is slope
        // leaking through the 2 mm arm on steep ground, not a step).
        if (r.seam_h > 0.05f || r.seam_t > 0.05f) ++sm.seam_failures;
        sm.max_seam_h = std::max(sm.max_seam_h, r.seam_h);
        sm.max_seam_t = std::max(sm.max_seam_t, r.seam_t);
        sm.seam_crossings += r.seam_crossings;
        sm.mean_cave_fraction += r.cave_fraction;
        sm.mean_overhang_cols += r.overhang_cols;
        // 8 is a degeneracy guard, not a target: the measured spread over
        // 200 seeds is 12 to 24, and the low end is seeds with little land
        // rather than a broken layer.
        // With caves off (every cave_density 0) the check inverts: a single
        // shaft would be a hole in the ground that leads nowhere.
        if (caves_enabled(table)) {
            if (r.entrance_trunks < 8 || r.entrance_mouth_sealed > 0 ||
                r.entrance_pinched > 0) ++sm.entrance_failures;
        } else if (r.entrance_trunks != 0 || r.entrance_segs != 0) {
            ++sm.entrance_failures;
        }
        sm.entrance_mouths_sealed += r.entrance_mouth_sealed;
        sm.entrance_segs_pinched += r.entrance_pinched;
        sm.min_entrance_trunks = std::min(sm.min_entrance_trunks, r.entrance_trunks);
        sm.mean_entrance_trunks += r.entrance_trunks;
        sm.mean_entrance_reach += r.entrance_reach;
        if (r.river_segments == 0 || r.river_ascending > 0 ||
            r.river_paths_to_ocean != r.river_paths_total) ++sm.river_failures;
        sm.mean_river_segments += r.river_segments;
        if (sm.tree_per_ha.empty()) {
            sm.tree_per_ha.assign(table.size(), 0.0);
            sm.tree_seeds.assign(table.size(), 0);
        }
        for (size_t b = 0; b < table.size() && b < r.tree_area.size(); ++b) {
            if (r.tree_area[b] <= 0.0) continue;
            const double per_ha = r.tree_count[b] / r.tree_area[b] * 10000.0;
            sm.tree_per_ha[b] += per_ha;
            ++sm.tree_seeds[b];
            // A degeneracy guard: a biome that is configured with trees and
            // sampled over a real area, growing none at all.
            if (table[b].tree_density > 0.0f && table[b].tree_cover > 0.0f &&
                r.tree_area[b] >= 20000.0 && r.tree_count[b] == 0.0) ++sm.tree_bare_seeds;
        }
        sm.mean_golden_trees += r.golden_trees;
        for (int kind = 0; kind < 2; ++kind) {
            if (sm.rock_per_ha[kind].empty()) {
                sm.rock_per_ha[kind].assign(table.size(), 0.0);
                sm.rock_seeds[kind].assign(table.size(), 0);
            }
            for (size_t b = 0; b < table.size() && b < r.rock_area[kind].size(); ++b) {
                if (r.rock_area[kind][b] <= 0.0) continue;
                sm.rock_per_ha[kind][b] += r.rock_count[kind][b] / r.rock_area[kind][b] * 10000.0;
                ++sm.rock_seeds[kind][b];
            }
        }
        sm.mean_golden_rocks += r.golden_rocks;
        sm.rocks_unseated += r.rocks_unseated;
        if (sm.flat_per_seed.size() < table.size()) sm.flat_per_seed.assign(table.size(), 0.0);
        for (size_t b = 0; b < r.flat_by_biome.size() && b < table.size(); ++b) sm.flat_per_seed[b] += r.flat_by_biome[b];
        sm.mean_flats += r.flat_count;
        sm.flat_mean_radius += r.flat_mean_radius;
        sm.flat_worst_pad = std::max(sm.flat_worst_pad, r.flat_worst_pad);
        sm.flat_worst_rim = std::max(sm.flat_worst_rim, r.flat_worst_rim);
        sm.min_home_share = std::min(sm.min_home_share, r.home_share);
        sm.mean_raw_land += r.raw_land;
        sm.max_raw_land = std::max(sm.max_raw_land, r.raw_land);
        sm.mean_knee_land += r.knee_land;
        sm.max_knee_land = std::max(sm.max_knee_land, r.knee_land);
        sm.mean_knee_ms += r.knee_ms;
        sm.max_knee_ms = std::max(sm.max_knee_ms, r.knee_ms);
        sm.mean_reach += r.reach_share;
        sm.min_reach = std::min(sm.min_reach, r.reach_share);
        sm.max_reach = std::max(sm.max_reach, r.reach_share);
        sm.mean_shelf_km2 += r.shelf_km2;
        sm.max_shelf_km2 = std::max(sm.max_shelf_km2, r.shelf_km2);
        sm.reach_biome_hist[std::min(15, std::max(0, r.reach_biomes))] += 1;
        if (r.reach_biomes >= r.biomes_present) ++sm.reach_all_biomes;
        sm.min_rsize = std::min(sm.min_rsize, r.regime_size);
        sm.max_rsize = std::max(sm.max_rsize, r.regime_size);
        sm.min_rmean = std::min(sm.min_rmean, r.regime_mean);
        sm.max_rmean = std::max(sm.max_rmean, r.regime_mean);
        sm.min_ramp = std::min(sm.min_ramp, r.regime_amp);
        sm.max_ramp = std::max(sm.max_ramp, r.regime_amp);
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
    sm.mean_golden_trees *= inv;
    sm.mean_golden_rocks *= inv;
    // Flat sites: every pad level to 2 cm through terrain_height, every
    // shoulder walkable (rise over run under 0.6, ~31 degrees), every biome
    // configured with sites gets some, and none configured without gets more
    // than a trace from a neighbour's blend.
    sm.mean_flats *= inv;
    sm.flat_mean_radius = static_cast<float>(sm.flat_mean_radius * inv);
    for (size_t b = 0; b < sm.flat_per_seed.size(); ++b) sm.flat_per_seed[b] *= inv;
    if (table.flats.present) {
        int fails = 0;
        if (sm.flat_worst_pad > 0.02f) ++fails;
        if (sm.flat_worst_rim > 0.6f) ++fails;
        double most = 0.0;
        for (size_t b = 0; b < sm.flat_per_seed.size(); ++b) most = std::max(most, sm.flat_per_seed[b]);
        for (size_t b = 0; b < sm.flat_per_seed.size(); ++b) {
            if (table[b].flat_chance > 0.0f && sm.flat_per_seed[b] <= 0.0) ++fails;
            if (table[b].flat_chance <= 0.0f && sm.flat_per_seed[b] > 0.05 * most) ++fails;
        }
        sm.flat_failures = fails;
    }
    // Rocks: every biome configured with a class grows some of it (mean over
    // the seeds it appeared in), none configured without grows more than a
    // trace from a neighbour's blend, and every pinned rock is seated.
    if (table.rocks.present) {
        int fails = 0;
        for (int kind = 0; kind < 2; ++kind) {
            double max_ha = 0.0;
            for (size_t b = 0; b < sm.rock_per_ha[kind].size(); ++b) {
                if (sm.rock_seeds[kind][b] > 0) sm.rock_per_ha[kind][b] /= sm.rock_seeds[kind][b];
                if (sm.rock_per_ha[kind][b] > max_ha) max_ha = sm.rock_per_ha[kind][b];
            }
            for (size_t b = 0; b < sm.rock_per_ha[kind].size(); ++b) {
                if (sm.rock_seeds[kind][b] == 0) continue;
                const float d = kind == 0 ? table[b].rock_boulders : table[b].rock_outcrops;
                const double cfg = d * table[b].rock_cover;
                if (cfg > 0.0 && sm.rock_per_ha[kind][b] <= 0.0) ++fails;
                if (cfg <= 0.0 && sm.rock_per_ha[kind][b] > 0.05 * max_ha) ++fails;
            }
        }
        if (sm.rocks_unseated > 0) ++fails;
        sm.rock_failures = fails;
    }
    // Trees: what each biome actually grew, against what it was configured to
    // grow (density x cover, the expected share of candidate cells). The
    // configured ranking must survive into the world: altitude, slope, shore
    // and river exclusions thin every biome differently, and a biome whose
    // ground is mostly too steep would invert the order the table asks for.
    {
        double max_ha = 0.0;
        for (size_t b = 0; b < sm.tree_per_ha.size(); ++b) {
            if (sm.tree_seeds[b] > 0) sm.tree_per_ha[b] /= sm.tree_seeds[b];
            if (sm.tree_per_ha[b] > max_ha) max_ha = sm.tree_per_ha[b];
        }
        int fails = 0;
        std::vector<double> want, have;
        for (size_t b = 0; b < sm.tree_per_ha.size(); ++b) {
            const double cfg = table[b].tree_density * table[b].tree_cover;
            if (cfg > 0.0 && sm.tree_per_ha[b] <= 0.0) ++fails;           // treed, grew none
            if (cfg <= 0.0 && sm.tree_per_ha[b] > 0.05 * max_ha) ++fails; // bare, but wooded
            want.push_back(cfg);
            have.push_back(sm.tree_per_ha[b]);
        }
        // Spearman by ranks, ties averaged.
        auto ranks = [](const std::vector<double>& v) {
            std::vector<size_t> o(v.size());
            for (size_t i = 0; i < o.size(); ++i) o[i] = i;
            std::sort(o.begin(), o.end(), [&](size_t a, size_t b) { return v[a] < v[b]; });
            std::vector<double> r(v.size(), 0.0);
            for (size_t i = 0; i < o.size();) {
                size_t j = i;
                while (j + 1 < o.size() && v[o[j + 1]] == v[o[i]]) ++j;
                for (size_t k = i; k <= j; ++k) r[o[k]] = 0.5 * (double(i) + double(j));
                i = j + 1;
            }
            return r;
        };
        if (want.size() >= 3) {
            const std::vector<double> ra = ranks(want), rb = ranks(have);
            double ma = 0, mb = 0;
            for (size_t i = 0; i < ra.size(); ++i) { ma += ra[i]; mb += rb[i]; }
            ma /= ra.size(); mb /= rb.size();
            double sab = 0, saa = 0, sbb = 0;
            for (size_t i = 0; i < ra.size(); ++i) {
                sab += (ra[i] - ma) * (rb[i] - mb);
                saa += (ra[i] - ma) * (ra[i] - ma);
                sbb += (rb[i] - mb) * (rb[i] - mb);
            }
            sm.tree_spearman = (saa > 0 && sbb > 0) ? float(sab / std::sqrt(saa * sbb)) : 0.0f;
        }
        if (sm.tree_spearman < 0.80f) ++fails;
        // At most 2% of seeds may have a treed biome that grew nothing: a
        // biome sampled at one or two cells high on a mountain can be bare
        // for real.
        if (sm.tree_bare_seeds > nseeds / 50) ++fails;
        sm.tree_failures = fails;
    }
    sm.mean_entrance_trunks *= inv; sm.mean_entrance_reach *= inv;
    sm.mean_cave_fraction *= inv; sm.mean_overhang_cols *= inv;
    sm.mean_home_share *= inv; sm.mean_landmasses *= inv;
    sm.mean_raw_land *= inv; sm.mean_knee_land *= inv; sm.mean_knee_ms *= inv;
    sm.mean_reach *= inv; sm.mean_shelf_km2 *= inv;
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

// The eight seeds, hashed at a canonical resolution. The cell areas and the
// land mask are derived from the raster, so hashing at whatever --grid a run
// happened to use would pin a HARNESS setting rather than the generator, and
// the gate would fire on nothing.
static WorldParams golden_params(const WorldParams& wp) {
    WorldParams gw = wp;
    gw.grid = 1024;
    return gw;
}

static const int kGoldenSeeds = 8;

// One thread per seed. The gate sits on every build that touches worldgen/,
// and a check people are tempted to switch off is not a gate -- 4.2 s serial
// against 1.1 s here. Threading cannot move a hash: each seed is an
// independent build_world with its own per-feature seeds (invariant 4), and
// if that were ever untrue this is the one check that would say so.
// Per-layer hashes and seed 0's first trees, from the last golden_hashes.
static uint64_t g_layer_hash[kGoldenSeeds][5];
static std::string g_seed0_trees;
static uint64_t g_river_hash[kGoldenSeeds];
static int g_river_count[kGoldenSeeds];

static void golden_hashes(const WorldParams& wp, const ArchetypeTable& table,
                          const AssignParams& ap,
                          std::vector<std::pair<uint64_t, uint64_t> >& out) {
    const WorldParams gw = golden_params(wp);
    std::vector<uint64_t> hashes(kGoldenSeeds, 0);

    unsigned nthreads = std::thread::hardware_concurrency();
    if (nthreads == 0) nthreads = 4;
    nthreads = std::min<unsigned>(nthreads, static_cast<unsigned>(kGoldenSeeds));

    std::vector<std::thread> pool;
    for (unsigned th = 0; th < nthreads; ++th) {
        pool.push_back(std::thread([&, th]() {
            for (int i = static_cast<int>(th); i < kGoldenSeeds;
                 i += static_cast<int>(nthreads)) {
                const SeedResult r = eval_seed(static_cast<uint64_t>(i), gw, table, ap);
                hashes[i] = r.hash;
                for (int k = 0; k < 5; ++k) g_layer_hash[i][k] = r.layer_hash[k];
                g_river_hash[i] = r.river_hash;
                g_river_count[i] = r.river_count;
                if (i == 0) g_seed0_trees = r.tree_dump + "    seed 0's tiles:\n" + r.tile_dump;
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();

    out.clear();
    for (int i = 0; i < kGoldenSeeds; ++i) {
        out.push_back(std::make_pair(static_cast<uint64_t>(i), hashes[i]));
    }
}

// How many of the pinned hashes `now` disagrees with. A different COUNT of
// seeds is a move too: it means the pinned set is not the set being checked.
static int golden_moved_count(const Golden& g,
                              const std::vector<std::pair<uint64_t, uint64_t> >& now) {
    if (g.seeds.size() != now.size()) return static_cast<int>(now.size());
    int moved = 0;
    for (size_t i = 0; i < g.seeds.size(); ++i) {
        if (g.seeds[i].first != now[i].first || g.seeds[i].second != now[i].second) ++moved;
    }
    return moved;
}

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


// --- the gate --------------------------------------------------------------
//
// M2: this is what the BUILD runs, not what someone remembers to run. Three
// states, and only the first is a pass:
//
//   version matches the pin, hashes match  -> the world is what it claims
//   version matches the pin, hashes moved  -> a silent world change
//   version differs from the pin           -> the pin is stale, bless it
//
// The second is the one the milestone is about. It cannot be cleared by
// re-pinning, because --bless refuses while kGeneratorVersion is unchanged:
// the only way out is to revert the change or say, in worldgen's own header,
// that the world is allowed to be different.
static int run_golden_gate(const WorldParams& wp, const ArchetypeTable& table,
                           const AssignParams& ap) {
    Golden g;
    const bool had = load_golden(g);
    std::vector<std::pair<uint64_t, uint64_t> > now;
    golden_hashes(wp, table, ap, now);

    if (!had) {
        std::printf("golden gate: no %s.\n", kGoldenPath);
        std::printf("  This world has never been pinned. Run:  validate --bless\n");
        return 1;
    }

    const int moved = golden_moved_count(g, now);

    if (g.generator_version != kGeneratorVersion) {
        std::printf("golden gate: worldgen/generator_version.h says %d, but %s\n",
                    kGeneratorVersion, kGoldenPath);
        std::printf("  holds hashes pinned at %d.\n", g.generator_version);
        std::printf("  %d of %d seeds %s.\n", moved, static_cast<int>(now.size()),
                    moved ? "differ" : "still agree");
        std::printf("  Re-pin the new version:  validate --bless\n");
        return 1;
    }

    if (moved > 0) {
        std::printf("golden gate: THE WORLD MOVED at generator_version %d.\n",
                    kGeneratorVersion);
        std::printf("  %d of %d golden seeds hash differently than %s pins.\n",
                    moved, static_cast<int>(now.size()), kGoldenPath);
        for (size_t i = 0; i < now.size() && i < g.seeds.size(); ++i) {
            if (g.seeds[i].second == now[i].second) continue;
            std::printf("    seed %llu: pinned 0x%016llx, now 0x%016llx\n",
                        static_cast<unsigned long long>(now[i].first),
                        static_cast<unsigned long long>(g.seeds[i].second),
                        static_cast<unsigned long long>(now[i].second));
        }
        std::printf("\n  By layer (grid / biomes / entrances / trees / rocks), to compare with\n"
                    "  a build that agrees with the pin -- the layer that differs is\n"
                    "  the one that moved:\n");
        for (size_t i = 0; i < now.size(); ++i) {
            std::printf("    seed %llu: %016llx %016llx %016llx %016llx %016llx\n",
                        static_cast<unsigned long long>(now[i].first),
                        static_cast<unsigned long long>(g_layer_hash[i][0]),
                        static_cast<unsigned long long>(g_layer_hash[i][1]),
                        static_cast<unsigned long long>(g_layer_hash[i][2]),
                        static_cast<unsigned long long>(g_layer_hash[i][3]),
                        static_cast<unsigned long long>(g_layer_hash[i][4]));
        }
        for (size_t i = 0; i < now.size(); ++i)
            std::printf("    seed %llu rivers: %d segments, %016llx\n", static_cast<unsigned long long>(now[i].first),
                        g_river_count[i], static_cast<unsigned long long>(g_river_hash[i]));
        std::printf("    seed 0's first trees, bit-exact:\n%s", g_seed0_trees.c_str());
        std::printf("\n  Saves store edits as absolute density against the base\n");
        std::printf("  terrain, so a world that changes under them corrupts them.\n");
        std::printf("  Either revert the change, or bump kGeneratorVersion in\n");
        std::printf("  worldgen/generator_version.h and then run:  validate --bless\n");
        return 1;
    }

    std::printf("golden gate: %d seeds pinned at generator_version %d, all matching\n",
                static_cast<int>(now.size()), kGeneratorVersion);
    if (std::getenv("VALIDATE_LAYER_HASHES")) {
        for (size_t i = 0; i < now.size(); ++i) {
            std::printf("    seed %llu: %016llx %016llx %016llx %016llx %016llx\n",
                        static_cast<unsigned long long>(now[i].first),
                        static_cast<unsigned long long>(g_layer_hash[i][0]),
                        static_cast<unsigned long long>(g_layer_hash[i][1]),
                        static_cast<unsigned long long>(g_layer_hash[i][2]),
                        static_cast<unsigned long long>(g_layer_hash[i][3]),
                        static_cast<unsigned long long>(g_layer_hash[i][4]));
        }
        for (size_t i = 0; i < now.size(); ++i)
            std::printf("    seed %llu rivers: %d segments, %016llx\n", static_cast<unsigned long long>(now[i].first),
                        g_river_count[i], static_cast<unsigned long long>(g_river_hash[i]));
        std::printf("    seed 0's first trees, bit-exact:\n%s", g_seed0_trees.c_str());
    }
    return 0;
}

// Pin the current world. Refuses the one case that would make the gate
// meaningless: writing moved hashes back at a version that has not changed,
// which is "make the red build green" with extra steps.
static int run_bless(const WorldParams& wp, const ArchetypeTable& table,
                     const AssignParams& ap) {
    Golden g;
    const bool had = load_golden(g);
    std::vector<std::pair<uint64_t, uint64_t> > now;
    golden_hashes(wp, table, ap, now);

    if (had && g.generator_version == kGeneratorVersion &&
        golden_moved_count(g, now) > 0) {
        std::printf("refusing to bless: generator_version is still %d.\n",
                    kGeneratorVersion);
        std::printf("  These hashes are a different world from the ones already\n");
        std::printf("  pinned at that version, and pinning both under one number\n");
        std::printf("  is what the number exists to prevent.\n");
        std::printf("  Bump kGeneratorVersion in worldgen/generator_version.h\n");
        std::printf("  first -- or revert, if the change was not meant to move\n");
        std::printf("  the world.\n");
        return 1;
    }

    g.generator_version = kGeneratorVersion;
    g.seeds = now;
    if (!write_golden(g)) {
        std::printf("FAILED to write %s\n", kGoldenPath);
        return 1;
    }
    std::printf("blessed %d golden seeds at generator_version %d -> %s\n",
                static_cast<int>(g.seeds.size()), g.generator_version, kGoldenPath);
    return 0;
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
    // M2: the gate on its own, which is what the build runs. Eight seeds,
    // no 200-seed sweep, no contact sheet -- it has to be cheap enough to
    // sit on every build that touches worldgen/.
    bool golden_only = false;
    bool relief = false;
    bool closeup = false;
    bool components = false;
    bool cave_access = false;
    bool entrances_probe = false;
    bool tree_quantiles = false;
    bool tree_reasons = false;
    bool spawns = false;
    bool flatness = false;
    bool tree_sites = false;
    uint64_t tree_sites_seed = 0;
    bool entrance_section = false;
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
        else if (!std::strcmp(argv[i], "--golden"))                 golden_only = true;
        else if (!std::strcmp(argv[i], "--relief"))                 relief = true;
        else if (!std::strcmp(argv[i], "--components"))              components = true;
        else if (!std::strcmp(argv[i], "--cave-access"))             cave_access = true;
        else if (!std::strcmp(argv[i], "--entrances"))               entrances_probe = true;
        else if (!std::strcmp(argv[i], "--tree-quantiles"))          tree_quantiles = true;
        else if (!std::strcmp(argv[i], "--tree-reasons"))            tree_reasons = true;
        else if (!std::strcmp(argv[i], "--spawns"))                  spawns = true;
        else if (!std::strcmp(argv[i], "--flatness"))                flatness = true;
        else if (!std::strcmp(argv[i], "--tree-sites") && i + 1 < argc) {
            tree_sites = true;
            tree_sites_seed = std::strtoull(argv[++i], nullptr, 10);
        }
        else if (!std::strcmp(argv[i], "--entrance-section") && i + 1 < argc) {
            entrance_section = true;
            section_seed = std::strtoull(argv[++i], nullptr, 10);
        }
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
        // Layer 0 prototype (docs/worldgen-design.md, "Layer 0, proposed").
        // Every default is today's world; any of these moves the golden
        // hashes, which is the gate working, not a reason to --bless.
        else if (!std::strcmp(argv[i], "--centre-floor") && i + 1 < argc) wp.centre_floor = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--fine-feat") && i + 1 < argc)    wp.fine_freq = 1.0f / static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--fine-octaves") && i + 1 < argc) wp.fine_octaves = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--fine-weight") && i + 1 < argc)  wp.fine_weight = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--fine-regime") && i + 1 < argc)  wp.fine_regime = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--regime-feat") && i + 1 < argc)  wp.regime_freq = 1.0f / static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--regime-octaves") && i + 1 < argc) wp.regime_octaves = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--regime-mean") && i + 1 < argc)  wp.regime_mean = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--regime-amp") && i + 1 < argc)   wp.regime_amp = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--regime-draw"))                  wp.regime_draw = true;
        else if (!std::strcmp(argv[i], "--regime-feat-range") && i + 2 < argc) {
            wp.regime_size_lo = static_cast<float>(std::atof(argv[++i]));
            wp.regime_size_hi = static_cast<float>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i], "--regime-mean-range") && i + 2 < argc) {
            wp.regime_mean_lo = static_cast<float>(std::atof(argv[++i]));
            wp.regime_mean_hi = static_cast<float>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i], "--regime-amp-range") && i + 2 < argc) {
            wp.regime_amp_lo = static_cast<float>(std::atof(argv[++i]));
            wp.regime_amp_hi = static_cast<float>(std::atof(argv[++i]));
        }
        else if (!std::strcmp(argv[i], "--shelf-depth") && i + 1 < argc)  wp.shelf_depth = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--shelf-width") && i + 1 < argc)  wp.shelf_width = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--shelf-regime") && i + 1 < argc) wp.shelf_regime = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--knee"))                         wp.knee = true;
        else if (!std::strcmp(argv[i], "--knee-k") && i + 1 < argc)       wp.knee_k = static_cast<float>(std::atof(argv[++i]));
        else if (!std::strcmp(argv[i], "--knee-c") && i + 1 < argc)       wp.knee_c = static_cast<float>(std::atof(argv[++i]));
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

    // M2: the gate and the pin, both on their own. These return straight out
    // -- the 200-seed sweep below asks a different question (is this world any
    // good) from the one the build asks (is this the world that is pinned).
    if (golden_only) return run_golden_gate(wp, table, ap);
    if (bless)       return run_bless(wp, table, ap);

    if (entrance_section) {
        // Slice vertically through the first mouth of this seed. Narrow by
        // default: a shaft throat is 2.4 m, so the contact sheet's 87 m per
        // pixel would not show it at all.
        const SeedWorld sw = build_world(section_seed, wp, table, ap);
        if (sw.entrance_mouths.empty()) {
            std::printf("seed %llu has no entrances\n",
                        static_cast<unsigned long long>(section_seed));
            return 1;
        }
        const SeedWorld::EntranceMouth& em = sw.entrance_mouths[0];
        const int W = 1400, H = 700;
        std::printf("entrance section, seed %llu, mouth at (%.0f, %.0f) surface %.0f m, "
                    "reaching cave air %.0f m down, %.0f m wide\n",
                    static_cast<unsigned long long>(section_seed), em.x, em.z, em.y,
                    em.reach, section_width);
        std::vector<unsigned char> img(static_cast<size_t>(W) * H * 3, 0);
        const float ylo = em.y - em.reach - 70.0f, yhi = em.y + 30.0f;
        // Equal metres per pixel on both axes: W/H times the vertical span.
        const float xspan = (yhi - ylo) * static_cast<float>(W) / static_cast<float>(H);
        render_shaft_profile(sw, wp, table, 0, ylo, yhi, xspan, img, W, H);
        if (!write_bmp(sheet_path, W, H, img)) {
            std::printf("FAILED to write %s\n", sheet_path);
            return 1;
        }
        std::printf("wrote %s\n", sheet_path);
        return 0;
    }

    if (entrances_probe) {
        // Walk each stored trunk from its mouth to its tip and ask the carved
        // density whether the tube is open the whole way. build_entrances
        // guarantees the walk ENDED in air; what this checks is that the carve
        // then actually opened what the walk traced, which is a different
        // claim and the one a player experiences.
        std::printf("entrances over %d seeds\n", nseeds);
        int total = 0, pinched = 0, mouth_sealed = 0, mouth_shallow = 0;
        double sum_reach = 0.0;
        int min_count = 1 << 30, max_count = 0;
        double sum_count = 0.0, sum_depth = 0.0, sum_tries = 0.0;
        for (int sd = 0; sd < nseeds; ++sd) {
            // Same seed derivation as the gate, so the two sets of numbers
            // describe the same 200 worlds.
            const SeedWorld cw = build_world(0x5EEDull * 1000003ull + static_cast<uint64_t>(sd), wp, table, ap);
            min_count = std::min(min_count, cw.entrance_count);
            max_count = std::max(max_count, cw.entrance_count);
            sum_count += cw.entrance_count;
            sum_tries += cw.entrance_tries_used;

            // Segments are emitted trunk-first per entrance, but trunks and
            // branches are interleaved in one list, so walk every segment and
            // test its midpoint rather than trying to reconstruct the trunks.
            for (size_t m = 0; m < cw.entrances.size(); ++m) {
                const SeedWorld::EntranceSeg& e = cw.entrances[m];
                const float mx = 0.5f * (e.ax + e.bx);
                const float my = 0.5f * (e.ay + e.by);
                const float mz = 0.5f * (e.az + e.bz);
                const float rr = std::sqrt(mx * mx + mz * mz) / wp.radius;
                const float surf = terrain_height(cw, wp, table, mx, mz, rr);
                if (my > surf) continue;          // above ground; nothing to open
                ++total;
                if (terrain_density(cw, wp, table, mx, my, mz) > 0.0f) ++pinched;
                sum_depth = sum_depth + (surf - my);
            }

            // And at each mouth: does the hole break the surface? Testing a
            // straight column would be wrong -- a shaft is tilted up to 0.85
            // lateral metres per metre of drop, so it leaves a plumb line
            // within a few metres and the rock beside it is not a seal.
            // What matters is that the ground directly under the mouth is open.
            for (size_t m = 0; m < cw.entrance_mouths.size(); ++m) {
                const SeedWorld::EntranceMouth& em = cw.entrance_mouths[m];
                sum_reach += em.reach;
                if (terrain_density(cw, wp, table, em.x, em.y - 1.0f, em.z) > 0.0f) ++mouth_sealed;
                if (terrain_density(cw, wp, table, em.x, em.y - 4.0f, em.z) > 0.0f) ++mouth_shallow;
            }
        }
        std::printf("trunks per seed: min %d  mean %.1f  max %d  (target %d, mean tries %.0f of %d)\n",
                    min_count, sum_count / nseeds, max_count,
                    wp.entrance_target, sum_tries / nseeds, wp.entrance_tries);
        std::printf("subsurface shaft segments: %d, still solid at the midpoint: %d (%.2f%%)\n",
                    total, pinched, total > 0 ? 100.0 * pinched / total : 0.0);
        std::printf("mean depth of a shaft segment: %.1f m\n",
                    total > 0 ? sum_depth / total : 0.0);
        std::printf("mouths: %.0f total, solid 1 m under the lip: %d, solid 4 m under: %d\n",
                    sum_count, mouth_sealed, mouth_shallow);
        std::printf("mean reach: a shaft finds cave air %.0f m below the surface\n",
                    sum_count > 0.0 ? sum_reach / sum_count : 0.0);
        return 0;
    }

    if (tree_sites) {
        // Where to point a camera: the nearest tree of every species to the
        // origin, and the densest 32 m square within 2 km. Seed as the engine
        // builds it (WorldGpu uses the raw seed, 0 by default).
        const SeedWorld w = build_world(tree_sites_seed, wp, table, ap);
        const float cell = table.trees.cell;
        const size_t ns = table.trees.species.size();
        std::vector<float> best_d(ns, 3.4e38f);
        std::vector<TreeInstance> best_t(ns);
        int dense_n = 0;
        float dense_x = 0.0f, dense_z = 0.0f;
        const int span = static_cast<int>(2000.0f / cell);
        std::vector<TreeInstance> ts;
        for (int tj = -span; tj < span; tj += 8) {
            for (int ti = -span; ti < span; ti += 8) {
                ts.clear();
                trees_in_cells(w, wp, table, ti, tj, ti + 8, tj + 8, ts);
                if (static_cast<int>(ts.size()) > dense_n) {
                    dense_n = static_cast<int>(ts.size());
                    dense_x = (ti + 4) * cell;
                    dense_z = (tj + 4) * cell;
                }
                for (size_t k = 0; k < ts.size(); ++k) {
                    const float d = std::sqrt(ts[k].x * ts[k].x + ts[k].z * ts[k].z);
                    if (d < best_d[ts[k].species]) { best_d[ts[k].species] = d; best_t[ts[k].species] = ts[k]; }
                }
            }
        }
        std::printf("seed %llu: densest 32 m square within 2 km holds %d trees, centred (%.0f, %.0f)\n",
                    static_cast<unsigned long long>(tree_sites_seed), dense_n, dense_x, dense_z);
        for (size_t s = 0; s < ns; ++s) {
            if (best_d[s] > 1e30f) { std::printf("  %-8s none within 2 km\n", table.trees.species[s].name.c_str()); continue; }
            const TreeInstance& t = best_t[s];
            std::printf("  %-8s at (%.1f, %.1f, %.1f), %.1f m tall, %.0f m from the origin\n",
                        table.trees.species[s].name.c_str(), t.x, t.y, t.z, t.height, best_d[s]);
        }
        return 0;
    }

    if (tree_reasons) {
        // Where layer 8's candidates go, per biome of the sampled cell's site:
        // the same 32 m squares the trees check samples, over a few seeds.
        const char* names[TREE_REJECT_COUNT] = {"kept", "outside", "no land", "clearing",
            "roll", "shore", "slope", "river", "altitude", "fit roll", "surface", "flat site"};
        const size_t nb = table.size();
        std::vector<std::vector<double> > hist(nb, std::vector<double>(TREE_REJECT_COUNT, 0.0));
        std::vector<double> hsum(nb, 0.0), hn(nb, 0.0);
        const int ns = std::min(nseeds, 12);
        for (int s = 0; s < ns; ++s) {
            const SeedWorld w = build_world(0x5EEDull * 1000003ull + static_cast<uint64_t>(s), wp, table, ap);
            const float cell = table.trees.cell;
            for (size_t c = 0; c < w.pts.size(); ++c) {
                if (w.biome[c] < 0) continue;
                const int ci = static_cast<int>(std::floor(w.pts[c].first / cell));
                const int cj = static_cast<int>(std::floor(w.pts[c].second / cell));
                for (int j = cj - 4; j < cj + 4; ++j) {
                    for (int i = ci - 4; i < ci + 4; ++i) {
                        // Credited to the dominant biome where the candidate
                        // is: the lookup is warped, so the sampled site's own
                        // biome is usually not the ground around it.
                        const float cx = (i + 0.5f) * cell, cz = (j + 0.5f) * cell;
                        int bb[4];
                        float bw[4];
                        const int m = biome_mix(w, wp, cx, cz, bb, bw);
                        if (m == 0) continue;
                        int dom = 0;
                        for (int k = 1; k < m; ++k) if (bw[k] > bw[dom]) dom = k;
                        const int b = bb[dom];
                        TreeInstance t;
                        int why = 0;
                        tree_at_cell(w, wp, table, i, j, t, &why);
                        hist[b][why] += 1.0;
                        if (i == ci && j == cj) {
                            hsum[b] += terrain_height(w, wp, table, cx, cz,
                                                      std::sqrt(cx * cx + cz * cz) / wp.radius);
                            hn[b] += 1.0;
                        }
                    }
                }
            }
        }
        std::printf("layer 8 candidates by outcome, %% of candidate cells, %d seeds\n%-10s %7s", ns, "biome", "mean h");
        for (int r = 0; r < TREE_REJECT_COUNT; ++r) std::printf(" %8s", names[r]);
        std::printf("\n");
        for (size_t b = 0; b < nb; ++b) {
            double tot = 0.0;
            for (int r = 0; r < TREE_REJECT_COUNT; ++r) tot += hist[b][r];
            if (tot <= 0.0) continue;
            std::printf("%-10s %6.0fm", table[b].name.c_str(), hn[b] > 0 ? hsum[b] / hn[b] : 0.0);
            for (int r = 0; r < TREE_REJECT_COUNT; ++r) std::printf(" %7.1f%%", 100.0 * hist[b][r] / tot);
            std::printf("\n");
        }
        return 0;
    }

    if (spawns) {
        // Where find_spawn puts the player, per seed: the engine's start.
        std::vector<float> dist;
        int strict = 0;
        double ms = 0.0;
        for (int s = 0; s < nseeds; ++s) {
            const uint64_t seed = 0x5EEDull * 1000003ull + static_cast<uint64_t>(s);
            const SeedWorld sw = build_world(seed, wp, table, ap);
            const auto t0 = std::chrono::steady_clock::now();
            const SpawnPoint sp = find_spawn(sw, wp, table);
            ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            const float d = std::sqrt(sp.x * sp.x + sp.z * sp.z);
            dist.push_back(d);
            if (sp.strict) ++strict;
            std::printf("  seed %3d  (%7.0f, %7.0f)  %5.0f m from centre, ground %5.1f m, calm weight %.2f%s\n",
                        s, sp.x, sp.z, d, sp.ground, sp.weight, sp.strict ? "" : "  FALLBACK");
        }
        std::sort(dist.begin(), dist.end());
        std::printf("start point over %d seeds: %d strict; distance from centre median %.0f m, max %.0f m; "
                    "search %.1f ms a seed\n", nseeds, strict, dist[dist.size() / 2], dist.back(), ms / nseeds);
        return 0;
    }

    if (flatness) {
        // How flat each biome is at a building's scale: the height RANGE over
        // a pad (its centre and 16 points on rings at half and full radius),
        // at random land points credited to the dominant biome. A pad under
        // 0.5 m of range is buildable as it stands.
        const float pads[3] = {10.0f, 20.0f, 40.0f};   // pad radius, metres
        const size_t nb = table.size();
        std::vector<std::vector<float> > by[3];
        for (int q = 0; q < 3; ++q) by[q].assign(nb, std::vector<float>());
        const float dg = 0.70710678f;
        const float rx[8] = {1.0f, dg, 0.0f, -dg, -1.0f, -dg, 0.0f, dg};
        const float rz[8] = {0.0f, dg, 1.0f, dg, 0.0f, -dg, -1.0f, -dg};
        for (int s = 0; s < nseeds; ++s) {
            const uint64_t seed = 0x5EEDull * 1000003ull + static_cast<uint64_t>(s);
            const SeedWorld sw = build_world(seed, wp, table, ap);
            Rng rng(seed ^ 0xF1A7ull);
            const float R = wp.radius;
            auto hat = [&](float x, float z) {
                return terrain_height(sw, wp, table, x, z, std::sqrt(x * x + z * z) / R);
            };
            for (int k = 0; k < 3000; ++k) {
                const float x = (rng.uniform() * 2.0f - 1.0f) * 0.9f * R;
                const float z = (rng.uniform() * 2.0f - 1.0f) * 0.9f * R;
                if (x * x + z * z > 0.81f * R * R) continue;
                int bio[4];
                float bw[4];
                const int n = biome_mix(sw, wp, x, z, bio, bw);
                if (n == 0) continue;
                int dom = 0;
                for (int m = 1; m < n; ++m) if (bw[m] > bw[dom]) dom = m;
                const float h0 = hat(x, z);
                if (h0 < table.sea_level + 0.5f) continue;
                for (int q = 0; q < 3; ++q) {
                    float lo = h0, hi = h0;
                    for (int ring = 1; ring <= 2; ++ring) {
                        const float rr = pads[q] * 0.5f * static_cast<float>(ring);
                        for (int a = 0; a < 8; ++a) {
                            const float h = hat(x + rx[a] * rr, z + rz[a] * rr);
                            lo = std::min(lo, h);
                            hi = std::max(hi, h);
                        }
                    }
                    by[q][static_cast<size_t>(bio[dom])].push_back(hi - lo);
                }
            }
        }
        std::printf("height range over a pad, %d seeds: share of land points whose pad "
                    "is under 0.5 / 1 / 2 m, and the median range (m)\n", nseeds);
        std::printf("%-10s %7s   %-28s %-28s %-28s\n", "biome", "points",
                    "pad radius 10 m", "pad radius 20 m", "pad radius 40 m");
        for (size_t b = 0; b < nb; ++b) {
            std::printf("%-10s %7zu", table[b].name.c_str(), by[0][b].size());
            for (int q = 0; q < 3; ++q) {
                std::vector<float>& v = by[q][b];
                if (v.empty()) { std::printf("   %-28s", "-"); continue; }
                std::sort(v.begin(), v.end());
                const double c = static_cast<double>(v.size());
                const double f05 = (std::lower_bound(v.begin(), v.end(), 0.5f) - v.begin()) / c;
                const double f1  = (std::lower_bound(v.begin(), v.end(), 1.0f) - v.begin()) / c;
                const double f2  = (std::lower_bound(v.begin(), v.end(), 2.0f) - v.begin()) / c;
                std::printf("   %5.1f%% %5.1f%% %5.1f%% med %4.1f",
                            100.0 * f05, 100.0 * f1, 100.0 * f2, v[v.size() / 2]);
            }
            std::printf("\n");
        }
        return 0;
    }

    if (tree_quantiles) {
        // The patch noise's distribution, which tree_cover_threshold in
        // field.h bakes. For cover c the threshold is the value the noise
        // exceeds with probability c. Sampled at the patch frequency over the
        // same seeds the gate uses, uniformly over the disc.
        std::vector<float> v;
        const int per = 20000;
        v.reserve(static_cast<size_t>(nseeds) * per);
        for (int s = 0; s < nseeds; ++s) {
            const uint64_t seed = 0x5EEDull * 1000003ull + static_cast<uint64_t>(s);
            const uint32_t sp = feature_seed(seed, FEATURE_TREE_PATCH);
            Rng rng(seed ^ 0x7EEull);
            for (int k = 0; k < per; ++k) {
                const float x = (rng.uniform() * 2.0f - 1.0f) * wp.radius;
                const float z = (rng.uniform() * 2.0f - 1.0f) * wp.radius;
                v.push_back(fbm(x * table.trees.patch_freq, z * table.trees.patch_freq, 3, 2.0f, 0.5f, sp));
            }
        }
        std::sort(v.begin(), v.end());
        std::printf("patch noise over %d seeds x %d samples: min %.4f max %.4f\n",
                    nseeds, per, v.front(), v.back());
        std::printf("cover -> threshold (P(n > q) = cover)\n");
        for (int c = 0; c <= 10; ++c) {
            const double p = 1.0 - c / 10.0;
            const size_t at = std::min(v.size() - 1, static_cast<size_t>(p * (v.size() - 1)));
            std::printf("  q[%d] = %+.4f\n", c, v[at]);
        }
        std::printf("check with the baked table:\n");
        for (int c = 1; c <= 9; ++c) {
            const float thr = tree_cover_threshold(c / 10.0f);
            const size_t above = v.end() - std::upper_bound(v.begin(), v.end(), thr);
            std::printf("  cover %.1f -> threshold %+.4f -> measured %.3f\n",
                        c / 10.0f, thr, double(above) / v.size());
        }
        return 0;
    }

    if (cave_access) {
        // Walk each land column down from above and find the first air below
        // the first solid. That depth is the thickness of the rind sealing
        // the cave system off from the surface.
        std::printf("cave access over %d seeds: depth of the first subsurface air\n", nseeds);
        int  cols = 0, with_air = 0;
        int  bucket[6] = {0, 0, 0, 0, 0, 0};   // <1, <2, <5, <10, <25, >=25 m
        float shallowest = 1.0e9f;
        for (int sd = 0; sd < nseeds; ++sd) {
            // Same seed derivation as the gate, so the two sets of numbers
            // describe the same 200 worlds.
            const SeedWorld cw = build_world(0x5EEDull * 1000003ull + static_cast<uint64_t>(sd), wp, table, ap);
            const float R = wp.radius;
            const int   N = 11;
            for (int gj = 0; gj < N; ++gj) {
                const float z = -R + (gj + 0.5f) * (2.0f * R / N);
                for (int gi = 0; gi < N; ++gi) {
                    const float x = -R + (gi + 0.5f) * (2.0f * R / N);
                    const float rr = std::sqrt(x * x + z * z) / R;
                    if (rr > 1.0f) continue;
                    const float surf = terrain_height(cw, wp, table, x, z, rr);
                    if (surf <= wp.sea_level) continue;

                    // First solid going down, then first air below it.
                    float first_solid = 1.0e9f;
                    float first_air = 1.0e9f;
                    for (int k = 0; k <= 900; ++k) {
                        const float y = surf + 60.0f - 1.0f * k;
                        const bool solid = terrain_density(cw, wp, table, x, y, z) > 0.0f;
                        if (solid && first_solid > 1.0e8f) first_solid = y;
                        if (!solid && first_solid < 1.0e8f) { first_air = y; break; }
                    }
                    if (first_solid > 1.0e8f) continue;
                    ++cols;
                    if (first_air > 1.0e8f) continue;
                    ++with_air;
                    const float depth = first_solid - first_air;
                    if (depth < shallowest) shallowest = depth;
                    if (depth < 1.0f) ++bucket[0];
                    else if (depth < 2.0f) ++bucket[1];
                    else if (depth < 5.0f) ++bucket[2];
                    else if (depth < 10.0f) ++bucket[3];
                    else if (depth < 25.0f) ++bucket[4];
                    else ++bucket[5];
                }
            }
        }
        std::printf("land columns sampled: %d, with subsurface air below them: %d (%.1f%%)\n",
                    cols, with_air, cols > 0 ? 100.0 * with_air / cols : 0.0);
        std::printf("shallowest rind anywhere: %.1f m\n", shallowest);
        std::printf("rind thickness  <1m %d   <2m %d   <5m %d   <10m %d   <25m %d   >=25m %d\n",
                    bucket[0], bucket[1], bucket[2], bucket[3], bucket[4], bucket[5]);
        std::printf("\nA rind that never reaches zero means no cave touches the surface:\n"
                    "the system is sealed and needs entrances designed in.\n");
        return 0;
    }

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
        const float targets[] = {0.40f, 0.35f, 0.32f, 0.30f, 0.28f};
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
                    const float a = layer0_height(x, z, rr, cwld.l0);
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
        Layer0Params dl0 = make_layer0(wp, dump_world_seed);
        if (wp.knee) { float a, b, c; apply_land_knee(dl0, wp, a, b, c); }
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
                if (layer0_height(x, z, rr, dl0) > wp.sea_level) ++land;
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
            const Layer0Params sd = make_layer0(wp, 0);
            for (int rep = 0; rep < 3; ++rep) {
                double sink = 0.0;
                const std::chrono::high_resolution_clock::time_point t0 =
                    std::chrono::high_resolution_clock::now();
                for (int i = 0; i < N; ++i) {
                    const float x = static_cast<float>(i & 2047) * 9.7f - 10000.0f;
                    const float z = static_cast<float>(i >> 11) * 9.7f - 10000.0f;
                    sink += layer0_height(x, z, 0.5f, sd);
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
        // --bless and --golden returned long before this point, so all that is
        // left here is reporting. The comparison itself is the gate's, not a
        // second copy: two implementations would eventually disagree, and the
        // one that disagreed quietly would be this one.
        std::vector<std::pair<uint64_t, uint64_t> > golden_now;
        golden_hashes(wp, table, ap, golden_now);
        const int  golden_moved = had_golden ? golden_moved_count(g, golden_now) : 0;
        const bool golden_ok = had_golden && golden_moved == 0 &&
                               g.generator_version == kGeneratorVersion;

        std::printf("\nchecks over %d seeds\n", nseeds);
        char d[512];

        std::snprintf(d, sizeof(d), "%d seeds missing a biome", s.presence_failures);
        line("presence", s.presence_failures == 0, d);
        std::snprintf(d, sizeof(d), "max %.1f%% off target, tolerance 10%%", 100.0 * s.max_quota_err);
        line("quota", s.quota_failures == 0, d);
        std::snprintf(d, sizeof(d), "Spearman min %.2f, threshold 0.80", s.min_spearman);
        line("radial order", s.radial_failures == 0, d);
        // A mean target plus a per-seed band, not a tight per-seed target.
        // The spread IS the islandness, so squeezing every seed onto the
        // mean would remove the archipelagos along with it.
        const bool land_mean_ok = std::fabs(s.mean_land_frac - kLandMeanTarget) <= 0.02f;
        std::snprintf(d, sizeof(d), "mean %.1f%% (target %.1f +/- 2), seeds %.1f-%.1f%% "
                      "(band %.0f-45), %d outside",
                      100.0 * s.mean_land_frac, 100.0 * kLandMeanTarget,
                      100.0 * s.min_land_frac, 100.0 * s.max_land_frac,
                      100.0 * kLandSeedMin, s.land_failures);
        line("land fraction", s.land_failures == 0 && land_mean_ok, d);
        std::snprintf(d, sizeof(d), "%d seeds break an authored adjacency rule", s.adjacency_failures);
        line("adjacency", s.adjacency_failures == 0, d);
        std::snprintf(d, sizeof(d), "%d seeds with land past r=0.95 or a rim above the void",
                      s.taper_failures);
        line("edge taper", s.taper_failures == 0, d);
        std::snprintf(d, sizeof(d), "smallest spawn landmass %d cells; %d seeds under %d "
                      "(a reading since v18: a bare islet is an allowed start)",
                      s.min_home_cells, s.home_failures, kHomeMinCells);
        std::printf("  %-18s %-6s %s\n", "home landmass", "n/a", d);
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
        // Layer 0 prototype readings (docs/worldgen-design.md, "Layer 0,
        // proposed"), on the same terms: measured, never asserted.
        std::snprintf(d, sizeof(d), "on foot from spawn %.1f%% of land (min %.1f%%, max %.1f%%), "
                      "every biome on foot on %d of %d seeds",
                      100.0 * s.mean_reach, 100.0 * s.min_reach, 100.0 * s.max_reach,
                      s.reach_all_biomes, s.seeds);
        std::printf("  %-18s %-6s %s\n", "", "", d);
        {
            std::string hist = "biomes on foot -> seeds:";
            for (int b = 0; b < 16; ++b) {
                if (!s.reach_biome_hist[b]) continue;
                char t[32];
                std::snprintf(t, sizeof(t), "  %d:%d", b, s.reach_biome_hist[b]);
                hist += t;
            }
            std::printf("  %-18s %-6s %s\n", "", "", hist.c_str());
        }
        std::snprintf(d, sizeof(d), "shelf (water < 0.85 m) %.1f km2/seed (max %.1f)",
                      s.mean_shelf_km2, s.max_shelf_km2);
        std::printf("  %-18s %-6s %s\n", "", "", d);
        std::snprintf(d, sizeof(d), "land before knee %.1f%% (max %.1f%%), after %.1f%% (max %.1f%%)%s, "
                      "knee %.1f ms (max %.1f)",
                      100.0 * s.mean_raw_land, 100.0 * s.max_raw_land,
                      100.0 * s.mean_knee_land, 100.0 * s.max_knee_land,
                      wp.knee ? "" : " [knee off]", s.mean_knee_ms, s.max_knee_ms);
        std::printf("  %-18s %-6s %s\n", "", "", d);
        std::snprintf(d, sizeof(d), "regime size %.0f-%.0f m, mean %+.3f..%+.3f, amp %.3f-%.3f%s",
                      s.min_rsize, s.max_rsize, s.min_rmean, s.max_rmean, s.min_ramp, s.max_ramp,
                      wp.regime_draw ? " (drawn per seed)" : " (fixed)");
        std::printf("  %-18s %-6s %s\n", "", "", d);
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
        std::snprintf(d, sizeof(d), "%d seeds step over 5 cm where the cell blend changes hands; "
                      "worst %.3f m base, %.3f m 3D, %ld crossings",
                      s.seam_failures, s.max_seam_h, s.max_seam_t, s.seam_crossings);
        line("blend seams", s.seam_failures == 0, d);
        std::snprintf(d, sizeof(d), "%.0f segments/seed, all descending and reaching the sea",
                      s.mean_river_segments);
        line("river sanity", s.river_failures == 0, d);
        {
            std::string per;
            for (size_t b = 0; b < table.size() && b < s.tree_per_ha.size(); ++b) {
                char one[64];
                std::snprintf(one, sizeof(one), "%s%s %.0f", b ? ", " : "",
                              table[b].name.c_str(), s.tree_per_ha[b]);
                per += one;
            }
            std::snprintf(d, sizeof(d), "trees/ha %s; rank vs configured %.2f (min 0.80), "
                          "%d bare seed-biomes, %.0f trees hashed/seed",
                          per.c_str(), s.tree_spearman, s.tree_bare_seeds, s.mean_golden_trees);
            line("trees", s.tree_failures == 0, d);
        }
        if (table.rocks.present) {
            std::string per;
            for (size_t b = 0; b < table.size() && b < s.rock_per_ha[0].size(); ++b) {
                char one[80];
                std::snprintf(one, sizeof(one), "%s%s %.1f/%.2f", b ? ", " : "", table[b].name.c_str(),
                              s.rock_per_ha[0][b], s.rock_per_ha[1][b]);
                per += one;
            }
            std::snprintf(d, sizeof(d), "boulders/outcrops per ha %s; %d unseated, %.0f rocks hashed/seed",
                          per.c_str(), s.rocks_unseated, s.mean_golden_rocks);
            line("rocks", s.rock_failures == 0, d);
        }
        if (table.flats.present) {
            std::string per;
            for (size_t b = 0; b < s.flat_per_seed.size(); ++b) {
                if (s.flat_per_seed[b] < 0.05 && table[b].flat_chance <= 0.0f) continue;
                char one[64];
                std::snprintf(one, sizeof(one), "%s%s %.1f", per.empty() ? "" : ", ",
                              table[b].name.c_str(), s.flat_per_seed[b]);
                per += one;
            }
            std::snprintf(d, sizeof(d), "%.1f sites/seed (%s), mean radius %.0f m; worst pad range "
                          "%.3f m (limit 0.02), steepest shoulder %.2f rise/run (limit 0.6)",
                          s.mean_flats, per.c_str(), s.flat_mean_radius, s.flat_worst_pad, s.flat_worst_rim);
            line("flat sites", s.flat_failures == 0, d);
        }
        if (caves_enabled(table)) {
            std::snprintf(d, sizeof(d), "%.1f shafts/seed (min %d, floor 8), reaching cave air "
                          "%.0f m down; %d mouths sealed, %d sampled segments still solid",
                          s.mean_entrance_trunks, s.min_entrance_trunks, s.mean_entrance_reach,
                          s.entrance_mouths_sealed, s.entrance_segs_pinched);
        } else {
            std::snprintf(d, sizeof(d), "caves OFF (every cave_density 0): %d seeds built a "
                          "shaft, must be 0", s.entrance_failures);
        }
        line("cave entrances", s.entrance_failures == 0, d);
        std::snprintf(d, sizeof(d), "%.0f ns/sample, scalar CPU reference; design budget is "
                      "20 ns serial-equivalent for the full field on the shipping path", ns);
        // 1600 since v18 (400 before): the regime and the fine continent noise
        // are two more fbm stacks per sample. Measured 189 -> 441 ns on the
        // dev machine but 207 -> 787 on CI's Windows runner (98 -> 418 on
        // Linux), so the guard keeps v17's ~2x over CI's Windows figure.
        line("throughput", ns <= 1600.0, d);
        if (!had_golden) {
            std::snprintf(d, sizeof(d), "no %s -- run with --bless to pin this world",
                          kGoldenPath);
        } else if (golden_moved > 0) {
            std::snprintf(d, sizeof(d), "%d of %d moved: revert, or bump kGeneratorVersion",
                          golden_moved, static_cast<int>(g.seeds.size()));
        } else if (g.generator_version != kGeneratorVersion) {
            std::snprintf(d, sizeof(d),
                          "hashes agree but are pinned at %d, not %d -- run --bless",
                          g.generator_version, kGeneratorVersion);
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
                         s.boat_failures == 0 &&
                         s.spawn_failures == 0 && s.slope_failures == 0 &&
                         land_mean_ok &&
                         s.river_failures == 0 && s.lod_failures == 0 &&
                         s.seam_failures == 0 && s.tree_failures == 0 &&
                         s.rock_failures == 0 && s.flat_failures == 0 &&
                         // It printed FAIL and never failed the run: the
                         // entrance check was missing from this conjunction.
                         s.entrance_failures == 0 &&
                         ns <= 1600.0 && golden_ok && sheet_ok;
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
