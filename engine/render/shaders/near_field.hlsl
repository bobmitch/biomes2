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
// A ray that meets rock carries what that wall sends back (nf_hit): the
// sun on it through the cascades and the clouds, every local light on it
// through its own shadow, and the probes round it -- every further bounce,
// a frame at a time -- times the wall's albedo over pi. Deep and unlit is
// still dark: nothing there to send anything. Glowing things are meant to
// be local lights (an algae pool is a few dim unshadowed ones), so they
// reach the walls and the bounce the same way a torch does.
// ---------------------------------------------------------------------------
#include "engine/field/real_field.hlsl"
#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/near_field_common.hlsli"
Texture2D<float> g_cloud_shadow : register(t60);   // clouds.hlsl cs_shadow, last frame's
#include "engine/render/shaders/cloud_shadow.hlsli"

cbuffer NfConstants : register(b0) {
    int4 nf_origin;    // the volume's min world voxel (a multiple of 8); w unused
    uint nf_edits;     // entries in g_edits
    uint nf_probes;    // entries in g_probe_list
    float nf_keep_box;   // the blend over history: probes traced on the camera's box's timer (and a still)
    float nf_keep_rr;    // the round robin's (near_field.h Schedule: exp(-interval / memory))
    float4 nf_rot0, nf_rot1, nf_rot2;   // this update's random rotation of the ray set (rows)
    float4 nf_cloud;                    // the clouds' shadow map: centre x, z, half extent (0: none), base
    uint   nf_lights;                   // local lights to light a hit with (the frame's first, most important)
    uint   nf_pad2, nf_pad3, nf_pad4;
};
// A dug wall's albedo: soil and rock, a little warm. The terrain's own
// material per biome would be better (gaps.md); this is its mean.
static const float3 kNfWallAlbedo = float3(0.26f, 0.23f, 0.20f);
// 64 rays an update, and history kept in SECONDS by how often a probe is
// traced (the blends come from the CPU, near_field.h Schedule). 32 rays and
// 0.85 everywhere made the probes round the camera DANCE wherever the sky
// came in through a small opening (a stairwell 10 m down): a few of 32 rays
// find it, a different few each update.
static const int   kNfRays = 64;
static const float kNfSunNear = 4.0f, kNfSunFar = 12.0f;   // the sun at a ray's hit, faded out over this ray length (nf_hit)
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
    // The block's probes are off until traced again (the plan queues them).
    // The volume is toroidal: a slot that scrolls in still held a probe from
    // the far side -- daylight from the surface, say -- and a neighbour's hit
    // read it through nf_hit_irradiance and passed it round the bounce loop.
    // Walking into a tunnel, its darkest tenth drained from 0.2 to 0.02 nits
    // over half a minute: a slow "eye opening" that was the probes.
    if (all((t & 1u) == 0u)) {
        const uint o4 = nf_probe_index(w / 2) * uint(kNfProbeFloat4s);
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
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

// The key light's visibility at a hit: one tap of the third cascade, and
// the clouds' shadow over it. NOT the cascade by distance, as the mist takes
// it: the near two are fitted to slices of the VIEW, and a hit behind or
// beside the camera falls outside their maps, where the border reads "lit"
// -- full sun on the walls of a sealed tunnel 12 m down. The third is fitted
// round the camera (380 m), so it holds the whole volume; its 0.19 m texels
// are far finer than the volume's metre.
float nf_key_visibility(float3 p) {
    float v = 1.0f;
    if (shadow_res >= 0.0f) {
        const float d = length(p - shadow_cam);
        if (d < shadow_split.w) {
            const uint c = max(2u, uint(l_shadow_frame.z + 0.5f));
            const float4 lp = mul(float4(p, 1.0f), light_vp[c]);
            const float2 suv = float2(lp.x * 0.5f + 0.5f, 0.5f - lp.y * 0.5f) * shadow_scale[c];
            v = g_shadow.SampleCmpLevelZero(g_shadow_cmp, float3(suv, float(c)), lp.z - 0.12f / shadow_depth_range[c]);
        }
    }
    return v * cloud_shadow_at(g_cloud_shadow, g_gi_linear, nf_cloud, kSunDir, p);
}

// What reaches a hit from the probes round it -- the sky through the
// openings and every bounce so far -- read from the buffer being written
// (a probe updated earlier this pass is a frame fresher; DDGI reads last
// frame's). Rock between the hit and a probe drops it, as in the reader.
float3 nf_hit_irradiance(float3 p, float3 n) {
    const float3 q = p + n * 0.5f;
    const float3 f = (q - 1.0f) * 0.5f;
    const int3 p0 = int3(floor(f));
    const float3 t = f - float3(p0);
    const int3 op = nf_origin.xyz / 2;
    float4 sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    float wsum = 0.0f;
    [loop] for (int c = 0; c < 8; ++c) {
        const int3 o = int3(c & 1, (c >> 1) & 1, c >> 2);
        const int3 pq = p0 + o;
        if (any(pq < op) || any(pq >= op + kNfProbeDims)) continue;
        const uint i4 = nf_probe_index(pq) * uint(kNfProbeFloat4s);
        const float4 meta = u_probes[i4 + 4u];
        if (meta.x < 0.5f) continue;
        const float3 pc = float3(pq * 2 + 1) + meta.yzw;
        const float3 v = pc - q;
        if (!nf_clear(q, pc)) continue;
        const float3 tw = lerp(1.0f - t, t, float3(o));
        const float bf = 0.5f + 0.5f * dot(v * rsqrt(max(dot(v, v), 1e-6f)), n);
        const float w = tw.x * tw.y * tw.z * (bf * bf + 0.05f);
        sv += w * u_probes[i4 + 0u];
        sr += w * u_probes[i4 + 1u];
        sg += w * u_probes[i4 + 2u];
        sb += w * u_probes[i4 + 3u];
        wsum += w;
    }
    if (wsum <= 1e-6f) return float3(0.0f, 0.0f, 0.0f);
    const float inv = 1.0f / wsum;
    return kSkyMean * hf_irradiance(sv * inv, n) +
           float3(hf_irradiance(sr * inv, n), hf_irradiance(sg * inv, n), hf_irradiance(sb * inv, n));
}

float nf_occ_lin(float3 wp) {
    return g_nf_occ.SampleLevel(g_nf_wrap, (wp + 0.5f) / float3(kNfDims), 0.0f);
}

// The radiance a wall sends back along -d from hit point p (in air, just
// short of the rock), its normal from the occupancy's gradient.
float3 nf_hit(float3 p, float3 d, float t) {
    const float e = 0.5f;
    const float3 g = float3(nf_occ_lin(p + float3(e, 0, 0)) - nf_occ_lin(p - float3(e, 0, 0)),
                            nf_occ_lin(p + float3(0, e, 0)) - nf_occ_lin(p - float3(0, e, 0)),
                            nf_occ_lin(p + float3(0, 0, e)) - nf_occ_lin(p - float3(0, 0, e)));
    float3 n = dot(g, g) > 1e-8f ? -normalize(g) : -d;
    if (dot(n, d) > 0.0f) n = -n;   // it faces the ray that found it
    float3 E = float3(0.0f, 0.0f, 0.0f);
    // The sun. The volume's wall is a metre-resolution guess at the real
    // mesh, so the shadow is taken half a metre out along the normal.
    // Only for a hit near the probe (faded from kNfSunNear to kNfSunFar
    // metres of ray): a sunlit patch far down a tunnel is a sliver rays
    // rarely find, and each that did lit its probe tenfold -- the torch-lit
    // tunnel's meter wandered 0.25-1.25 nits with it, 0.22-0.29 without.
    // Near the patch many rays find it, smoothly; further off its light
    // arrives second-hand, through those probes (nf_hit_irradiance below).
    const float nl = dot(n, kSunDir);
    const float sun_w = 1.0f - smoothstep(kNfSunNear, kNfSunFar, t);
    if (nl > 0.0f && sun_w > 0.0f) E += kSunLux * (nl * sun_w * nf_key_visibility(p + n * 0.5f));
    // NOT local lights. Their bounce came in through the probes' history --
    // faded in over seconds, and a ray that struck rock beside a flame (the
    // inverse square at a few tens of centimetres) lit its probe tenfold for
    // a frame: a torch-lit tunnel pulsed 2 stops every few seconds. A light's
    // bounce is now its own instant fill (local_lights.hlsli), shadowed by
    // its own map. The probes carry the sky and the sun.
    E += nf_hit_irradiance(p, n);
    const float3 L = kNfWallAlbedo * (E * (1.0f / kPi));
    return (any(isnan(L)) || any(isinf(L))) ? float3(0.0f, 0.0f, 0.0f) : L;
}

// One ray from o along d, marched through the occupancy at its finest
// level -- a cone's coarse levels average a 6 m roof into a half-open one,
// and the sealed chamber of --leak-test lit to 18 nits through it. The
// coarse level only SKIPS: where a 4 m cell holds no rock at all the ray
// strides to its far side. A ray that rises above the heightfield's top
// surface, or leaves the volume, carries back the heightfield's sky
// visibility and bounce in its direction (irradiance / pi of its SH); one
// that meets rock carries what the wall sends back (nf_hit).
void nf_ray(float3 o, float3 d, out float vis, out float3 rad) {
    vis = 0.0f;
    rad = float3(0.0f, 0.0f, 0.0f);
    float t = 0.3f, t_air = 0.0f;
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
        if (g_nf_occ.SampleLevel(g_nf_wrap, uvw, 0.0f) > 0.5f) {
            // Rock: close in on the surface from the last point in air.
            float ta = t_air, tb = t;
            [unroll] for (int k = 0; k < 3; ++k) {
                const float tm = 0.5f * (ta + tb);
                if (nf_occ_lin(o + d * tm) > 0.5f) tb = tm;
                else ta = tm;
            }
            rad = nf_hit(o + d * ta, d, ta);
            return;
        }
        t_air = t;
        // Empty 4 m cell (mip 2 is its mean; zero is no rock at all): stride.
        const float coarse = g_nf_occ.SampleLevel(g_nf_wrap, uvw, 2.0f);
        t += coarse <= 0.0f ? 2.0f : 0.5f;
        if (t > 48.0f) return;
    }
}

// One GROUP a probe and one THREAD a ray. A probe a thread walked its 64
// rays one after another -- up to 96 steps each, and a lit hit gathering 8
// neighbouring probes -- and the pass lasted as long as the slowest probe's
// serial chain: 0.8 ms at 1440p whether it traced 4,608 probes (play) or
// all 131k (a still), and twice that of 32 rays. A thread a ray makes the
// chain one ray long; the group sums the rays' SH in group memory.
// Dispatched (kNfProbeRowGroups, rows): the list is longer than one
// dimension's 65,535 groups in a still.
static const uint kNfProbeRowGroups = 256u;
// Each probe's own turn of the ray set, from a hash of its index, on top of
// the update's shared rotation. With the rotation SHARED, every probe's rays
// pointed the same way in the same frame: down a straight tunnel the sunlit
// ramp at its mouth is a sliver of sky most rays miss, and on the frame the
// rotation aimed one up the tunnel, every probe caught 100 klx at once and
// the whole tunnel flared 10x (the torch walk's meter: 16 pulses in 75 s).
// Turned per probe, the same rare rays land in a few probes a frame, which
// their history averages.
uint nf_pcg(uint v) {
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
float3x3 nf_probe_turn(uint pi) {
    const uint h0 = nf_pcg(pi * 3u + 1u), h1 = nf_pcg(h0 + 7u), h2 = nf_pcg(h1 + 13u);
    const float u1 = float(h0 & 0xFFFFFFu) / 16777216.0f;
    const float u2 = float(h1 & 0xFFFFFFu) / 16777216.0f * 6.2831853f;
    const float u3 = float(h2 & 0xFFFFFFu) / 16777216.0f * 6.2831853f;
    const float a = sqrt(1.0f - u1), b = sqrt(u1);
    const float x = a * sin(u2), y = a * cos(u2), z = b * sin(u3), w = b * cos(u3);
    return float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                    2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                    2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y));
}

groupshared float4 gs_nf_v[kNfRays];
groupshared float4 gs_nf_r[kNfRays];
groupshared float4 gs_nf_g[kNfRays];
groupshared float4 gs_nf_b[kNfRays];

[numthreads(kNfRays, 1, 1)]
void cs_nf_probe(uint3 gid : SV_GroupID, uint k : SV_GroupIndex) {
    const uint li = gid.y * kNfProbeRowGroups + gid.x;
    const bool live = li < nf_probes;
    const uint entry = live ? g_probe_list[li] : 0u;
    const uint pi = entry & 0x3FFFFFFFu;
    const int3 s = int3(pi % uint(kNfProbeDims.x), (pi / uint(kNfProbeDims.x)) % uint(kNfProbeDims.y),
                        pi / uint(kNfProbeDims.x * kNfProbeDims.y));
    const int3 op = nf_origin.xyz / 2;
    const int3 p = op + nf_wrap(s - op, kNfProbeDims);
    const float3 c = float3(p * 2 + 1);
    const uint o4 = pi * uint(kNfProbeFloat4s);

    // What the probe needs, the same in every thread of its group (so the
    // barriers below stay in uniform flow): 0 nothing (past the list), 1 a
    // copy of the heightfield, 2 switched off, 3 traced.
    //
    // At or over the top surface: open air, which the heightfield already
    // answers -- and a probe there only matters to a reader under the
    // surface as a neighbour, which its occupancy test mostly rejects.
    // Tracing these (their sideways rays crawl along the ground at 0.5 m
    // steps) was most of +0.8 ms on an open-ground walk at 1440p.
    // Traced only under real cover: under the 1 m top surface (a tunnel, a
    // roof), or more than 1.5 m under the heightfield's 2 m map (a narrow
    // pit, which the map's texel maximum spans). Open air over a slope also
    // sits under that maximum; tracing it -- and since the hits are lit,
    // lighting every hit -- doubled the walking cost.
    uint mode = 0u;
    float2 uv = 0.0f;
    float3 off = float3(0.0f, 0.0f, 0.0f);
    if (live) {
        float top;
        const float ttop = u_top[nf_wrap(int3(int(floor(c.x + 0.5f)), 0, int(floor(c.z + 0.5f))), kNfDims).xz];
        if (nf_hf_uv(c.xz, uv, top) && c.y > top - 1.5f && c.y > ttop - 0.5f) {
            mode = 1u;
        } else {
            // In rock: move it out into the nearest air along an axis, or
            // switch it off. A probe in rock would see nothing and darken the
            // air beside it.
            mode = 3u;
            if (nf_occ_point(c) > 0.5f) {
                mode = 2u;
                static const float3 kTry[6] = {float3(0, 0.9f, 0), float3(0.9f, 0, 0), float3(-0.9f, 0, 0),
                                               float3(0, 0, 0.9f), float3(0, 0, -0.9f), float3(0, -0.9f, 0)};
                [loop] for (int q = 0; q < 6 && mode == 2u; ++q) {
                    if (nf_occ_point(c + kTry[q]) < 0.5f) { off = kTry[q]; mode = 3u; }
                }
            }
        }
    }

    // This thread's ray of the N over the sphere (a Fibonacci set, turned by
    // this update's random rotation), projected into L1 SH as radiance:
    // c0 += L Y00 dw, c1 += L Y1 d dw.
    float4 sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    if (mode == 3u) {
        const float dw = 4.0f * 3.14159265f / float(kNfRays);
        const float zf = 1.0f - (2.0f * float(k) + 1.0f) / float(kNfRays);
        const float rr = sqrt(saturate(1.0f - zf * zf));
        const float ph = 2.39996323f * float(k);
        const float3 d0 = mul(nf_probe_turn(pi), float3(rr * cos(ph), zf, rr * sin(ph)));
        const float3 d = float3(dot(nf_rot0.xyz, d0), dot(nf_rot1.xyz, d0), dot(nf_rot2.xyz, d0));
        float vis;
        float3 rad;
        nf_ray(c + off, d, vis, rad);
        const float4 basis = float4(kHfY00, kHfY1 * d) * dw;
        sv = vis * basis;
        sr = rad.r * basis;
        sg = rad.g * basis;
        sb = rad.b * basis;
    }
    gs_nf_v[k] = sv;
    gs_nf_r[k] = sr;
    gs_nf_g[k] = sg;
    gs_nf_b[k] = sb;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint h = kNfRays / 2u; h > 0u; h >>= 1u) {
        if (k < h) {
            gs_nf_v[k] += gs_nf_v[k + h];
            gs_nf_r[k] += gs_nf_r[k + h];
            gs_nf_g[k] += gs_nf_g[k + h];
            gs_nf_b[k] += gs_nf_b[k + h];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (k != 0u || mode == 0u) return;
    if (mode == 1u) {
        u_probes[o4 + 0] = g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 1] = g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 2] = g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 3] = g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 4] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    if (mode == 2u) {
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // Blended into what the probe held, DDGI's way, by how often it is traced
    // (nf_keep_*); a probe an edit dirtied starts over (the entry's top bit).
    const bool fresh = (entry & 0x80000000u) != 0u;
    const bool every = (entry & 0x40000000u) != 0u;
    const float keep = (fresh || u_probes[o4 + 4].x < 0.5f) ? 0.0f : every ? nf_keep_box : nf_keep_rr;
    u_probes[o4 + 0] = lerp(gs_nf_v[0], u_probes[o4 + 0], keep);
    u_probes[o4 + 1] = lerp(gs_nf_r[0], u_probes[o4 + 1], keep);
    u_probes[o4 + 2] = lerp(gs_nf_g[0], u_probes[o4 + 2], keep);
    u_probes[o4 + 3] = lerp(gs_nf_b[0], u_probes[o4 + 3], keep);
    u_probes[o4 + 4] = float4(1.0f, off);
}
