// ---------------------------------------------------------------------------
// Reading the heightfield GI (ADR-004 L6, the no-RT tier's GI; height_gi.h):
// at a point, the sky it sees past the terrain round it and the light the
// terrain bounces at it, both as L1 spherical harmonics gathered over a
// 512 m map of the ground seen from above -- off screen as much as on.
//
// Height matters because the map describes the TOP surface. A point well
// above it (a crown, a cliff top) sees more sky, so the answer fades to the
// open fallback over 40 m. A point well BELOW it is under rock or an
// overhang -- a cave, a dug chamber -- and gets neither sky nor this bounce.
//
// Included by shadow.hlsli, whose constants carry the map (l_hf); its
// textures are a sixth range of the lit pipelines' shadow table, heap
// [48..52] (t38..t42), sampled through the lit pipelines' linear sampler (s3, shadow.hlsli).
// ---------------------------------------------------------------------------
#ifndef ENGINE_HFGI_HLSLI
#define ENGINE_HFGI_HLSLI

#include "engine/render/shaders/hfgi_common.hlsli"

Texture2D<float4> g_hf_vis    : register(t38);   // sky visibility, SH
Texture2D<float4> g_hf_r      : register(t39);   // bounce radiance, SH, red
Texture2D<float4> g_hf_g      : register(t40);
Texture2D<float4> g_hf_b      : register(t41);
Texture2D<float2> g_hf_height : register(t42);   // the top surface's height, its curvature over 6 m

// `vis_e`: the sky's share as an irradiance factor -- pi for a point that
// sees a whole open hemisphere about n, as kSkyMean * vis_e is the sky's
// irradiance. `bounce`: the terrain's bounce irradiance, lux. `cover` how
// much to trust it (0 off the map), for the caller to blend its fallback.
void hfgi_sample(float3 world, float3 n, out float vis_e, out float3 bounce, out float cover) {
    vis_e = 0.0f;
    bounce = float3(0.0f, 0.0f, 0.0f);
    cover = 0.0f;
    if (l_hf.w < 0.5f) return;
    const float2 uv = float2((world.x - l_hf.x) / kHfExtent, 1.0f - (world.z - l_hf.y) / kHfExtent);
    const float2 e = min(uv, 1.0f - uv);
    if (min(e.x, e.y) <= 0.0f) return;
    const float h = g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f).x;
    if (h < kHfNone * 0.5f) return;
    cover = saturate(min(e.x, e.y) * 16.0f);
    float2 suv = uv;
    float k = 1.0f;
    const float dh = world.y - h;
    if (dh < -1.5f) {
        // Under the top surface. An overhang is open to the side and a cave
        // is not: look for the nearest column whose surface lies below this
        // point -- a way out to the sky -- and take ITS light, fading with
        // the distance in. None within 12 m: enclosed, and no sky or bounce
        // reaches it. "Under the surface means covered" alone drew every
        // overhang's underside black.
        float best = 1e9f;
        [loop] for (int r = 1; r <= 3; ++r) {
            const float rad = 2.0f * float(1 << r);   // 4, 8, 16 m
            [loop] for (int a = 0; a < 8; ++a) {
                const float ang = 0.785398f * float(a) + 0.39f * float(r);
                const float2 q = world.xz + float2(cos(ang), sin(ang)) * rad;
                const float2 quv = float2((q.x - l_hf.x) / kHfExtent, 1.0f - (q.y - l_hf.y) / kHfExtent);
                if (any(quv < 0.0f) || any(quv > 1.0f)) continue;
                const float hq = g_hf_height.SampleLevel(g_gi_linear, quv, 0.0f).x;
                if (hq > kHfNone * 0.5f && hq < world.y - 0.5f && rad < best) {
                    best = rad;
                    suv = quv;
                }
            }
            if (best < 1e8f) break;
        }
        k = saturate(1.0f - best / 12.0f) * (best < 1e8f ? 1.0f : 0.0f);
    }
    vis_e = hf_irradiance(g_hf_vis.SampleLevel(g_gi_linear, suv, 0.0f), n) * k;
    bounce = float3(hf_irradiance(g_hf_r.SampleLevel(g_gi_linear, suv, 0.0f), n),
                    hf_irradiance(g_hf_g.SampleLevel(g_gi_linear, suv, 0.0f), n),
                    hf_irradiance(g_hf_b.SampleLevel(g_gi_linear, suv, 0.0f), n)) * k;
    if (dh >= -1.5f) {
        // Above it: towards an open hemisphere with height.
        const float f = saturate(dh / 40.0f);
        vis_e = lerp(vis_e, 3.14159265f * (0.5f + 0.5f * n.y), f);
    }
}

#endif
