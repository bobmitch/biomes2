// ---------------------------------------------------------------------------
// Gameplay measurements of one built seed: where the player starts, what can
// be walked to from there, how far each biome is, where the level ground is,
// and how danger rises with distance from the start.
//
// The world viewer shows these for the seed on screen and tabulates them over
// many seeds; nothing here asserts anything. Whether 4 km to the nearest
// volcanic biome is right is a design call -- this makes it a number.
//
// Downstream of the golden hash (tools/), so free to use any maths. It reads a
// built SeedWorld and nothing else, and is safe to run on several threads at
// once, each with its own world.
// ---------------------------------------------------------------------------
#pragma once

#include "worldgen/world.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace view {

using namespace worldgen;

// Danger bands by straight-line distance from spawn, in metres. The last is
// open-ended.
static const int   kDangerBands = 5;
static const float kDangerBandEdge[kDangerBands] = {1000.0f, 2000.0f, 4000.0f, 6000.0f, 1.0e9f};

struct SeedMetrics {
    uint64_t   seed = 0;
    SpawnPoint spawn;
    float      spawn_r = 0.0f;        // spawn's distance from the world's centre

    // Islandness by layer-2 cell, the harness's measure.
    float land_fraction = 0.0f;
    int   landmasses = 0;
    int   biggest_pct = 0;            // share of land cells on the largest mass

    // The sample grid: G x G over the disc's bounding square.
    int   G = 0;
    float step = 0.0f;
    std::vector<signed char>   dom;   // dominant biome; -1 water, -2 off the disc
    std::vector<unsigned char> foot;  // 1: land joined to spawn's without crossing water
    int   land_samples = 0, foot_samples = 0;

    // Per biome, indexed like the table.
    std::vector<int>   samples;       // land samples where it dominates
    std::vector<int>   foot_count;    // ... of those, on spawn's landmass
    std::vector<float> nearest;       // metres from spawn to its nearest sample, -1 none
    std::vector<float> nearest_foot;  // ... counting only spawn's landmass, -1 none

    // Layer 5b.
    int   flats = 0, flats_1k = 0, flats_2k = 0;
    float nearest_flat = -1.0f;

    float spawn_danger = 0.0f;
    float nearest_river = -1.0f;      // metres from spawn to a channel's centreline
    float danger_mean[kDangerBands] = {0, 0, 0, 0, 0};
    int   danger_n[kDangerBands] = {0, 0, 0, 0, 0};

    int biomes_off_foot() const {
        int n = 0;
        for (size_t b = 0; b < samples.size(); ++b)
            if (samples[b] > 0 && nearest_foot[b] < 0.0f) ++n;
        return n;
    }
    // The furthest biome's nearest point on spawn's landmass: how far a player
    // must walk to have seen every biome that can be walked to.
    float furthest_foot() const {
        float m = 0.0f;
        for (size_t b = 0; b < nearest_foot.size(); ++b) m = std::max(m, nearest_foot[b]);
        return m;
    }
};

// Connected components of land cells -- the harness's islandness.
inline void cell_islandness(const SeedWorld& w, int& landmasses, int& biggest_pct) {
    const std::vector<CellRecord>& cells = w.cells;
    const CellGraph& graph = w.graph;
    const int n = static_cast<int>(cells.size());
    std::vector<int> comp(n, -1);
    std::vector<int> stack;
    int land_total = 0, biggest = 0;
    for (int i = 0; i < n; ++i) if (cells[i].is_land) ++land_total;
    landmasses = 0;
    for (int i = 0; i < n; ++i) {
        if (!cells[i].is_land || comp[i] >= 0) continue;
        const int id = landmasses++;
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
        if (size > biggest) biggest = size;
    }
    biggest_pct = land_total > 0 ? (100 * biggest) / land_total : 0;
}

// Run fn(i) for i in [0, n) on `threads` threads, interleaved so rows of
// different cost spread evenly. threads <= 1 runs inline.
template <class F>
inline void parallel_rows(int n, int threads, F fn) {
    if (threads <= 1) { for (int i = 0; i < n; ++i) fn(i); return; }
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int i = t; i < n; i += threads) fn(i);
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
}

inline int default_threads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 4 : static_cast<int>(n);
}

// G = 400 is 50 m a sample over a 20 km disc: fine enough that a strait or an
// isthmus a few samples wide is seen, coarse enough to take ~30 ms threaded.
// "On foot" is 4-connected land on this grid, so a land bridge narrower than
// about a sample is missed and a strait narrower than one is crossed.
inline SeedMetrics measure_seed(const SeedWorld& w, const WorldParams& wp,
                                const ArchetypeTable& table, const SpawnPoint& spawn,
                                uint64_t seed, int threads, int G = 400) {
    SeedMetrics m;
    m.seed = seed;
    m.spawn = spawn;
    m.spawn_r = std::sqrt(spawn.x * spawn.x + spawn.z * spawn.z);
    m.land_fraction = w.land_fraction;
    cell_islandness(w, m.landmasses, m.biggest_pct);

    const float R = wp.radius;
    const int nb = static_cast<int>(table.size());
    m.G = G;
    m.step = 2.0f * R / G;
    m.dom.assign(static_cast<size_t>(G) * G, -2);
    m.foot.assign(static_cast<size_t>(G) * G, 0);
    std::vector<float> danger(static_cast<size_t>(G) * G, 0.0f);

    parallel_rows(G, threads, [&](int j) {
        const float z = -R + (j + 0.5f) * m.step;
        for (int i = 0; i < G; ++i) {
            const float x = -R + (i + 0.5f) * m.step;
            const float rr = std::sqrt(x * x + z * z) / R;
            const size_t k = static_cast<size_t>(j) * G + i;
            if (rr > 1.0f) continue;
            const float h = terrain_height(w, wp, table, x, z, rr);
            if (h <= wp.sea_level) { m.dom[k] = -1; continue; }
            int bi[8];
            float bw[8];
            const int n = biome_mix(w, wp, x, z, bi, bw);
            // Two of the four cells can share a biome: sum before choosing.
            int best = -1;
            float bwt = -1.0f;
            for (int q = 0; q < n; ++q) {
                float s = 0.0f;
                for (int r = 0; r < n; ++r) if (bi[r] == bi[q]) s += bw[r];
                if (s > bwt) { bwt = s; best = bi[q]; }
            }
            m.dom[k] = static_cast<signed char>(best < 0 ? -1 : best);
            danger[k] = layer1_danger(x, z, rr, wp, w.s_dang);
        }
    });

    // Spawn's sample, or the nearest land sample to it: a spawn on a 30 m
    // spit can fall between samples.
    int si = static_cast<int>((spawn.x + R) / m.step), sj = static_cast<int>((spawn.z + R) / m.step);
    si = std::max(0, std::min(G - 1, si));
    sj = std::max(0, std::min(G - 1, sj));
    int start = -1;
    for (int ring = 0; ring < 4 && start < 0; ++ring) {
        float bd = 3.4e38f;
        for (int j = sj - ring; j <= sj + ring; ++j) {
            for (int i = si - ring; i <= si + ring; ++i) {
                if (i < 0 || j < 0 || i >= G || j >= G) continue;
                if (m.dom[static_cast<size_t>(j) * G + i] < 0) continue;
                const float d = static_cast<float>((i - si) * (i - si) + (j - sj) * (j - sj));
                if (d < bd) { bd = d; start = j * G + i; }
            }
        }
    }
    if (start >= 0) {
        std::vector<int> stack(1, start);
        m.foot[start] = 1;
        while (!stack.empty()) {
            const int k = stack.back(); stack.pop_back();
            const int i = k % G, j = k / G;
            const int nbr[4][2] = {{i - 1, j}, {i + 1, j}, {i, j - 1}, {i, j + 1}};
            for (int q = 0; q < 4; ++q) {
                const int a = nbr[q][0], b = nbr[q][1];
                if (a < 0 || b < 0 || a >= G || b >= G) continue;
                const int kk = b * G + a;
                if (m.foot[kk] || m.dom[kk] < 0) continue;
                m.foot[kk] = 1;
                stack.push_back(kk);
            }
        }
    }

    m.samples.assign(nb, 0);
    m.foot_count.assign(nb, 0);
    m.nearest.assign(nb, -1.0f);
    m.nearest_foot.assign(nb, -1.0f);
    double dsum[kDangerBands] = {0, 0, 0, 0, 0};
    for (int j = 0; j < G; ++j) {
        const float z = -R + (j + 0.5f) * m.step;
        for (int i = 0; i < G; ++i) {
            const size_t k = static_cast<size_t>(j) * G + i;
            const int b = m.dom[k];
            if (b < 0) continue;
            ++m.land_samples;
            const float x = -R + (i + 0.5f) * m.step;
            const float d = std::sqrt((x - spawn.x) * (x - spawn.x) + (z - spawn.z) * (z - spawn.z));
            int band = 0;
            while (band + 1 < kDangerBands && d >= kDangerBandEdge[band]) ++band;
            dsum[band] += danger[k];
            ++m.danger_n[band];
            if (b >= nb) continue;
            ++m.samples[b];
            if (m.nearest[b] < 0.0f || d < m.nearest[b]) m.nearest[b] = d;
            if (m.foot[k]) {
                ++m.foot_samples;
                ++m.foot_count[b];
                if (m.nearest_foot[b] < 0.0f || d < m.nearest_foot[b]) m.nearest_foot[b] = d;
            }
        }
    }
    for (int q = 0; q < kDangerBands; ++q)
        m.danger_mean[q] = m.danger_n[q] > 0 ? static_cast<float>(dsum[q] / m.danger_n[q]) : 0.0f;

    m.flats = static_cast<int>(w.flats.size());
    for (size_t i = 0; i < w.flats.size(); ++i) {
        const float dx = w.flats[i].x - spawn.x, dz = w.flats[i].z - spawn.z;
        const float d = std::sqrt(dx * dx + dz * dz);
        if (d <= 1000.0f) ++m.flats_1k;
        if (d <= 2000.0f) ++m.flats_2k;
        if (m.nearest_flat < 0.0f || d < m.nearest_flat) m.nearest_flat = d;
    }

    // Straight from the segment list: nearest_river only searches the bins
    // round a point, and a spawn kilometres from any channel finds nothing.
    for (size_t i = 0; i < w.rivers.size(); ++i) {
        const SeedWorld::RiverSeg& s = w.rivers[i];
        const float ex = s.bx - s.ax, ez = s.bz - s.az;
        const float len2 = ex * ex + ez * ez;
        float t = len2 > 0.0f ? ((spawn.x - s.ax) * ex + (spawn.z - s.az) * ez) / len2 : 0.0f;
        t = std::max(0.0f, std::min(1.0f, t));
        const float px = s.ax + ex * t - spawn.x, pz = s.az + ez * t - spawn.z;
        const float d = std::sqrt(px * px + pz * pz);
        if (m.nearest_river < 0.0f || d < m.nearest_river) m.nearest_river = d;
    }
    const float sr = m.spawn_r / R;
    m.spawn_danger = layer1_danger(spawn.x, spawn.z, sr, wp, w.s_dang);
    return m;
}

}  // namespace view
