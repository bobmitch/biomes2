// ---------------------------------------------------------------------------
// CANONICAL field function, layers 0 and 1 (ADR-001).
//
// Same dialect rules as noise.h: compiles unmodified as C++ and as HLSL,
// tools/wgslgen emits the WGSL. Everything that evaluates per sample lives
// here. The compute kernel that calls it is hand-written per backend and is
// NOT part of the dialect.
//
// Parameters are passed as scalars because the dialect has no structs. That is
// liveable at two layers and will need revisiting when layer 4 arrives with a
// per-biome height profile -- see ADR-001's open questions.
// ---------------------------------------------------------------------------
#ifndef WORLDGEN_FIELD_H
#define WORLDGEN_FIELD_H
#include "noise.h"

// Layer 0 -- continent mask and edge taper. r is normalised radius, 0 at the
// world centre and 1 at the rim. Past r = 1 the caller drops terrain into the
// void; that is a world-bounds decision, not a field one.
//
// The home-landmass guarantee is a FLOOR, not a lift. An additive dome of
// A*(1 - smoothstep(0, 0.25, r)) puts +3.1 standard deviations of the
// continent noise at the centre, which made the world's highest point sit
// within 640 m of the origin on 59 of 64 seeds. Flooring instead leaves relief
// to the noise now and to the layer-4 biome profiles later, which is where it
// belongs. Measured: peaks inside r < 0.25 fall from 64/64 to 2/64, while the
// central disc gets MORE reliably land (90.6% -> 95.6%).
//
// The floor is blended out across r rather than driven to zero, because
// wg_smax(base, 0, k) is a soft ReLU and would clamp every ocean in the world
// to a uniform shelf -- interior ocean depth collapses sevenfold if you do.
WG_FN float layer0_height(float x, float z, float r, float freq, int octaves, float centre_floor, float edge_drop, float land_offset, uint seed) {
    const float base = fbm(x * freq, z * freq, octaves, 2.0f, 0.5f, seed) + land_offset;
    const float t = wg_smoothstep(0.0f, 0.40f, r);
    const float lifted = wg_smax(base, centre_floor, 0.01f) * (1.0f - t) + base * t;
    return lifted - edge_drop * wg_smoothstep(0.80f, 1.0f, r);
}

// Layer 1 -- the danger field, defined on normalised AREA (r squared), never
// on normalised radius. Disc area grows as r squared, so using radius here
// silently skews every biome quota downstream.
WG_FN float layer1_danger(float x, float z, float r, float w, float freq, int octaves, uint seed) {
    const float r2 = r * r;
    return wg_saturate(r2 + w * fbm(x * freq, z * freq, octaves, 2.0f, 0.5f, seed));
}

// --- layer 4: height composition -------------------------------------------
//
// Blend the resulting HEIGHTS across a cell boundary, never the noise
// parameters. Interpolating octave counts and lacunarity produces mush at
// exactly the place the player is looking, so each neighbouring cell's profile
// is evaluated in full and only the metres are mixed.
//
// The per-biome shaping parameters travel as a BiomeProfile struct. They were
// eleven loose scalars while a WGSL generator forbade structs; ADR-002
// retired that, and layer 6's cave parameters would have made it thirteen.

// fBm with a warp from rolling to ridged. ridge_blend 0 is plain fBm; 1 is
// ridged peaks. Not a blend of billow against ridged -- those are mirror
// images of each other and cancel to a flat field at the midpoint.
WG_FN float fbm_shaped(float x, float y, int octaves, float lacunarity, float gain, float ridge_blend, uint seed) {
    float amp = 1.0f;
    float freq = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; o = o + 1) {
        const float n = simplex2(x * freq, y * freq, seed + asuint(o) * 0x9E3779B1u);
        const float ridged = 1.0f - 2.0f * abs(n);
        const float v = n * (1.0f - ridge_blend) + ridged * ridge_blend;
        sum = sum + amp * v;
        norm = norm + amp;
        amp = amp * gain;
        freq = freq * lacunarity;
    }
    if (norm > 0.0f) { return sum / norm; }
    return 0.0f;
}

// A rational bias curve, NOT pow(t, flatness). Same shape -- identity at
// flatness 1, pushes towards the base elevation above it and towards peaks
// below it, endpoints fixed at 0 and 1 -- but built from multiply and divide
// only, so it is bit-identical on every platform.
//
// pow is a libm function and is not exactly specified: glibc and the MSVC CRT
// disagree by an ULP. Layer 4 absorbed that, because its output is continuous.
// Layer 5 does not: the drainage network is a DISCRETE function of this
// heightfield, so one ULP flips a depression-fill comparison and the river
// moves. It cost three of eight golden seeds across platforms before CI caught
// it. The denominator is flatness*(1-t) + t, positive for any flatness > 0.
WG_FN float flatness_curve(float t, float flatness) {
    return t / (flatness * (1.0f - t) + t);
}

// The shaping half of a BiomeArchetype, which is all layer 4 needs. Kept here
// rather than taking the archetype directly so worldgen/shaders/ stays
// independent of the table format, and so the same struct can be packed into
// a constant buffer for the HLSL path.
struct BiomeProfile {
    float base_elevation;
    float amplitude;
    int   octaves;
    float lacunarity;
    float gain;
    float ridge_blend;
    float flatness;
};

// One biome's profile, in metres.
WG_FN float layer4_profile(float x, float z, float freq, BiomeProfile p, uint seed) {
    const float n = fbm_shaped(x * freq, z * freq, p.octaves, p.lacunarity, p.gain, p.ridge_blend, seed);
    const float t = wg_saturate(n * 0.5f + 0.5f);
    const float shaped = flatness_curve(t, p.flatness);
    return p.base_elevation + p.amplitude * (shaped * 2.0f - 1.0f);
}

// Weighted mix of up to four neighbouring cells' heights. Weights come from
// smoothed distance to the cell boundary and are the caller's business: which
// cells are near a point is an index question, not a field one.
WG_FN float layer4_blend(float h0, float w0, float h1, float w1, float h2, float w2, float h3, float w3) {
    const float wsum = w0 + w1 + w2 + w3;
    if (wsum > 0.0f) {
        return (h0 * w0 + h1 * w1 + h2 * w2 + h3 * w3) / wsum;
    }
    return h0;
}

// Final ground height in metres: the biome profiles laid over the shared
// continental base, faded out as the continent mask drops to sea level so the
// coastline does not step.
WG_FN float layer4_height(float continent, float continent_scale, float blended, float sea_level) {
    const float land = wg_smoothstep(sea_level, sea_level + 0.12f, continent);
    return continent * continent_scale + blended * land;
}

// --- layer 5: rivers --------------------------------------------------------
//
// Same split as layer 4. WHICH segments are near a point is a spatial index
// question and belongs to the caller; the geometry and the channel profile are
// here, so both backends agree on the shape of the water. That keeps arrays
// out of the dialect, which is the only reason the generator is still small.

// Distance from a point to a segment, in the plane.
WG_FN float river_segment_distance(float px, float pz, float ax, float az, float bx, float bz) {
    const float vx = bx - ax;
    const float vz = bz - az;
    const float wx = px - ax;
    const float wz = pz - az;
    const float len2 = vx * vx + vz * vz;
    float t = 0.0f;
    if (len2 > 0.0f) {
        t = wg_saturate((wx * vx + wz * vz) / len2);
    }
    const float dx = wx - vx * t;
    const float dz = wz - vz * t;
    return sqrt(dx * dx + dz * dz);
}

// Carve the channel out of the surface. Subtracted from the height rather
// than faked with noise: noise-faked rivers are cheaper and read as fake the
// moment the player has seen real drainage.
WG_FN float river_carve(float h, float d, float width, float depth) {
    const float t = wg_saturate(1.0f - d / width);
    const float profile = t * t * (3.0f - 2.0f * t);
    return h - depth * profile;
}

#endif
