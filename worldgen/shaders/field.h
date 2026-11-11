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
float layer0_height(float x, float z, float r, float freq, int octaves, float centre_bias, float edge_drop, float land_offset, uint seed) {
    const float h = fbm(x * freq, z * freq, octaves, 2.0f, 0.5f, seed);
    return h + land_offset + centre_bias * (1.0f - wg_smoothstep(0.0f, 0.25f, r)) - edge_drop * wg_smoothstep(0.80f, 1.0f, r);
}

// Layer 1 -- the danger field, defined on normalised AREA (r squared), never
// on normalised radius. Disc area grows as r squared, so using radius here
// silently skews every biome quota downstream.
float layer1_danger(float x, float z, float r, float w, float freq, int octaves, uint seed) {
    const float r2 = r * r;
    return wg_saturate(r2 + w * fbm(x * freq, z * freq, octaves, 2.0f, 0.5f, seed));
}

#endif
