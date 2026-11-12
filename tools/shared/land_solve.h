// ---------------------------------------------------------------------------
// Choosing a land_offset for a target land fraction.
//
// This is a TOOL for picking a number, not part of generation. Once a value is
// chosen it is baked into WorldParams as a literal; nothing here ever runs in
// the hash path. A runtime search would still be deterministic arithmetic, but
// a baked constant is simply not an argument, and invariant 9 is easier to
// hold when the generator has no search in it at all.
//
// THE SOLVE IS GLOBAL AND MUST STAY GLOBAL. One offset is calibrated so the
// MEAN land fraction across seeds hits the target; individual seeds are left
// free to land above and below it. Solving per seed would make every world
// come out at the same land fraction, and that spread is exactly what produces
// the islandness variety -- the worlds that break into archipelagos are the
// tail of the distribution, not a separate mode. If every seed ends up at the
// target, this is broken.
// ---------------------------------------------------------------------------
#pragma once

#include "worldgen/world.h"

#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace view {

// Layer 0 only: land fraction needs the continent mask and nothing below it,
// so this is far cheaper than a world build and the solve can afford to be
// a plain bisection.
inline float land_fraction_for(const worldgen::WorldParams& wp, uint64_t seed, int grid) {
    using namespace worldgen;
    const uint32_t s_cont = feature_seed(seed, FEATURE_CONTINENT);
    const float R = wp.radius;
    const float step = 2.0f * R / grid;
    long long disc = 0, land = 0;
    for (int j = 0; j < grid; ++j) {
        const float z = -R + (j + 0.5f) * step;
        for (int i = 0; i < grid; ++i) {
            const float x = -R + (i + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            if (rr > 1.0f) continue;
            ++disc;
            if (layer0_height(x, z, rr, wp, s_cont) > wp.sea_level) ++land;
        }
    }
    return disc > 0 ? static_cast<float>(static_cast<double>(land) / disc) : 0.0f;
}

inline float mean_land_fraction(const worldgen::WorldParams& wp, int seeds, int grid) {
    std::vector<float> out(seeds, 0.0f);
    unsigned nt = std::thread::hardware_concurrency();
    if (nt == 0) nt = 4;
    nt = std::min<unsigned>(nt, static_cast<unsigned>(seeds));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < nt; ++t) {
        pool.push_back(std::thread([&, t]() {
            for (int s = static_cast<int>(t); s < seeds; s += static_cast<int>(nt)) {
                out[s] = land_fraction_for(wp, static_cast<uint64_t>(s), grid);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
    double sum = 0.0;
    for (int s = 0; s < seeds; ++s) sum += out[s];
    return seeds > 0 ? static_cast<float>(sum / seeds) : 0.0f;
}

// Bisect land_offset so the MEAN land fraction hits `target`. Land fraction is
// monotone in the offset, so bisection is enough and has no tuning of its own.
inline float solve_land_offset(const worldgen::WorldParams& base, float target,
                               int seeds, int grid) {
    worldgen::WorldParams wp = base;
    float lo = -0.60f, hi = 0.80f;
    for (int it = 0; it < 40; ++it) {
        const float mid = 0.5f * (lo + hi);
        wp.land_offset = mid;
        const float got = mean_land_fraction(wp, seeds, grid);
        if (std::fabs(got - target) < 1.0e-4f) return mid;
        if (got < target) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

// A monotone offset -> mean-land-fraction curve, built once and inverted by
// interpolation. Bisecting on every slider step costs about a second; this
// costs that once at startup and is then instant.
//
// Still a global solve: each sample is a mean across seeds, so reading an
// offset off this curve sets the mean and leaves the seeds their spread.
struct LandCalibration {
    std::vector<std::pair<float, float> > pts;   // (offset, mean land fraction)

    void build(const worldgen::WorldParams& base, int seeds, int grid,
               float lo = -0.10f, float hi = 0.36f, int steps = 20) {
        pts.clear();
        worldgen::WorldParams wp = base;
        for (int i = 0; i <= steps; ++i) {
            const float off = lo + (hi - lo) * (static_cast<float>(i) / steps);
            wp.land_offset = off;
            pts.push_back(std::make_pair(off, mean_land_fraction(wp, seeds, grid)));
        }
    }

    float land_for_offset(float off) const {
        if (pts.empty()) return 0.0f;
        if (off <= pts.front().first) return pts.front().second;
        if (off >= pts.back().first) return pts.back().second;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (off <= pts[i].first) {
                const float t = (off - pts[i - 1].first) / (pts[i].first - pts[i - 1].first);
                return pts[i - 1].second + t * (pts[i].second - pts[i - 1].second);
            }
        }
        return pts.back().second;
    }

    float offset_for_land(float target) const {
        if (pts.empty()) return 0.0f;
        if (target <= pts.front().second) return pts.front().first;
        if (target >= pts.back().second) return pts.back().first;
        for (size_t i = 1; i < pts.size(); ++i) {
            if (target <= pts[i].second) {
                const float span = pts[i].second - pts[i - 1].second;
                const float t = span > 0.0f ? (target - pts[i - 1].second) / span : 0.0f;
                return pts[i - 1].first + t * (pts[i].first - pts[i - 1].first);
            }
        }
        return pts.back().first;
    }
};

}  // namespace view
