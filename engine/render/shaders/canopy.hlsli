// ---------------------------------------------------------------------------
// Reading the canopy map (ADR-004 L6; canopy.h): how much sky the crowns
// overhead leave a point. Included by shadow.hlsli, whose constants carry
// l_canopy, so every lit shader that binds the shadow table has it, and
// lighting.hlsli's gi_sky_visibility calls it. The map is at heap [38]
// (t25), a third range of that table.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CANOPY_HLSLI
#define ENGINE_CANOPY_HLSLI

#include "engine/render/shaders/canopy_common.hlsli"

Texture2DArray<float4> g_canopy : register(t25);

// Bilinear by four loads: the lit pipelines have no linear sampler to spare.
// `g` in texels from the map's min corner.
float4 canopy_bilinear(float2 g, int slice) {
    g -= 0.5f;
    const float2 fl = floor(g);
    const float2 f = g - fl;
    const int m = int(kCanopySize) - 1;
    const int2 i0 = clamp(int2(fl), 0, m), i1 = clamp(int2(fl) + 1, 0, m);
    const float4 a = g_canopy.Load(int4(i0.x, i0.y, slice, 0));
    const float4 b = g_canopy.Load(int4(i1.x, i0.y, slice, 0));
    const float4 c = g_canopy.Load(int4(i0.x, i1.y, slice, 0));
    const float4 d = g_canopy.Load(int4(i1.x, i1.y, slice, 0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// 1 under open sky, less under crowns. Ignores l_canopy.w's "applied" flag:
// the RT instrument compares it against the traced sky while that is on.
float canopy_visibility_raw(float3 world) {
    if (l_canopy.w < 0.5f) return 1.0f;
    const float2 g = (world.xz - l_canopy.xy) / kCanopyTexel;
    const float S = float(kCanopySize);
    const float edge = min(min(g.x, g.y), min(S - g.x, S - g.y));
    if (edge <= 0.0f) return 1.0f;
    const float h = world.y - l_canopy.z;
    // Where the canopy is above this point, from a wide footprint, then the
    // footprint that height calls for. One load, not four: slice 4 is
    // already 13 m wide, and this only picks the slice.
    const int2 gi = clamp(int2(g), 0, int(kCanopySize) - 1);
    const float4 wide = g_canopy.Load(int4(gi, 4, 0));
    if (wide.x < 1e-3f) return 1.0f;
    const float mid = 0.5f * (wide.y + wide.z) / wide.x;
    const float s = clamp(log2(max(mid - h, 0.5f) * kCanopySpread), 0.0f, float(kCanopySlices - 1));
    const int s0 = int(s);
    const int s1 = min(s0 + 1, kCanopySlices - 1);
    const float4 v = lerp(canopy_bilinear(g, s0), canopy_bilinear(g, s1), s - float(s0));
    if (v.x < 1e-4f) return 1.0f;
    // A point inside the canopy has only the part above it overhead.
    const float b = v.y / v.x, t = v.z / v.x;
    const float frac = saturate((t - h) / max(t - b, 0.5f));
    // Two ends of one question. Averaging the TRANSMITTANCE over the
    // footprint assumes each sky ray crosses one texel's column of crowns;
    // averaging the optical DEPTH first assumes a ray mixes all of them.
    // A slanted ray does some of each. Weighted 3:1 in log space, the map
    // sits inside 0.05 of --rt-report's traced crowns in a dense wood and
    // over clearings and edges alike; the geometric mean (1:1) matched the
    // wood and smeared the wood's dark into the clearings, 1:0 the reverse.
    const float t_mean = max(v.w, 1e-4f);
    const float t_mixed = exp(-kCanopyPath * v.x);
    const float vis = exp(frac * (kCanopyMix * log(t_mixed) + (1.0f - kCanopyMix) * log(t_mean)));
    return lerp(1.0f, vis, saturate(edge / 32.0f));
}

// Trees and grass read it per VERTEX, through lighting.hlsli's g_gi_vertex:
// leaf cards overdraw so heavily that reading the map per pixel cost 0.1 ms
// of a 2.4 ms walking frame, against 0.02 ms for all the terrain.
float canopy_visibility(float3 world) {
    return l_canopy.w > 1.5f ? canopy_visibility_raw(world) : 1.0f;
}

#endif
