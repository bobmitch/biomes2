// ---------------------------------------------------------------------------
// DDGI's probe update (ADR-004 L6; Majercik et al. 2019, RTXGI's shape).
// SM 6.5, DXC. Per frame, a quarter of each cascade's probes (phase):
//
//   cs_ddgi_trace       128 rays a probe (32 for one switched off in rock)
//                       through gi_trace_probe. A miss is the sky; a hit is
//                       lit by the key light (a transmittance ray: terrain,
//                       trunks, crown volumes), the first few local lights
//                       through their shadow atlas, and LAST frame's probes
//                       at the hit, which is every further bounce. The
//                       crowns in front of the hit pass exp(-tau) of it and
//                       scatter the rest as their leaves' radiance.
//   cs_ddgi_blend_irr   each 8x8 tile texel the cosine-weighted mean of the
//                       rays, blended with what it held; gutters from group
//                       memory, so no separate border pass.
//   cs_ddgi_blend_vis   the same for the 16x16 depth moments (mean, mean
//                       square), sharply weighted.
//   cs_ddgi_relocate    moves a probe out of rock or off a surface it sits
//                       on, switches it off when it cannot get out, and
//                       stamps the slot with the world coordinate it holds.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/gi_trace.hlsli"

cbuffer DdgiConstants : register(b0) {
    float4 ray_rot0;  float4 ray_rot1;  float4 ray_rot2;   // this frame's random rotation of the ray set (rows)
    uint   phase;     uint frame;       float hysteresis;  uint max_lights;
    float4 leaf_radiance;                                    // what a crown's leaves send along a ray, nits
};

RWStructuredBuffer<float4>    u_rays   : register(u0);   // per traced probe and ray: radiance, distance (< 0 back face)
RWStructuredBuffer<DdgiProbe> u_probes : register(u1);
RWTexture2DArray<float4>      u_irr    : register(u2);
RWTexture2DArray<float2>      u_vis    : register(u3);
RWStructuredBuffer<float4>    u_cam    : register(u4);   // x: how much of an open sky's light reaches the camera

static const int kDdgiPerPhase = kDdgiProbes / int(kDdgiPhases);

float3 ddgi_ray_dir(uint i) {
    // A spherical Fibonacci set, rotated afresh every frame so the probes
    // integrate more than 128 fixed directions over time.
    const float phi = 6.2831853f * frac(float(i) * 0.6180339887f);
    const float y = 1.0f - (2.0f * float(i) + 1.0f) / float(kDdgiRays);
    const float r = sqrt(max(0.0f, 1.0f - y * y));
    const float3 d = float3(r * cos(phi), y, r * sin(phi));
    return float3(dot(ray_rot0.xyz, d), dot(ray_rot1.xyz, d), dot(ray_rot2.xyz, d));
}

// The probe a group works on: its slot, and the world coordinate the slot
// should hold now that the cascade sits at its current base.
void ddgi_probe_of(uint group, int c, out int slot, out int3 s, out int3 g) {
    slot = int(group * kDdgiPhases + phase);
    s = ddgi_slot_coord(slot);
    const int3 base = int3(ddgi_cascade(c).xyz);
    g = base + ddgi_wrap(s - base);
}

[numthreads(128, 1, 1)]
void cs_ddgi_trace(uint3 gid : SV_GroupID, uint ray : SV_GroupThreadID) {
    const int c = int(gid.y);
    int slot; int3 s, g;
    ddgi_probe_of(gid.x, c, slot, s, g);
    // Read through the SRV: ddgi_irradiance below reads the same buffer
    // that way, and one pass cannot hold it in both states.
    const DdgiProbe pr = g_ddgi_probes[c * kDdgiProbes + slot];
    const bool fresh = any(pr.coord.xyz != g);
    const bool inactive = !fresh && pr.offset.w > 0.5f;
    const uint out_i = (uint(c * kDdgiPerPhase) + gid.x) * kDdgiRays + ray;
    if (inactive && ray >= kDdgiRaysInactive) {
        u_rays[out_i] = float4(0.0f, 0.0f, 0.0f, kDdgiUnused);
        return;
    }
    const float sp = ddgi_cascade(c).w;
    const float3 origin = float3(g) * sp + (fresh ? float3(0.0f, 0.0f, 0.0f) : pr.offset.xyz);
    const float3 dir = ddgi_ray_dir(ray);
    GiRay r;
    r.origin = origin;
    r.tmin = 0.0f;
    r.dir = dir;
    r.tmax = 2000.0f;
    float tau;
    bool back;
    const GiHit h = gi_trace_probe(r, tau, back);
    float3 L;
    float dist;
    if (!h.hit) {
        // Beyond the traced terrain (LOD 0-3, 512 m) below the horizon is
        // more ground; above it, the sky.
        L = dir.y >= 0.0f ? sky_radiance(dir) : kGroundRadiance;
        dist = kDdgiMiss;
    } else if (back) {
        u_rays[out_i] = float4(0.0f, 0.0f, 0.0f, -h.t);
        return;
    } else {
        const float3 P = origin + dir * h.t;
        const float3 n = h.normal;
        float3 E = float3(0.0f, 0.0f, 0.0f);
        const float nl = dot(n, kSunDir);
        if (nl > 0.0f) {
            GiRay sr;
            sr.origin = P + n * 0.05f;
            sr.tmin = 0.0f;
            sr.dir = kSunDir;
            sr.tmax = 2000.0f;
            E += kSunLux * (nl * gi_transmittance(sr));
        }
        const uint nlights = min(uint(l_cluster.w), max_lights);
        [loop] for (uint i = 0u; i < nlights; ++i) {
            const LocalLightGpu Lg = g_lights[i];
            float3 l;
            const float3 El = light_illuminance(Lg, P, l);
            const float ndl = dot(n, l);
            if (ndl <= 0.0f || max(El.r, max(El.g, El.b)) <= 0.0f) continue;
            E += El * (ndl * local_shadow(Lg, P, n, l));
        }
        // Every further bounce: last frame's probes at the hit. Outside the
        // cascades, the open sky's fallback.
        float cover;
        const float3 Ei = ddgi_irradiance(P, n, -dir, cover);
        const float up = 0.5f + 0.5f * n.y;
        const float3 open = kPi * (kSkyMean * up + kGroundRadiance * (1.0f - up));
        E += Ei + open * (1.0f - cover);
        L = h.albedo / kPi * E;
        dist = h.t;
    }
    const float T = exp(-tau);
    L = L * T + leaf_radiance.rgb * (1.0f - T);
    if (any(isnan(L)) || any(isinf(L))) L = float3(0.0f, 0.0f, 0.0f);
    u_rays[out_i] = float4(L, dist);
}

// The gutter texel k of a tile with interior n, and the interior texel it
// copies: the octahedral fold mirrors each edge about its midpoint.
void ddgi_gutter(uint k, int n, out int2 dst, out int2 src) {
    const int i = int(k % uint(n));
    const uint side = k / uint(n);
    if (side == 0u)      { dst = int2(i, -1); src = int2(n - 1 - i, 0); }
    else if (side == 1u) { dst = int2(i, n);  src = int2(n - 1 - i, n - 1); }
    else if (side == 2u) { dst = int2(-1, i); src = int2(0, n - 1 - i); }
    else if (side == 3u) { dst = int2(n, i);  src = int2(n - 1, n - 1 - i); }
    else {
        const uint q = k - 4u * uint(n);
        if (q == 0u)      { dst = int2(-1, -1); src = int2(n - 1, n - 1); }
        else if (q == 1u) { dst = int2(n, -1);  src = int2(0, n - 1); }
        else if (q == 2u) { dst = int2(-1, n);  src = int2(n - 1, 0); }
        else              { dst = int2(n, n);   src = int2(0, 0); }
    }
}

groupshared float3 gs_irr[64];

[numthreads(8, 8, 1)]
void cs_ddgi_blend_irr(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int c = int(gid.y);
    int slot; int3 s, g;
    ddgi_probe_of(gid.x, c, slot, s, g);
    const DdgiProbe pr = u_probes[c * kDdgiProbes + slot];
    const bool fresh = any(pr.coord.xyz != g);
    const float3 tdir = ddgi_oct_decode((float2(tid.xy) + 0.5f) / float(kDdgiIrr) * 2.0f - 1.0f);
    const uint rbase = (uint(c * kDdgiPerPhase) + gid.x) * kDdgiRays;
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    float ws = 0.0f;
    for (uint r = 0u; r < kDdgiRays; ++r) {
        const float4 ray = u_rays[rbase + r];
        if (ray.w < 0.0f || ray.w >= kDdgiUnused * 0.5f) continue;   // back faces carry nothing
        const float w = max(0.0f, dot(ddgi_ray_dir(r), tdir));
        sum += ray.rgb * w;
        ws += w;
    }
    const int2 o = ddgi_tile_origin(s, kDdgiIrr);
    const uint3 px = uint3(o + int2(tid.xy), uint(c));
    float3 v = u_irr[px].rgb;
    if (ws > 1e-4f) {
        const float3 now = sum / ws;
        v = fresh ? now : lerp(now, v, hysteresis);
    } else if (fresh) {
        v = float3(0.0f, 0.0f, 0.0f);
    }
    u_irr[px] = float4(v, 1.0f);
    gs_irr[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    if (gi < uint(4 * kDdgiIrr + 4)) {
        int2 dst, src;
        ddgi_gutter(gi, kDdgiIrr, dst, src);
        u_irr[uint3(o + dst, uint(c))] = float4(gs_irr[src.y * kDdgiIrr + src.x], 1.0f);
    }
}

groupshared float2 gs_vis[256];

[numthreads(16, 16, 1)]
void cs_ddgi_blend_vis(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    const int c = int(gid.y);
    int slot; int3 s, g;
    ddgi_probe_of(gid.x, c, slot, s, g);
    const DdgiProbe pr = u_probes[c * kDdgiProbes + slot];
    const bool fresh = any(pr.coord.xyz != g);
    const float sp = ddgi_cascade(c).w;
    const float max_d = 1.5f * sp;
    const float3 tdir = ddgi_oct_decode((float2(tid.xy) + 0.5f) / float(kDdgiVis) * 2.0f - 1.0f);
    const uint rbase = (uint(c * kDdgiPerPhase) + gid.x) * kDdgiRays;
    float2 sum = float2(0.0f, 0.0f);
    float ws = 0.0f;
    for (uint r = 0u; r < kDdgiRays; ++r) {
        const float4 ray = u_rays[rbase + r];
        if (ray.w >= kDdgiUnused * 0.5f) continue;
        float d = ray.w;
        // A back face is rock: as good as a wall right against the probe.
        if (d < 0.0f) d *= -0.2f;
        d = min(d, max_d);
        const float w = pow(max(0.0f, dot(ddgi_ray_dir(r), tdir)), 50.0f);
        sum += float2(d, d * d) * w;
        ws += w;
    }
    const int2 o = ddgi_tile_origin(s, kDdgiVis);
    const uint3 px = uint3(o + int2(tid.xy), uint(c));
    float2 v = u_vis[px];
    if (ws > 1e-6f) {
        const float2 now = sum / ws;
        v = fresh ? now : lerp(now, v, hysteresis);
    } else if (fresh) {
        v = float2(max_d, max_d * max_d);
    }
    u_vis[px] = v;
    gs_vis[gi] = v;
    GroupMemoryBarrierWithGroupSync();
    if (gi < uint(4 * kDdgiVis + 4)) {
        int2 dst, src;
        ddgi_gutter(gi, kDdgiVis, dst, src);
        u_vis[uint3(o + dst, uint(c))] = gs_vis[src.y * kDdgiVis + src.x];
    }
}

// The camera's own share of an open sky's light, from the probes round it in
// six directions: ~1 in the open, less under a canopy, ~0 in a sealed cave.
// The CPU reads it back and scales the atmosphere's and the mist's
// in-scattering by it near the camera -- both assume sunlit, sky-lit air,
// and in a sealed chamber they were the whole of what --leak-test found:
// 3.4 nits of haze and 10 of mist, where the walls themselves were black.
[numthreads(1, 1, 1)]
void cs_ddgi_camera() {
    const float3 dirs[6] = {float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                            float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1)};
    float lit = 0.0f, open_sum = 0.0f;
    [loop] for (int k = 0; k < 6; ++k) {
        const float3 d = dirs[k];
        float cover;
        const float3 E = ddgi_irradiance(shadow_cam, d, d, cover);
        const float up = 0.5f + 0.5f * d.y;
        const float3 open = kPi * (kSkyMean * up + kGroundRadiance * (1.0f - up));
        lit += gi_luma(E + open * (1.0f - cover));
        open_sum += gi_luma(open);
        // For --gi-report: what the probes say arrives from each side, and
        // what the fallback would say.
        u_cam[1 + k] = float4(E, cover);
        u_cam[7 + k] = float4(open, 0.0f);
    }
    u_cam[0] = float4(saturate(lit / max(open_sum, 1e-4f)), 0.0f, 0.0f, 0.0f);
}

[numthreads(64, 1, 1)]
void cs_ddgi_relocate(uint3 id : SV_DispatchThreadID) {
    if (id.x >= uint(kDdgiPerPhase)) return;
    const int c = int(id.y);
    int slot; int3 s, g;
    ddgi_probe_of(id.x, c, slot, s, g);
    DdgiProbe pr = u_probes[c * kDdgiProbes + slot];
    const bool fresh = any(pr.coord.xyz != g);
    const float sp = ddgi_cascade(c).w;
    const float min_front = 0.25f * sp;
    const uint rbase = (uint(c * kDdgiPerPhase) + id.x) * kDdgiRays;
    uint traced = 0u, backs = 0u;
    float cb = 1e30f, cf = 1e30f, ff = 0.0f;
    float3 cb_dir = float3(0.0f, 0.0f, 0.0f), cf_dir = cb_dir, ff_dir = cb_dir;
    for (uint r = 0u; r < kDdgiRays; ++r) {
        const float w = u_rays[rbase + r].w;
        if (w >= kDdgiUnused * 0.5f) continue;
        ++traced;
        const float3 d = ddgi_ray_dir(r);
        if (w < 0.0f) {
            ++backs;
            if (-w < cb) { cb = -w; cb_dir = d; }
        } else {
            if (w < cf) { cf = w; cf_dir = d; }
            const float far_d = min(w, 2.0f * sp);
            if (far_d > ff) { ff = far_d; ff_dir = d; }
        }
    }
    const float3 old = fresh ? float3(0.0f, 0.0f, 0.0f) : pr.offset.xyz;
    float3 off = old;
    const bool inside = traced > 0u && float(backs) > 0.25f * float(traced);
    if (inside) {
        // Inside rock: step through the nearest wall seen from behind.
        off += cb_dir * (cb + 0.5f * min_front);
    } else if (cf < min_front) {
        // Sitting on a surface: lean away, towards the most open direction,
        // unless that is back towards the surface.
        if (dot(cf_dir, ff_dir) <= 0.0f) off += ff_dir * min(ff, min_front);
    }
    if (any(abs(off) > 0.45f * sp)) off = old;
    // Idle: no surface within its cell's reach, so no shading point will
    // ask it -- a camera 46 m up had all of cascade 0 in empty air, tracing
    // 128 rays each for nothing. It keeps 32 to notice when one arrives.
    const bool idle = !inside && backs == 0u && cf > 1.75f * sp;
    pr.offset = float4(off, inside ? 1.0f : (idle ? 2.0f : 0.0f));
    pr.coord = int4(g, fresh ? 1 : min(pr.coord.w + 1, 1000000));
    u_probes[c * kDdgiProbes + slot] = pr;
}
