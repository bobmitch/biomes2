// ---------------------------------------------------------------------------
// The heightfield GI's update (ADR-004 L6, the no-RT tier; height_gi.h).
//
//   cs_hf_light   per texel of the top-down map: the height, a normal from its
//                 neighbours, the sun's visibility by marching the heightfield
//                 towards it (and the canopy over the ground), and the
//                 radiance the ground sends back -- albedo / pi times sun, sky
//                 and the bounce the last rebuild found here, which is every
//                 further bounce, one rebuild at a time.
//   cs_hf_gather  per texel: twelve azimuths out to ~90 m, each rise of the
//                 horizon a band of terrain whose radiance goes into an L1
//                 spherical harmonic; above the last horizon, open sky, into
//                 a visibility harmonic. Analytic in elevation, so a band is
//                 exact however wide.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/lighting.hlsli"

cbuffer HfConstants : register(b0) {
    float2 origin_xz;  float y_top;   float y_range;   // the map's min corner; the top-down view's depth range
    uint   first;      float3 pad0;                    // first: no previous rebuild to bounce from
};

Texture2D<float4> g_top_albedo : register(t0);
Texture2D<float>  g_top_depth  : register(t1);
Texture2D<float4> g_hf_rad     : register(t2);
SamplerState      g_clamp      : register(s0);

RWTexture2D<float2> u_height : register(u0);   // the top height, and its curvature over 6 m
RWTexture2D<float4> u_rad    : register(u1);
RWTexture2D<float4> u_vis    : register(u2);
RWTexture2D<float4> u_r      : register(u3);
RWTexture2D<float4> u_g      : register(u4);
RWTexture2D<float4> u_b      : register(u5);
RWStructuredBuffer<float4> u_cam : register(u6);   // x: the camera's share of an open sky's light

float top_height(int2 t) {
    const float d = g_top_depth.Load(int3(clamp(t, 0, int(kHfSize) - 1), 0));
    return d >= 1.0f ? kHfNone : y_top - d * y_range;
}

float2 texel_xz(int2 t) {
    return float2(origin_xz.x + (float(t.x) + 0.5f) * kHfTexel, origin_xz.y + kHfExtent - (float(t.y) + 0.5f) * kHfTexel);
}

float2 xz_uv(float2 xz) {
    return float2((xz.x - origin_xz.x) / kHfExtent, 1.0f - (xz.y - origin_xz.y) / kHfExtent);
}

[numthreads(8, 8, 1)]
void cs_hf_light(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kHfSize || id.y >= kHfSize) return;
    const int2 t = int2(id.xy);
    const float h = top_height(t);
    if (h < kHfNone * 0.5f) {
        u_height[id.xy] = float2(kHfNone, 0.0f);
        u_rad[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // The normal from the neighbours; +j runs towards -z.
    float hl = top_height(t - int2(1, 0)), hr = top_height(t + int2(1, 0));
    float hu = top_height(t - int2(0, 1)), hd = top_height(t + int2(0, 1));
    if (hl < kHfNone * 0.5f) hl = h;
    if (hr < kHfNone * 0.5f) hr = h;
    if (hu < kHfNone * 0.5f) hu = h;
    if (hd < kHfNone * 0.5f) hd = h;
    const float dhdx = (hr - hl) / (2.0f * kHfTexel);
    const float dhdz = (hu - hd) / (2.0f * kHfTexel);
    const float3 n = normalize(float3(-dhdx, 1.0f, -dhdz));
    const float2 xz = texel_xz(t);
    const float3 P = float3(xz.x, h, xz.y);
    const float3 albedo = g_top_albedo.Load(int3(t, 0)).rgb;

    // The sun, through the heightfield and the canopy.
    float3 E = float3(0.0f, 0.0f, 0.0f);
    const float3 l = kSunDir;
    const float nl = dot(n, l);
    if (nl > 0.0f && l.y > 0.0f) {
        float vis = 1.0f;
        const float lh = length(l.xz);
        if (lh > 1e-3f) {
            const float2 dir = l.xz / lh;
            const float rise = l.y / lh;
            float d = kHfTexel;
            [loop] for (int k = 0; k < 24 && vis > 0.0f; ++k) {
                d *= 1.2f;
                const float2 uv = xz_uv(xz + dir * d);
                if (any(uv < 0.0f) || any(uv > 1.0f)) break;
                const float dep = g_top_depth.SampleLevel(g_clamp, uv, 0.0f);
                if (dep >= 1.0f) continue;
                const float hs = y_top - dep * y_range;
                const float ray = h + 0.3f + d * rise;
                vis = min(vis, saturate((ray - hs) / (0.03f * d + 0.3f)));
            }
        }
        vis *= canopy_visibility_raw(P + float3(0.0f, 0.5f, 0.0f));
        E += kSunLux * (nl * vis);
    }
    // The sky and the bounce the last rebuild found here.
    if (first == 1u) {
        const float up = 0.5f + 0.5f * n.y;
        E += kPi * (kSkyMean * up + kGroundRadiance * (1.0f - up));
    } else {
        const float sv = canopy_visibility_raw(P + float3(0.0f, 0.5f, 0.0f));
        E += kSkyMean * (hf_irradiance(g_hf_vis.Load(int3(t, 0)), n) * sv);
        // 2: the light jumped (HeightGi::reset_bounce), and the last
        // rebuild's bounce was the old light's.
        if (first == 0u)
            E += float3(hf_irradiance(g_hf_r.Load(int3(t, 0)), n), hf_irradiance(g_hf_g.Load(int3(t, 0)), n),
                        hf_irradiance(g_hf_b.Load(int3(t, 0)), n));
    }
    // The curvature for the ground's macro variation (mesh.hlsl): the
    // height over the mean of four points 6 m round, + a crest, - a hollow.
    // Here once a texel per rebuild, where the terrain shader read five
    // points of the map for every pixel (0.15 ms at 1440p).
    float curv = 0.0f;
    {
        const float c0 = top_height(t + int2(3, 0)), c1 = top_height(t - int2(3, 0));
        const float c2 = top_height(t + int2(0, 3)), c3 = top_height(t - int2(0, 3));
        if (min(min(c0, c1), min(c2, c3)) > kHfNone * 0.5f) curv = h - 0.25f * (c0 + c1 + c2 + c3);
    }
    u_height[id.xy] = float2(h, curv);
    u_rad[id.xy] = float4(albedo / kPi * E, 1.0f);
}

// The camera's own share of an open sky's light, from the map round it in
// six directions -- what the haze and the mist take near the eye, as DDGI's
// cs_ddgi_camera gives them when it is on. Without it a sealed chamber's air
// glowed with daylight (13.8 nits) while its walls were black.
[numthreads(1, 1, 1)]
void cs_hf_camera() {
    const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                            float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    float lit = 0.0f, open_sum = 0.0f, sky = 0.0f, sky_open = 0.0f;
    [loop] for (int k = 0; k < 6; ++k) {
        const float3 d = dirs[k];
        const float up = 0.5f + 0.5f * d.y;
        const float3 open = kPi * (kSkyMean * up + kGroundRadiance * (1.0f - up));
        float vis_e, cover;
        float3 b;
        hfgi_sample(shadow_cam, d, vis_e, b, cover);
        lit += gi_luma(lerp(open, kSkyMean * vis_e + b, cover));
        open_sum += gi_luma(open);
        // The sky alone, by geometry: no bounce, no light. What the eye's
        // dark floor goes by (dark vision) -- the light share above counts a
        // torch's bounce, which read a torch-lit tunnel at night as open air.
        const float vis_open = kPi * up;
        sky += lerp(vis_open, vis_e, cover);
        sky_open += vis_open;
    }
    u_cam[0] = float4(saturate(lit / max(open_sum, 1e-4f)), saturate(sky / max(sky_open, 1e-4f)), 0.0f, 0.0f);
}

// One band of elevation [a, b] in an azimuth wedge, into an L1 harmonic:
// dw = cos(e) de dphi, and the direction (cos e cos phi, sin e, cos e sin phi).
void hf_band(float a, float b, float cphi, float sphi, float dphi, float wc, out float4 c) {
    const float dw = dphi * (sin(b) - sin(a));
    const float ic = (0.5f * b + 0.25f * sin(2.0f * b)) - (0.5f * a + 0.25f * sin(2.0f * a));
    const float is = 0.5f * (sin(b) * sin(b) - sin(a) * sin(a));
    c = float4(kHfY00 * dw, kHfY1 * wc * cphi * ic, kHfY1 * dphi * is, kHfY1 * wc * sphi * ic);
}

[numthreads(8, 8, 1)]
void cs_hf_gather(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kHfSize || id.y >= kHfSize) return;
    const int2 t = int2(id.xy);
    const float h = g_hf_height.Load(int3(t, 0)).x;
    if (h < kHfNone * 0.5f) {
        u_vis[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        u_r[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        u_g[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        u_b[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    const float2 xz = texel_xz(t);
    const float hr = h + 0.5f;   // a receiver half a metre up: grass, a trunk's foot
    const int K = 12;
    const float dphi = 6.2831853f / float(K);
    const float wc = 2.0f * sin(0.5f * dphi);
    float4 vis = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float4 cr = vis, cg = vis, cb = vis;
    [loop] for (int k = 0; k < K; ++k) {
        const float phi = (float(k) + 0.5f) * dphi;
        const float cphi = cos(phi), sphi = sin(phi);
        const float2 dir = float2(cphi, sphi);
        float hor = -1.5707963f;
        float d = 1.6f;
        [loop] for (int s = 0; s < 17; ++s) {
            d *= 1.25f;
            const float2 uv = xz_uv(xz + dir * d);
            if (any(uv < 0.0f) || any(uv > 1.0f)) break;
            const float hs = g_hf_height.SampleLevel(g_clamp, uv, 0.0f).x;
            if (hs < kHfNone * 0.5f) continue;
            const float th = atan2(hs - hr, d);
            if (th <= hor) continue;
            const float3 L = g_hf_rad.SampleLevel(g_clamp, uv, 0.0f).rgb;
            float4 c;
            hf_band(hor, th, cphi, sphi, dphi, wc, c);
            cr += c * L.r;
            cg += c * L.g;
            cb += c * L.b;
            hor = th;
        }
        float4 c;
        hf_band(hor, 1.5707963f, cphi, sphi, dphi, wc, c);
        vis += c;
    }
    u_vis[id.xy] = vis;
    u_r[id.xy] = cr;
    u_g[id.xy] = cg;
    u_b[id.xy] = cb;
}
