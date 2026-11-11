// ---------------------------------------------------------------------------
// STAND-IN NOISE. NOT THE AUTHORITY. NOT worldgen/.
//
// This exists so the layer-3 spike has a plausible land mask and danger field
// to chew on. The real field function is worldgen/shaders/noise.wgsl, and when
// that exists THIS FILE MUST BE DELETED rather than kept in sync -- two
// implementations of a float-heavy function will not agree, which is the whole
// point of the CPU/GPU parity section of the design doc.
//
// It lives under tools/ precisely so it cannot be mistaken for worldgen code.
// ---------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <cmath>

namespace spike {

// --- seeding -------------------------------------------------------------
// Per-feature seeds: hash64(world_seed ^ FEATURE_ID). Retuning one layer must
// not reshuffle any other layer.
enum FeatureId : uint32_t {
    FEATURE_CONTINENT = 1u,
    FEATURE_DANGER    = 2u,
    FEATURE_CELLS     = 3u,
    FEATURE_WARP_X    = 4u,
    FEATURE_WARP_Z    = 5u
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

// --- integer hash --------------------------------------------------------
// Analytic, never a permutation texture: bit-reproducible across backends.
inline uint32_t pcg(uint32_t v) {
    uint32_t state = v * 747796405u + 2891336453u;
    uint32_t word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

inline uint32_t hash2i(int32_t x, int32_t y, uint32_t seed) {
    return pcg(static_cast<uint32_t>(x) * 0x9E3779B1u ^
               pcg(static_cast<uint32_t>(y) * 0x85EBCA77u ^ seed));
}

// 16 gradients on the unit circle, indexed by hash. Table, not texture.
inline void grad2(uint32_t h, float& gx, float& gy) {
    static const float kG[16][2] = {
        { 1.000000f,  0.000000f}, { 0.923880f,  0.382683f},
        { 0.707107f,  0.707107f}, { 0.382683f,  0.923880f},
        { 0.000000f,  1.000000f}, {-0.382683f,  0.923880f},
        {-0.707107f,  0.707107f}, {-0.923880f,  0.382683f},
        {-1.000000f,  0.000000f}, {-0.923880f, -0.382683f},
        {-0.707107f, -0.707107f}, {-0.382683f, -0.923880f},
        { 0.000000f, -1.000000f}, { 0.382683f, -0.923880f},
        { 0.707107f, -0.707107f}, { 0.923880f, -0.382683f}
    };
    const uint32_t i = h & 15u;
    gx = kG[i][0];
    gy = kG[i][1];
}

// --- simplex -------------------------------------------------------------
// Simplex family, never classic Perlin: Perlin's axis-aligned artifacts are
// invisible at 10 m and glaring at 5 km, which is the scale layer 0 works at.
inline float simplex2(float x, float y, uint32_t seed) {
    const float F2 = 0.3660254037844386f;   // 0.5 * (sqrt(3) - 1)
    const float G2 = 0.21132486540518713f;  // (3 - sqrt(3)) / 6

    const float s = (x + y) * F2;
    const int   i = static_cast<int>(std::floor(x + s));
    const int   j = static_cast<int>(std::floor(y + s));
    const float t = static_cast<float>(i + j) * G2;

    const float x0 = x - (static_cast<float>(i) - t);
    const float y0 = y - (static_cast<float>(j) - t);

    int i1, j1;
    if (x0 > y0) { i1 = 1; j1 = 0; } else { i1 = 0; j1 = 1; }

    const float x1 = x0 - static_cast<float>(i1) + G2;
    const float y1 = y0 - static_cast<float>(j1) + G2;
    const float x2 = x0 - 1.0f + 2.0f * G2;
    const float y2 = y0 - 1.0f + 2.0f * G2;

    float n = 0.0f;
    float gx, gy, tt;

    tt = 0.5f - x0 * x0 - y0 * y0;
    if (tt > 0.0f) { grad2(hash2i(i, j, seed), gx, gy); tt *= tt; n += tt * tt * (gx * x0 + gy * y0); }

    tt = 0.5f - x1 * x1 - y1 * y1;
    if (tt > 0.0f) { grad2(hash2i(i + i1, j + j1, seed), gx, gy); tt *= tt; n += tt * tt * (gx * x1 + gy * y1); }

    tt = 0.5f - x2 * x2 - y2 * y2;
    if (tt > 0.0f) { grad2(hash2i(i + 1, j + 1, seed), gx, gy); tt *= tt; n += tt * tt * (gx * x2 + gy * y2); }

    return 70.0f * n;  // roughly [-1, 1]
}

inline float fbm(float x, float y, int octaves, float lacunarity, float gain, uint32_t seed) {
    float amp = 1.0f, freq = 1.0f, sum = 0.0f, norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum  += amp * simplex2(x * freq, y * freq, seed + static_cast<uint32_t>(o) * 0x9E3779B1u);
        norm += amp;
        amp  *= gain;
        freq *= lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

// --- helpers -------------------------------------------------------------
inline float saturate(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

inline float smoothstep(float e0, float e1, float v) {
    const float t = saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// --- deterministic RNG for the cell scatter ------------------------------
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

}  // namespace spike
