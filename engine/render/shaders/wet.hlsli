// ---------------------------------------------------------------------------
// The wet world (docs/adr-005-clouds-and-rain.md, phase C4). Included after
// shadow.hlsli, whose constants carry l_wet: wetness 0..1, puddles 0..1, the
// rain now 0..1, and the ripples' clock (weather.h makes the first two from
// the rain of the last hour, so the world dries after a shower).
//
//   wet_shelter   how much of the rain reaches a point: less under crowns
//                 (the canopy map's sky visibility -- crowns catch rain as
//                 they catch sky light), none under an overhang (the
//                 heightfield GI's top surface well above the point).
//   wet_surface   Lagarde's wet surfaces ("Water drop 2", 2013) cut down:
//                 water in a POROUS surface's pores darkens it (less light
//                 finds its way back out through the water) and smooths it a
//                 little; on a SEALED surface the water stands as a film,
//                 which barely darkens and is glossy. Porosity is the lever.
//   puddle_amount where water stands: flat ground, in the terrain's real
//                 hollows (the heightfield map's curvature over 4 m) and the
//                 low parts of a fixed noise and the scan's own height map,
//                 filling as the puddles value rises.
//   rain_ripples  rings spreading on standing water, one drop per 0.4 m cell
//                 at a time, faded as the pixel outgrows them.
// ---------------------------------------------------------------------------
#ifndef WET_HLSLI
#define WET_HLSLI

#include "engine/render/shaders/ripple.hlsli"

float wet_vnoise(float2 p) {
    const float2 i = floor(p), f = p - i;
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = wet_hash(i), b = wet_hash(i + float2(1, 0));
    const float c = wet_hash(i + float2(0, 1)), d = wet_hash(i + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// The heightfield GI's top surface at a point (hfgi.hlsli's map), or a large
// negative number off the map.
float wet_top(float2 xz) {
    if (l_hf.w < 0.5f) return kHfNone;
    const float2 uv = float2((xz.x - l_hf.x) / kHfExtent, 1.0f - (xz.y - l_hf.y) / kHfExtent);
    if (any(uv <= 0.0f) || any(uv >= 1.0f)) return kHfNone;
    return g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f);
}

float wet_shelter(float3 world) {
    // Crowns: a wood's floor still gets wet (drips, the gaps), only less.
    float s = lerp(0.45f, 1.0f, canopy_visibility_raw(world));
    // An overhang: the top surface a metre and more above the point.
    const float top = wet_top(world.xz);
    if (top > kHfNone * 0.5f) s *= 1.0f - saturate((top - world.y - 1.0f) / 1.0f);
    return s;
}

// The wetness at a surface: the world's, sheltered, and less on steep faces,
// which drain.
float wet_at(float3 world, float3 n) {
    if (l_wet.x <= 0.0f) return 0.0f;
    return l_wet.x * wet_shelter(world) * lerp(0.7f, 1.0f, saturate(n.y));
}

void wet_surface(inout float3 albedo, inout float rough, inout float3 ns, float3 n, float porosity, float wet) {
    if (wet <= 0.0f) return;
    albedo *= lerp(1.0f, lerp(0.85f, 0.5f, porosity), wet);
    // Wet things read more saturated: the water cuts the white scatter
    // off the surface that greys a dry colour.
    const float y = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
    albedo = max(0.0f, lerp(float3(y, y, y), albedo, 1.0f + 0.3f * wet * porosity));
    // Glossy only where the water stands on top. Soaked soil stays nearly
    // matte -- at 0.35 the meadow mirrored the grey sky and read as frost.
    rough = lerp(rough, min(rough, lerp(0.12f, 0.6f, porosity)), wet);
    // A film fills a sealed surface's fine relief.
    ns = normalize(lerp(ns, n, 0.35f * wet * (1.0f - porosity)));
}

// 0..1 standing water. `scan_h` is the scan's height map at the pixel
// (0..1, 0.5 without one); `level` the ground (not rock) share.
float puddle_amount(float3 world, float3 n, float scan_h, float level, float shelter) {
    const float p = l_wet.y * shelter;
    if (p <= 0.0f) return 0.0f;
    const float flat = saturate((n.y - 0.93f) / 0.05f) * level;
    if (flat <= 0.0f) return 0.0f;
    // Low places in a fixed noise (puddles a few metres across) and in the
    // scan's grain, then the terrain's own hollows: how far the point sits
    // below the mean of the ground 4 m round it (2 m texels; positive is a
    // hollow), which pulls the score down by at most 0.4. Most ground cannot
    // hold water whatever its hollow; it leaves before the four lookups.
    const float thr = 0.04f + 0.3f * p;
    float score = 0.65f * wet_vnoise(world.xz / 7.0f) + 0.35f * wet_vnoise(world.xz / 2.3f + 17.0f) +
                  0.3f * (scan_h - 0.5f);
    if (score - 0.4f >= thr) return 0.0f;
    const float h0 = wet_top(world.xz);
    if (h0 > kHfNone * 0.5f) {
        const float hx0 = wet_top(world.xz + float2(4.0f, 0.0f)), hx1 = wet_top(world.xz - float2(4.0f, 0.0f));
        const float hz0 = wet_top(world.xz + float2(0.0f, 4.0f)), hz1 = wet_top(world.xz - float2(0.0f, 4.0f));
        if (min(min(hx0, hx1), min(hz0, hz1)) > kHfNone * 0.5f)
            score -= 4.0f * clamp(0.25f * (hx0 + hx1 + hz0 + hz1) - h0, -0.1f, 0.1f);
    }
    return saturate((thr - score) / 0.035f) * flat;
}

// Standing water over a surface: dark, a mirror, the rings on it. `fp` is the
// pixel's footprint in metres (fwidth of the world position, taken before
// any branch).
void puddle_surface(inout float3 albedo, inout float rough, inout float3 ns, float3 world, float pud, float fp) {
    if (pud <= 0.0f) return;
    albedo *= lerp(1.0f, 0.3f, pud);
    rough = lerp(rough, 0.02f, pud);
    ns = normalize(lerp(ns, puddle_normal(world.xz, l_wet.z, l_wet.w, fp), pud));
}

#endif
