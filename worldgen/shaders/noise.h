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
//   - parenthesise any mix of bitwise and arithmetic operators. WGSL rejects
//     it outright rather than applying C's precedence, and the C++ build will
//     not warn you, so the mapview shader compile is where this gets caught.
//
// Noise is an analytic integer hash, never a permutation texture: texture
// filtering is not bit-reproducible across backends (CLAUDE.md invariant 3).
// ---------------------------------------------------------------------------
#ifndef WORLDGEN_NOISE_H
#define WORLDGEN_NOISE_H
#include "field_types.h"

WG_FN uint pcg(uint v) {
    const uint state = v * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The parentheses around each multiply are required, not stylistic: WGSL
// refuses to infer precedence when * and ^ are mixed, where C would silently
// apply its own. Parenthesising matches C's grouping exactly, so the C++ and
// HLSL builds are unchanged.
WG_FN uint hash2i(int x, int y, uint seed) {
    return pcg((asuint(x) * 0x9E3779B1u) ^ pcg((asuint(y) * 0x85EBCA77u) ^ seed));
}

// 16 gradients on the unit circle, indexed by hash. A table, not a texture.
WG_FN float2 grad2(uint h) {
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
WG_FN float simplex2(float x, float y, uint seed) {
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

// Spelled out rather than using the HLSL/WGSL builtins: the C++ build is the
// determinism authority, so the arithmetic has to be ours, not the backend's.
WG_FN float wg_saturate(float v) {
    return clamp(v, 0.0f, 1.0f);
}

WG_FN float wg_smoothstep(float e0, float e1, float v) {
    const float t = wg_saturate((v - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

// Smooth maximum, k controlling the width of the rounded join. Layer 0 uses it
// to state the home-landmass guarantee as a FLOOR rather than a LIFT: a lift
// adds height everywhere near the centre, which makes the world centre the
// highest point on almost every seed; a floor only raises ground that was
// already below it, and leaves the peaks to the noise.
WG_FN float wg_smax(float a, float b, float k) {
    const float d = a - b;
    return 0.5f * (a + b + sqrt(d * d + k)) - 0.5f * sqrt(k);
}

// How much of an octave survives at a given voxel size. An octave whose world
// period is below twice the voxel spacing cannot be represented there, so
// sampling it produces detail the LOD cannot hold and the same ground meshed
// finer disagrees. Faded out between four voxels and two rather than cut: a
// hard cut pops on the LOD transition, a fade is invisible and costs a
// multiply.
//
// voxel <= 0 means "no LOD", every octave at full weight. That path returns
// exactly 1.0f, so multiplying by it is bit-identical to not having it --
// which is why adding LOD moved no golden hash.
WG_FN float octave_weight(float period, float voxel) {
    if (voxel <= 0.0f) { return 1.0f; }
    return wg_smoothstep(2.0f, 4.0f, period / voxel);
}

// base_period is the world size of octave 0, in metres. It is passed rather
// than derived because x and y arrive already multiplied by the base
// frequency, and recomputing that product here would round differently.
WG_FN float fbm_lod(float x, float y, int octaves, float lacunarity, float gain, uint seed, float base_period, float voxel) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o = o + 1) {
        const float w = octave_weight(base_period / freq, voxel);
        sum = sum + amp * w * simplex2(x * freq, y * freq, seed + asuint(o) * 0x9E3779B1u);
        // Normalise by the FULL amplitude, not the faded one. Rescaling the
        // survivors would change the shape of what remains, which is exactly
        // what LOD agreement forbids: a coarse sample must be the fine one
        // with its high frequencies removed, not a different world.
        norm = norm + amp;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    if (norm > 0.0f) { return sum / norm; }
    return 0.0f;
}

WG_FN float fbm(float x, float y, int octaves, float lacunarity, float gain, uint seed) {
    return fbm_lod(x, y, octaves, lacunarity, gain, seed, 1.0f, 0.0f);
}


// --- 3D noise (layer 6) -----------------------------------------------------
// Caves and overhangs are volumetric, so they need noise in three dimensions.
// The design doc offers a gyroid as one option for the cave field; it is not
// available to us, because a gyroid is sin and cos and invariant 9 forbids
// libm anywhere upstream of the golden hash. Simplex in 3D needs nothing but
// multiply, add, floor and compare.

WG_FN uint hash3i(int x, int y, int z, uint seed) {
    return pcg(asuint(x) * 0x9E3779B1u ^
               pcg(asuint(y) * 0x85EBCA77u ^
                   pcg(asuint(z) * 0xC2B2AE3Du ^ seed)));
}

// The twelve edge-midpoints of a cube, padded to sixteen so the index is a
// mask rather than a modulo. Perlin's set: equal length, well spread.
WG_FN float3 grad3(uint h) {
    float3 kG[16];
    kG[0] = float3(1.0f, 1.0f, 0.0f);
    kG[1] = float3(-1.0f, 1.0f, 0.0f);
    kG[2] = float3(1.0f, -1.0f, 0.0f);
    kG[3] = float3(-1.0f, -1.0f, 0.0f);
    kG[4] = float3(1.0f, 0.0f, 1.0f);
    kG[5] = float3(-1.0f, 0.0f, 1.0f);
    kG[6] = float3(1.0f, 0.0f, -1.0f);
    kG[7] = float3(-1.0f, 0.0f, -1.0f);
    kG[8] = float3(0.0f, 1.0f, 1.0f);
    kG[9] = float3(0.0f, -1.0f, 1.0f);
    kG[10] = float3(0.0f, 1.0f, -1.0f);
    kG[11] = float3(0.0f, -1.0f, -1.0f);
    kG[12] = float3(1.0f, 1.0f, 0.0f);
    kG[13] = float3(-1.0f, 1.0f, 0.0f);
    kG[14] = float3(0.0f, -1.0f, 1.0f);
    kG[15] = float3(0.0f, -1.0f, -1.0f);
    return kG[h & 15u];
}

WG_FN float simplex3(float x, float y, float z, uint seed) {
    const float F3 = 0.3333333333f;
    const float G3 = 0.1666666667f;

    const float s = (x + y + z) * F3;
    const int i = int(floor(x + s));
    const int j = int(floor(y + s));
    const int k = int(floor(z + s));
    const float t = float(i + j + k) * G3;

    const float x0 = x - (float(i) - t);
    const float y0 = y - (float(j) - t);
    const float z0 = z - (float(k) - t);

    // Which of the six tetrahedra the point is in, decided by ordering the
    // three offsets. Written as comparisons rather than a lookup so it maps
    // straight across to HLSL.
    int i1 = 0; int j1 = 0; int k1 = 0;
    int i2 = 0; int j2 = 0; int k2 = 0;
    if (x0 >= y0) {
        if (y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
        else               { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
    } else {
        if (y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
        else if (x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
        else               { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
    }

    const float x1 = x0 - float(i1) + G3;
    const float y1 = y0 - float(j1) + G3;
    const float z1 = z0 - float(k1) + G3;
    const float x2 = x0 - float(i2) + 2.0f * G3;
    const float y2 = y0 - float(j2) + 2.0f * G3;
    const float z2 = z0 - float(k2) + 2.0f * G3;
    const float x3 = x0 - 1.0f + 3.0f * G3;
    const float y3 = y0 - 1.0f + 3.0f * G3;
    const float z3 = z0 - 1.0f + 3.0f * G3;

    float n = 0.0f;
    float tt = 0.0f;
    float3 g = float3(0.0f, 0.0f, 0.0f);

    tt = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
    if (tt > 0.0f) {
        g = grad3(hash3i(i, j, k, seed));
        tt = tt * tt;
        n = n + tt * tt * (g.x * x0 + g.y * y0 + g.z * z0);
    }
    tt = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
    if (tt > 0.0f) {
        g = grad3(hash3i(i + i1, j + j1, k + k1, seed));
        tt = tt * tt;
        n = n + tt * tt * (g.x * x1 + g.y * y1 + g.z * z1);
    }
    tt = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
    if (tt > 0.0f) {
        g = grad3(hash3i(i + i2, j + j2, k + k2, seed));
        tt = tt * tt;
        n = n + tt * tt * (g.x * x2 + g.y * y2 + g.z * z2);
    }
    tt = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
    if (tt > 0.0f) {
        g = grad3(hash3i(i + 1, j + 1, k + 1, seed));
        tt = tt * tt;
        n = n + tt * tt * (g.x * x3 + g.y * y3 + g.z * z3);
    }
    return 32.0f * n;   // roughly [-1, 1]
}

// Octave-faded 3D fBm, same Nyquist rule as the 2D one.
WG_FN float fbm3_lod(float x, float y, float z, int octaves, float lacunarity, float gain, uint seed, float base_period, float voxel) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o = o + 1) {
        const float w = octave_weight(base_period / freq, voxel);
        sum = sum + amp * w * simplex3(x * freq, y * freq, z * freq, seed + asuint(o) * 0x9E3779B1u);
        norm = norm + amp;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    if (norm > 0.0f) { return sum / norm; }
    return 0.0f;
}

#endif
