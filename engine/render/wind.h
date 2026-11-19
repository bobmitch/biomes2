// ---------------------------------------------------------------------------
// The gust field (weather.h), compiled as C++ AND HLSL like water_wave.h.
//
// It was HLSL alone, which was enough while only shaders bent in it. The RT
// scene now leans each tree's instance by the same sway the vertex shader
// bends its mesh by (tree_sway.h), on the CPU, and a second copy of this is
// exactly how a shadow ends up swaying in a different gust from its tree.
//
// The gusts are a noise field carried BY the air -- sampled at p - drift --
// so a gust front is seen to cross a meadow, then a wood, at the wind's own
// speed. Presentation only: invariant 9 does not reach it.
// ---------------------------------------------------------------------------
#ifndef ENGINE_WIND_H
#define ENGINE_WIND_H

#ifdef __cplusplus
#define WD_FN inline
#include <cmath>
inline float wd_floor(float x) { return std::floor(x); }
inline float wd_frac(float x) { return x - std::floor(x); }
inline float wd_lerp(float a, float b, float t) { return a + (b - a) * t; }
inline float wd_saturate(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
#else
#define WD_FN
#define wd_floor floor
#define wd_frac frac
#define wd_lerp lerp
#define wd_saturate saturate
#endif

// A hash of a lattice point to [0, 1), scalar so it reads the same in both
// languages (the HLSL original was the float2 form of this).
WD_FN float wind_hash2(float x, float y) {
    float px = wd_frac(x * 0.1031f);
    float py = wd_frac(y * 0.1030f);
    const float d = px * (py + 33.33f) + py * (px + 33.33f);
    px = px + d;
    py = py + d;
    return wd_frac((px + py) * px);
}

WD_FN float wind_vnoise2(float x, float y) {
    const float ix = wd_floor(x), iy = wd_floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx);
    const float uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = wind_hash2(ix, iy);
    const float b = wind_hash2(ix + 1.0f, iy);
    const float c = wind_hash2(ix, iy + 1.0f);
    const float d = wind_hash2(ix + 1.0f, iy + 1.0f);
    return wd_lerp(wd_lerp(a, b, ux), wd_lerp(c, d, ux), uy);
}

// The local speed over the mean, ~0.3 to ~2.2, averaging 1, at (px, pz).
// (dx, dz) the way the air moves, `gust` the weather's gustiness 0..1,
// (sx, sz) the drift. Gust cells are ~90 x 45 m, stretched along the wind as
// real gusts are, with a smaller, faster turbulence on top that also evolves
// in place (the t term) so a still day does not freeze into one pattern.
WD_FN float wind_gust_at(float px, float pz, float dx, float dz, float gust,
                         float sx, float sz, float t) {
    const float qx = px - sx, qz = pz - sz;
    // Coordinates along and across the wind.
    const float ax = qx * dx + qz * dz;
    const float az = -qx * dz + qz * dx;
    const float big = wind_vnoise2(ax * (1.0f / 90.0f), az * (1.0f / 45.0f));
    const float fine = wind_vnoise2(ax * (1.0f / 14.0f), az * (1.0f / 14.0f) + t * 0.11f);
    const float n = wd_saturate(big * 0.75f + fine * 0.25f);
    // n^2 averages ~1/3 for value noise, so 3 n^2 averages ~1.
    return (1.0f - gust) + gust * 3.0f * n * n;
}

#endif  // ENGINE_WIND_H
