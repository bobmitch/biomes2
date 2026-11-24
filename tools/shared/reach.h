// ---------------------------------------------------------------------------
// Layer 0 readings: how much of a world the player can reach ON FOOT from
// spawn, and how much shallow sea there is. Shared by the harness and the
// viewer so the two report the same numbers.
//
// "On foot" is the design doc's definition (docs/worldgen-design.md, "Layer
// 0, proposed"): land, plus water whose STILL-water depth is under the
// physics' own wading limit, PhysicsWorld::kSwimLeave (0.85 m) -- the depth
// under which a swimmer stands up again. Weather makes a flat rougher, never
// deeper. River channels are not in it: a river is land here.
//
// Readings only, downstream of the hash, never asserted. The doc is explicit
// that the reach is printed and never aimed.
// ---------------------------------------------------------------------------
#pragma once

#include "worldgen/world.h"

#include <cmath>
#include <cstdint>
#include <vector>

namespace view {

using namespace worldgen;

// engine/physics/physics_world.h, PhysicsWorld::kSwimLeave. Copied rather than
// included: tools/ do not link the engine. render.h's kShallowDepth too.
static const float kWadeDepth = 0.85f;

struct FootReach {
    int   grid = 0;
    float land_km2 = 0.0f;        // land on the grid
    float shelf_km2 = 0.0f;       // water under kWadeDepth
    float reach_share = 0.0f;     // share of land reached on foot from spawn
    float reach_km2 = 0.0f;
    int   biomes_reached = 0;     // distinct biomes on the land reached
    int   biomes_present = 0;     // distinct biomes on any land
};

// Per-sample classes, for drawing: 0 outside the disc, 1 deep water,
// 2 shallows (wadeable), 3 land. Bit 2 set where reached from spawn.
inline FootReach foot_reach(const SeedWorld& w, const WorldParams& wp,
                            float spawn_x, float spawn_z, int G,
                            std::vector<unsigned char>* classes = nullptr) {
    FootReach out;
    out.grid = G;
    const float R = wp.radius;
    const float step = 2.0f * R / G;
    const float cell_km2 = step * step * 1.0e-6f;
    std::vector<unsigned char> cls(static_cast<size_t>(G) * G, 0);
    for (int j = 0; j < G; ++j) {
        const float z = -R + (j + 0.5f) * step;
        for (int i = 0; i < G; ++i) {
            const float x = -R + (i + 0.5f) * step;
            const float r = std::sqrt(x * x + z * z) / R;
            if (r > 1.0f) continue;
            const float d = still_water_depth(w, wp, x, z, r);
            unsigned char c = 1;
            if (d <= 0.0f) { c = 3; out.land_km2 += cell_km2; }
            else if (d < kWadeDepth) { c = 2; out.shelf_km2 += cell_km2; }
            cls[static_cast<size_t>(j) * G + i] = c;
        }
    }

    // The walkable sample nearest the spawn point (it is on land, but the
    // grid may put its sample just offshore).
    int si = static_cast<int>((spawn_x + R) / step), sj = static_cast<int>((spawn_z + R) / step);
    int start = -1;
    for (int ring = 0; ring < 6 && start < 0; ++ring) {
        for (int dj = -ring; dj <= ring && start < 0; ++dj) {
            for (int di = -ring; di <= ring && start < 0; ++di) {
                const int i = si + di, j = sj + dj;
                if (i < 0 || j < 0 || i >= G || j >= G) continue;
                if (cls[static_cast<size_t>(j) * G + i] >= 2) start = j * G + i;
            }
        }
    }

    std::vector<int> biome_any(64, 0), biome_hit(64, 0);
    auto biome_at = [&](int idx) {
        const int i = idx % G, j = idx / G;
        const float x = -R + (i + 0.5f) * step, z = -R + (j + 0.5f) * step;
        const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
        const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
        const int ci = w.lookup.nearest(w.pts, wx, wz);
        return ci >= 0 ? w.biome[ci] : -1;
    };
    // Biomes present on land: from the cells, which is what the assignment
    // placed (a cell is land if most of it is).
    for (size_t c = 0; c < w.cells.size(); ++c) {
        const int b = w.biome[c];
        if (b >= 0 && b < 64 && w.cells[c].is_land) biome_any[b] = 1;
    }

    if (start >= 0) {
        long long reached_land = 0, land_total = 0;
        for (size_t k = 0; k < cls.size(); ++k) if (cls[k] == 3) ++land_total;
        std::vector<int> stack;
        stack.push_back(start);
        cls[static_cast<size_t>(start)] |= 4;
        while (!stack.empty()) {
            const int idx = stack.back(); stack.pop_back();
            if ((cls[static_cast<size_t>(idx)] & 3) == 3) {
                ++reached_land;
                const int b = biome_at(idx);
                if (b >= 0 && b < 64) biome_hit[b] = 1;
            }
            const int i = idx % G, j = idx / G;
            const int nb[4] = {i > 0 ? idx - 1 : -1, i < G - 1 ? idx + 1 : -1,
                               j > 0 ? idx - G : -1, j < G - 1 ? idx + G : -1};
            for (int q = 0; q < 4; ++q) {
                if (nb[q] < 0) continue;
                unsigned char& c = cls[static_cast<size_t>(nb[q])];
                if ((c & 4) || (c & 3) < 2) continue;
                c |= 4;
                stack.push_back(nb[q]);
            }
        }
        out.reach_share = land_total > 0 ? static_cast<float>(reached_land) / land_total : 0.0f;
        out.reach_km2 = reached_land * cell_km2;
    }
    for (int b = 0; b < 64; ++b) {
        out.biomes_present += biome_any[b];
        out.biomes_reached += biome_hit[b];
    }
    if (classes) classes->swap(cls);
    return out;
}

// Landmasses on the cell graph (the harness's own measure) and the share of
// land cells on the one holding `home`.
struct Landmasses {
    int   count = 0;
    int   home_cells = 0;
    float home_share = 0.0f;
};

inline Landmasses landmasses(const SeedWorld& w, int home) {
    Landmasses out;
    const std::vector<CellRecord>& cells = w.cells;
    const CellGraph& graph = w.graph;
    const int n = static_cast<int>(cells.size());
    std::vector<int> comp(n, -1);
    std::vector<int> stack;
    int land_total = 0;
    for (int i = 0; i < n; ++i) if (cells[i].is_land) ++land_total;
    for (int i = 0; i < n; ++i) {
        if (!cells[i].is_land || comp[i] >= 0) continue;
        const int id = out.count++;
        int size = 0;
        comp[i] = id;
        stack.push_back(i);
        while (!stack.empty()) {
            const int c = stack.back(); stack.pop_back();
            ++size;
            for (int k = graph.offset[c]; k < graph.offset[c + 1]; ++k) {
                const int nb = graph.list[k];
                if (comp[nb] >= 0 || !cells[nb].is_land) continue;
                comp[nb] = id;
                stack.push_back(nb);
            }
        }
        if (home >= 0 && home < n && comp[home] == id) out.home_cells = size;
    }
    out.home_share = land_total > 0 ? static_cast<float>(out.home_cells) / land_total : 0.0f;
    return out;
}

// The cell a world point falls in, through the same boundary warp as layer 2.
inline int cell_at(const SeedWorld& w, const WorldParams& wp, float x, float z) {
    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
    return w.lookup.nearest(w.pts, wx, wz);
}

}  // namespace view
