// ---------------------------------------------------------------------------
// Reading the DDGI probes (ADR-004 L6; Majercik et al. 2019): irradiance at
// a point from the eight probes round it, each weighted by trilinear
// position, by whether it is in front of the surface, and by Chebyshev's
// bound on its depth moments -- the visibility term that keeps daylight out
// of a cave the probe's rays could not see into.
//
// Included by shadow.hlsli, whose constants carry the cascades (l_ddgi0..3),
// so lighting.hlsli's gi_diffuse sees it in every lit pipeline that binds
// the shadow table; its textures are a fourth range of that table, heap
// [40..42] (t26..t28), and s3 is a static linear sampler.
// ---------------------------------------------------------------------------
#ifndef ENGINE_DDGI_HLSLI
#define ENGINE_DDGI_HLSLI

#include "engine/render/shaders/ddgi_common.hlsli"

Texture2DArray<float4>      g_ddgi_irr    : register(t26);
Texture2DArray<float2>      g_ddgi_vis    : register(t27);
StructuredBuffer<DdgiProbe> g_ddgi_probes : register(t28);
SamplerState                g_ddgi_linear : register(s3);

float4 ddgi_cascade(int c) {
    return c == 0 ? l_ddgi0 : (c == 1 ? l_ddgi1 : l_ddgi2);
}

// Irradiance divided by pi (the cosine-weighted mean radiance the tiles
// store) from cascade c, and the weight it found; zero weight means no
// valid probe round P.
//
// `pending` counts neighbours not yet traced since they scrolled in: only
// those justify asking a coarser cascade. Neighbours switched off in rock
// are an answer -- this point is walled in -- and must not fall through to
// a coarser cascade whose nearest probes stand outside in daylight, where
// the Chebyshev floor and the weights' normalisation hand their light back.
float3 ddgi_cascade_radiance(int c, float3 P, float3 n, float3 v, out float wsum, out int pending) {
    const float4 cs = ddgi_cascade(c);
    const float sp = cs.w;
    const int3 base = int3(cs.xyz);
    // Off the surface, mostly towards the eye, so a probe just behind a wall
    // the surface belongs to does not see the wall's far side.
    const float3 Pb = P + (n * 0.2f + v * 0.8f) * (0.3f * sp);
    const float3 gp = Pb / sp;
    const int3 g0 = int3(floor(gp));
    const float3 a = saturate(gp - float3(g0));
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    wsum = 0.0f;
    pending = 0;
    [loop] for (int i = 0; i < 8; ++i) {
        const int3 o = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3 g = g0 + o;
        if (any(g < base) || any(g >= base + kDdgiDims)) continue;
        const int3 s = ddgi_wrap(g);
        const DdgiProbe pr = g_ddgi_probes[c * kDdgiProbes + ddgi_slot(s)];
        // Not yet traced since it scrolled in, or idle in open air: neither
        // says anything about this point, so a coarser cascade may. Off in
        // rock does: it is a wall.
        if (any(pr.coord.xyz != g) || pr.coord.w < 1 || pr.offset.w > 1.5f) { ++pending; continue; }
        if (pr.offset.w > 0.5f) continue;
        const float3 pp = float3(g) * sp + pr.offset.xyz;
        const float3 to_probe = normalize(pp - P);
        const float wrap = (dot(to_probe, n) + 1.0f) * 0.5f;
        float w = wrap * wrap + 0.2f;
        const float3 pb = Pb - pp;
        const float dist = max(length(pb), 1e-4f);
        const float2 m = g_ddgi_vis.SampleLevel(g_ddgi_linear, float3(ddgi_uv(s, pb / dist, kDdgiVis), float(c)), 0.0f);
        if (dist > m.x) {
            const float var = abs(m.x * m.x - m.y);
            const float d = dist - m.x;
            float cheb = var / (var + d * d);
            cheb = max(cheb * cheb * cheb, 0.0f);
            w *= max(0.05f, cheb);
        }
        w = max(1e-6f, w);
        const float crush = 0.2f;
        if (w < crush) w *= w * w / (crush * crush);
        const float3 tri = lerp(1.0f - a, a, float3(o));
        w *= tri.x * tri.y * tri.z;
        sum += w * g_ddgi_irr.SampleLevel(g_ddgi_linear, float3(ddgi_uv(s, n, kDdgiIrr), float(c)), 0.0f).rgb;
        wsum += w;
    }
    return wsum > 0.0f ? sum / wsum : float3(0.0f, 0.0f, 0.0f);
}

// Irradiance (lux) at P from the finest cascade that covers it, blended
// into the next over the last probe of its edge. `cover` is how much the
// probes answer: 1 inside, falling to 0 over the outermost cascade's edge,
// for the caller to blend its fallback in by.
float3 ddgi_irradiance(float3 P, float3 n, float3 v, out float cover) {
    cover = 0.0f;
    if (l_ddgi3.x < 0.5f) return float3(0.0f, 0.0f, 0.0f);
    [loop] for (int c = 0; c < kDdgiCascades; ++c) {
        const float4 cs = ddgi_cascade(c);
        const float3 gp = P / cs.w - cs.xyz;
        const float3 lo = gp - 1.0f, hi = float3(kDdgiDims - 2) - gp;
        const float e = min(min(min(lo.x, lo.y), lo.z), min(min(hi.x, hi.y), hi.z));
        if (e <= 0.0f) continue;
        float w0;
        int p0;
        const float3 L0 = ddgi_cascade_radiance(c, P, n, v, w0, p0);
        if (w0 <= 1e-5f) {
            if (p0 > 0) continue;            // not traced yet: ask the next cascade
            cover = 1.0f;                    // walled in by rock
            return float3(0.0f, 0.0f, 0.0f);
        }
        const float t = saturate(e);
        if (t >= 1.0f) { cover = 1.0f; return 3.14159265f * L0; }
        if (c == kDdgiCascades - 1) { cover = t; return 3.14159265f * L0; }
        float w1;
        int p1;
        const float3 L1 = ddgi_cascade_radiance(c + 1, P, n, v, w1, p1);
        cover = 1.0f;
        return 3.14159265f * (w1 > 1e-5f ? lerp(L1, L0, t) : L0);
    }
    return float3(0.0f, 0.0f, 0.0f);
}

#endif
