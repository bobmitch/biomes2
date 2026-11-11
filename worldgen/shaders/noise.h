// ---------------------------------------------------------------------------
// CANONICAL field-function noise (ADR-001).
//
// Written in the restricted C-like dialect: this file compiles UNMODIFIED as
// C++ (for tools/validate, golden hashes and any CPU path) and as HLSL (for
// the engine). tools/wgslgen emits the WGSL that tools/mapview consumes.
// Do not hand-edit the generated WGSL.
//
// Dialect rules, kept tight so the generator stays small:
//   - one statement per line; no multi-line declarations or initialisers
//   - functional casts only -- int(x), not (int)x
//   - arrays are declared bare and filled by assignment, never brace-initialised
//   - no structs, pointers, references, overloads or templates
//   - float literals carry the f suffix; unsigned literals carry u
//   - only the builtins declared in field_types.h
//
// Noise is an analytic integer hash, never a permutation texture: texture
// filtering is not bit-reproducible across backends (CLAUDE.md invariant 3).
// ---------------------------------------------------------------------------
#ifndef WORLDGEN_NOISE_H
#define WORLDGEN_NOISE_H
#include "field_types.h"

uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

uint hash2i(int x, int y, uint seed) {
    return pcg(asuint(x) * 0x9E3779B1u ^ pcg(asuint(y) * 0x85EBCA77u ^ seed));
}

// 16 gradients on the unit circle, indexed by hash. A table, not a texture.
float2 grad2(uint h) {
    float2 kG[16];
    kG[0] = float2(1.000000f, 0.000000f);
    kG[1] = float2(0.923880f, 0.382683f);
    kG[2] = float2(0.707107f, 0.707107f);
    kG[3] = float2(0.382683f, 0.923880f);
    kG[4] = float2(0.000000f, 1.000000f);
    kG[5] = float2(-0.382683f, 0.923880f);
    kG[6] = float2(-0.707107f, 0.707107f);
    kG[7] = float2(-0.923880f, 0.382683f);
    kG[8] = float2(-1.000000f, 0.000000f);
    kG[9] = float2(-0.923880f, -0.382683f);
    kG[10] = float2(-0.707107f, -0.707107f);
    kG[11] = float2(-0.382683f, -0.923880f);
    kG[12] = float2(0.000000f, -1.000000f);
    kG[13] = float2(0.382683f, -0.923880f);
    kG[14] = float2(0.707107f, -0.707107f);
    kG[15] = float2(0.923880f, -0.382683f);
    return kG[h & 15u];
}

// Simplex family, never classic Perlin: Perlin's axis-aligned artifacts are
// invisible at 10 m and glaring at 5 km, which is the scale layer 0 works at.
float simplex2(float x, float y, uint seed) {
    const float F2 = 0.3660254037844386f;
    const float G2 = 0.21132486540518713f;
    const float s = (x + y) * F2;
    const int i = int(floor(x + s));
    const int j = int(floor(y + s));
    const float t = float(i + j) * G2;
    const float x0 = x - (float(i) - t);
    const float y0 = y - (float(j) - t);
    int i1 = 0;
    int j1 = 1;
    if (x0 > y0) { i1 = 1; j1 = 0; }
    const float x1 = x0 - float(i1) + G2;
    const float y1 = y0 - float(j1) + G2;
    const float x2 = x0 - 1.0f + 2.0f * G2;
    const float y2 = y0 - 1.0f + 2.0f * G2;
    float n = 0.0f;
    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f) {
        const float2 g0 = grad2(hash2i(i, j, seed));
        t0 = t0 * t0;
        n = n + t0 * t0 * (g0.x * x0 + g0.y * y0);
    }
    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f) {
        const float2 g1 = grad2(hash2i(i + i1, j + j1, seed));
        t1 = t1 * t1;
        n = n + t1 * t1 * (g1.x * x1 + g1.y * y1);
    }
    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f) {
        const float2 g2 = grad2(hash2i(i + 1, j + 1, seed));
        t2 = t2 * t2;
        n = n + t2 * t2 * (g2.x * x2 + g2.y * y2);
    }
    return 70.0f * n;
}

float fbm(float x, float y, int octaves, float lacunarity, float gain, uint seed) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o = o + 1) {
        sum = sum + amp * simplex2(x * freq, y * freq, seed + asuint(o) * 0x9E3779B1u);
        norm = norm + amp;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    if (norm > 0.0f) { return sum / norm; }
    return 0.0f;
}

// Spelled out rather than using the HLSL/WGSL builtins: the C++ build is the
// determinism authority, so the arithmetic has to be ours, not the backend's.
float wg_saturate(float v) {
    return clamp(v, 0.0f, 1.0f);
}

float wg_smoothstep(float e0, float e1, float v) {
    const float t = wg_saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

#endif
