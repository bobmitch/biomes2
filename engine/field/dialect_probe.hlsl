// ---------------------------------------------------------------------------
// Does the real field function compile as HLSL?
//
// ADR-001's central bet is that worldgen/shaders/ is "ordinary C++ that also
// compiles as HLSL", and until M1 nothing had ever compiled noise.h or
// field.h that way. B1 proved only the empty half -- field_types.h, which is
// nothing but a WG_FN define once __cplusplus is absent.
//
// This probe exists to answer that question on its own, before any of the
// machinery that would depend on the answer. It calls one function from each
// layer so nothing is dead-stripped before the compiler has seen it.
// ---------------------------------------------------------------------------

#include "worldgen/shaders/field.h"

RWStructuredBuffer<float> g_out : register(u0);

[numthreads(1, 1, 1)]
void cs_probe(uint3 id : SV_DispatchThreadID) {
    // Read from the buffer, not literals. With constant inputs the compiler
    // folds the whole field away and still reports success, which proves the
    // headers PARSE as HLSL but not that any code was generated for them.
    const float x = g_out[1], z = g_out[2], r = g_out[3];

    // Layer 0 and 1: the continent mask and the danger field.
    float acc = layer0_height(x, z, r, 1.0f / 5200.0f, 5, 0.20f, 1.60f, -0.0248f,
                              0x12345678u, 0.0f);
    acc += layer1_danger(x, z, r, 0.60f, 1.0f / 2000.0f, 3, 0x9E3779B1u);

    // Layer 4: a biome profile and the cross-boundary blend.
    BiomeProfile p;
    p.base_elevation = 10.0f;
    p.amplitude = 40.0f;
    p.octaves = 4;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.ridge_blend = 0.3f;
    p.flatness = 1.2f;
    p.overhang_strength = 0.4f;
    p.cave_density = 0.5f;
    p.cave_depth_min = 8.0f;
    p.cave_depth_max = 220.0f;
    acc += layer4_profile(x, z, 1.0f / 900.0f, p, 0xABCDEF01u, 0.0f);
    acc += layer4_blend(1.0f, 0.5f, 2.0f, 0.25f, 3.0f, 0.125f, 4.0f, 0.125f, 1.0f);
    acc += layer4_height(0.3f, 140.0f, 12.0f, 0.0f);

    // Layer 5: the river channel.
    acc += river_segment_distance(x, z, 0.0f, 0.0f, 100.0f, 100.0f);
    acc += river_carve(50.0f, 3.0f, 8.0f, 2.0f);

    // Layer 6: 3D density, with the cave field inside it.
    acc += layer6_density(x, 12.0f, z, 40.0f, p, 1.0f / 50.0f, 30.0f,
                          1.0f / 260.0f, 10.0f, 0x11111111u, 0x22222222u, 0.0f);

    // Layer 7: the entrance shafts.
    acc += entrance_tube_sd(x, 1.0f, z, 0.0f, 0.0f, 0.0f, 2.0f,
                            10.0f, -20.0f, 10.0f, 3.0f);
    acc += entrance_carve(5.0f, -0.5f, 1.5f, 10.0f);
    acc += entrance_radius(0.4f, 8.0f, 2.4f, 1.8f, 3.5f);

    g_out[0] = acc;
}
