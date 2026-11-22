// ---------------------------------------------------------------------------
// The near field's update (near_field.h): light under the top surface.
//
//   cs_nf_fill   an 8 m block of the occupancy volume's density, from the
//                field at 1 m (the GPU's real_terrain_density, as the mesher
//                evaluates it), for each block that scrolled in or was dug.
//   cs_nf_edits  the player's edits over those blocks, scattered from the
//                CPU (absolute values at the 1 m points, invariant 5).
//   cs_nf_occ    occupancy from density, a 1 m ramp across the surface.
//   cs_nf_mip    occupancy mips to 16 m, for the cones.
//   cs_nf_probe  per probe, 32 rays marched through the occupancy at 1 m
//                (the mips only skip empty space) until they rise above the
//                heightfield GI's top surface -- out in the open, where its
//                maps say what light is there -- or meet rock. A ray that
//                gets out carries the heightfield's sky visibility and
//                bounce in its direction. L1 SH, the heightfield's own
//                convention, blended over updates as DDGI's probes are.
//
// What it does NOT do yet: light from the walls themselves (sun on a pit
// wall, a torch, glowing things). A ray that meets rock carries nothing, so
// deep is dark -- which is the goal -- and a lit wall bounces nothing
// either, which is the next step (the rays' hits).
// ---------------------------------------------------------------------------
#include "engine/field/real_field.hlsl"
#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/near_field_common.hlsli"

cbuffer NfConstants : register(b0) {
    int4 nf_origin;    // the volume's min world voxel (a multiple of 8); w unused
    uint nf_edits;     // entries in g_edits
    uint nf_probes;    // entries in g_probe_list
    uint nf_pad0, nf_pad1;
    float4 nf_rot0, nf_rot1, nf_rot2;   // this update's random rotation of the ray set (rows)
};
static const int   kNfRays = 32;
static const float kNfHysteresis = 0.85f;
cbuffer NfLevel : register(b3) { uint nf_level; };

StructuredBuffer<int4>   g_blocks     : register(t2);   // world block (8 m units) to fill
StructuredBuffer<float4> g_edits      : register(t3);   // x: storage index (bits), y: density
StructuredBuffer<uint>   g_probe_list : register(t4);   // probe storage indices to update
RWTexture3D<float>       u_density    : register(u0);
RWTexture3D<float>       u_occ[5]     : register(u1);   // mips 0..4 (u1..u5)
RWStructuredBuffer<float4> u_probes   : register(u6);
RWTexture2D<float>       u_top        : register(u7);   // per 1 m column, the topmost rock's surface
SamplerState             g_nf_wrap    : register(s6);   // toroidal: wraps

[numthreads(8, 8, 8)]
void cs_nf_fill(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const int3 w = g_blocks[g.x].xyz * 8 + int3(t);
    u_density[nf_wrap(w, kNfDims)] = real_terrain_density(float(w.x), float(w.y), float(w.z), 1.0f);
}

[numthreads(64, 1, 1)]
void cs_nf_edits(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nf_edits) return;
    const float4 e = g_edits[id.x];
    const uint i = asuint(e.x);
    const int3 s = int3(i % uint(kNfDims.x), (i / uint(kNfDims.x)) % uint(kNfDims.y),
                        i / uint(kNfDims.x * kNfDims.y));
    u_density[s] = e.y;
}

[numthreads(8, 8, 8)]
void cs_nf_occ(uint3 id : SV_DispatchThreadID) {
    // Density is about metres to the surface, positive in rock: occupancy
    // ramps across the metre round it, so a cone sees a surface inside a
    // voxel rather than a step at its edge.
    u_occ[0][id] = saturate(0.5f + u_density[id]);
}

[numthreads(4, 4, 4)]
void cs_nf_mip(uint3 id : SV_DispatchThreadID) {
    const int3 n = kNfDims >> int(nf_level);
    if (any(int3(id) >= n)) return;
    float s = 0.0f;
    [unroll] for (uint c = 0; c < 8u; ++c) s += u_occ[nf_level - 1u][id * 2u + uint3(c & 1u, (c >> 1) & 1u, c >> 2)];
    u_occ[nf_level][id] = s * 0.125f;
}

// The real top surface, per 1 m column: the first rock from the volume's top
// down, its surface found by the density's crossing. The readers decide
// "under cover" by it: the heightfield's 2 m map keeps the HIGHEST point of
// each texel, so ordinary bumpy ground sits up to a metre and a half under
// it, and testing that ground for a roof cost the terrain 0.3 ms at 1440p.
[numthreads(8, 8, 1)]
void cs_nf_top(uint3 id : SV_DispatchThreadID) {
    const int3 o = nf_origin.xyz;
    float top = -1.0e9f;
    float above = -1.0f;
    [loop] for (int y = kNfDims.y - 1; y >= 0; --y) {
        const int3 sw = nf_wrap(int3(int(id.x) - o.x, 0, int(id.y) - o.z), kNfDims);
        const int3 w = int3(o.x + sw.x, o.y + y, o.z + sw.z);
        const float d = u_density[nf_wrap(w, kNfDims)];
        if (d > 0.0f) {
            // Rock at the volume's top: buried deeper than it reaches.
            top = y == kNfDims.y - 1 ? float(w.y) + 1.0f : float(w.y) + d / max(d - above, 1e-4f);
            break;
        }
        above = d;
    }
    u_top[id.xy] = top;
}

// --- probes -----------------------------------------------------------------

bool nf_inside(float3 wp) {
    const float3 rel = wp - float3(nf_origin.xyz);
    return all(rel >= 0.5f) && all(rel <= float3(kNfDims) - 1.5f);
}

float nf_occ_point(float3 wp) {
    return g_nf_occ.Load(int4(nf_wrap(int3(floor(wp + 0.5f)), kNfDims), 0));
}

// The heightfield's map at (x, z): its uv, and whether it holds ground there.
bool nf_hf_uv(float2 xz, out float2 uv, out float top) {
    uv = float2((xz.x - l_hf.x) / kHfExtent, 1.0f - (xz.y - l_hf.y) / kHfExtent);
    top = kHfNone;
    if (l_hf.w < 0.5f || any(uv <= 0.0f) || any(uv >= 1.0f)) return false;
    top = g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f).x;
    return top > kHfNone * 0.5f;
}

// One ray from o along d, marched through the occupancy at its finest
// level -- a cone's coarse levels average a 6 m roof into a half-open one,
// and the sealed chamber of --leak-test lit to 18 nits through it. The
// coarse level only SKIPS: where a 4 m cell holds no rock at all the ray
// strides to its far side. A ray that rises above the heightfield's top
// surface, or leaves the volume, carries back the heightfield's sky
// visibility and bounce in its direction (irradiance / pi of its SH); one
// that meets rock carries nothing yet (the hit is where wall light goes).
void nf_ray(float3 o, float3 d, out float vis, out float3 rad) {
    vis = 0.0f;
    rad = float3(0.0f, 0.0f, 0.0f);
    float t = 0.3f;
    [loop] for (int i = 0; i < 96; ++i) {
        const float3 q = o + d * t;
        float2 uv;
        float top;
        const bool on_map = nf_hf_uv(q.xz, uv, top);
        if (!nf_inside(q) || (on_map && q.y > top + 1.0f)) {
            if (on_map) {
                vis = hf_irradiance(g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f), d) * (1.0f / 3.14159265f);
                rad = float3(hf_irradiance(g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f), d),
                             hf_irradiance(g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f), d),
                             hf_irradiance(g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f), d)) * (1.0f / 3.14159265f);
            } else {
                vis = saturate(0.5f + d.y);   // off the map: open air, sky above
            }
            return;
        }
        const float3 uvw = (q + 0.5f) / float3(kNfDims);
        if (g_nf_occ.SampleLevel(g_nf_wrap, uvw, 0.0f) > 0.5f) return;   // rock
        // Empty 4 m cell (mip 2 is its mean; zero is no rock at all): stride.
        const float coarse = g_nf_occ.SampleLevel(g_nf_wrap, uvw, 2.0f);
        t += coarse <= 0.0f ? 2.0f : 0.5f;
        if (t > 48.0f) return;
    }
}

[numthreads(64, 1, 1)]
void cs_nf_probe(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nf_probes) return;
    const uint pi = g_probe_list[id.x] & 0x7FFFFFFFu;
    const int3 s = int3(pi % uint(kNfProbeDims.x), (pi / uint(kNfProbeDims.x)) % uint(kNfProbeDims.y),
                        pi / uint(kNfProbeDims.x * kNfProbeDims.y));
    const int3 op = nf_origin.xyz / 2;
    const int3 p = op + nf_wrap(s - op, kNfProbeDims);
    const float3 c = float3(p * 2 + 1);
    const uint o4 = pi * uint(kNfProbeFloat4s);

    // At or over the top surface: open air, which the heightfield already
    // answers -- and a probe there only matters to a reader under the
    // surface as a neighbour, which its occupancy test mostly rejects.
    // Tracing these (their sideways rays crawl along the ground at 0.5 m
    // steps) was most of +0.8 ms on an open-ground walk at 1440p.
    float2 uv;
    float top;
    if (nf_hf_uv(c.xz, uv, top) && c.y > top - 0.5f) {
        u_probes[o4 + 0] = g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 1] = g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 2] = g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 3] = g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 4] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // In rock: move it out into the nearest air along an axis, or switch it
    // off. A probe in rock would see nothing and darken the air beside it.
    float3 off = float3(0.0f, 0.0f, 0.0f);
    float valid = 1.0f;
    if (nf_occ_point(c) > 0.5f) {
        valid = 0.0f;
        static const float3 kTry[6] = {float3(0, 0.9f, 0), float3(0.9f, 0, 0), float3(-0.9f, 0, 0),
                                       float3(0, 0, 0.9f), float3(0, 0, -0.9f), float3(0, -0.9f, 0)};
        [loop] for (int k = 0; k < 6 && valid < 0.5f; ++k) {
            if (nf_occ_point(c + kTry[k]) < 0.5f) { off = kTry[k]; valid = 1.0f; }
        }
    }
    if (valid < 0.5f) {
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // N rays over the sphere (a Fibonacci set, turned by this update's random
    // rotation), projected into L1 SH as radiance: c0 += L Y00 dw,
    // c1 += L Y1 d dw. Blended into what the probe held, DDGI's way, so 32
    // rays an update average to hundreds; a probe an edit dirtied starts
    // over (its index carries the top bit).
    const bool fresh = (g_probe_list[id.x] & 0x80000000u) != 0u;
    const float dw = 4.0f * 3.14159265f / float(kNfRays);
    float4 sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    [loop] for (int k = 0; k < kNfRays; ++k) {
        const float zf = 1.0f - (2.0f * float(k) + 1.0f) / float(kNfRays);
        const float rr = sqrt(saturate(1.0f - zf * zf));
        const float ph = 2.39996323f * float(k);
        const float3 d0 = float3(rr * cos(ph), zf, rr * sin(ph));
        const float3 d = float3(dot(nf_rot0.xyz, d0), dot(nf_rot1.xyz, d0), dot(nf_rot2.xyz, d0));
        float vis;
        float3 rad;
        nf_ray(c + off, d, vis, rad);
        const float4 basis = float4(kHfY00, kHfY1 * d) * dw;
        sv += vis * basis;
        sr += rad.r * basis;
        sg += rad.g * basis;
        sb += rad.b * basis;
    }
    const float keep = (fresh || u_probes[o4 + 4].x < 0.5f) ? 0.0f : kNfHysteresis;
    u_probes[o4 + 0] = lerp(sv, u_probes[o4 + 0], keep);
    u_probes[o4 + 1] = lerp(sr, u_probes[o4 + 1], keep);
    u_probes[o4 + 2] = lerp(sg, u_probes[o4 + 2], keep);
    u_probes[o4 + 3] = lerp(sb, u_probes[o4 + 3], keep);
    u_probes[o4 + 4] = float4(1.0f, off);
}
