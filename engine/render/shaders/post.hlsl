// ---------------------------------------------------------------------------
// The post chain, after the scene has been drawn into two HDR targets --
// the sun's direct light and everything else (scene_out.hlsli) -- and depth.
//
//   cs_ao        half resolution: horizon-based ambient occlusion from depth,
//                normals reconstructed from depth, a rotating pattern per
//                pixel and frame that TAA integrates.
//   cs_composite full resolution: direct x screen-space contact shadow
//                (a short march towards the sun through the depth buffer)
//                + ambient x AO (depth-aware upsample) -> one HDR colour;
//                then the atmosphere (L2): aerial perspective over every
//                surface by its depth, and the sky -- the sky-view table, the
//                sun's and the moon's disks -- wherever nothing was drawn.
//   cs_taa       reproject last frame's result by depth, clip it to this
//                frame's neighbourhood (YCoCg variance box), blend 1:9.
//   cs_hist_*    a 256-bin histogram of log2 luminance of what TAA produced
//   cs_exposure  -> the mean of its 10-90% band, adapted over time like an
//                eye (faster into light than out of it), as an exposure that
//                puts that mean at middle grey (ADR-004 L1).
//   ps_final     exposure, sharpen, then AgX (or a fitted ACES, for A/B) into
//                the back buffer, which is UNORM: the tonemap's output is
//                display-encoded already.
//
// The scene arrives in nits (lighting_units.h). A debug colour does not: its
// ambient alpha is negative (scene_out.hlsli: scene_flat), the composite
// writes alpha 0 for it, TAA carries that through, and ps_final passes it to
// the screen untouched. The meter skips it.
// ---------------------------------------------------------------------------

cbuffer PostConstants : register(b0) {
    row_major float4x4 inv_view_proj;    // this frame, jittered
    row_major float4x4 view_proj;        // this frame, jittered
    row_major float4x4 prev_view_proj;   // last frame, unjittered
    float3 cam_pos;   float time;
    float3 sun_dir;   float near_plane;
    float  far_plane; float sharpen;
    float2 inv_size;
    float2 jitter_ndc;
    uint   width;     uint height;
    uint   hist_cur;  uint flags;        // 1 taa, 2 ao, 4 contact, 8 history valid
    uint   frame;     float ao_radius;
    float  contact_len; float debug;     // 1 ao only, 2 contact only
    float  dt;          float ev_comp;   // seconds since last frame; exposure compensation, stops
    float  ev100_fixed; uint  tonemap;   // a fixed EV100 when > -100, else metered; 0 raw, 1 AgX, 2 ACES
    float  meter_lo;    float meter_hi;  // the metered band, as fractions of the pixels
    uint   expo_flags;  float agx_saturation;   // expo_flags 1: adapt instantly (a still)
    float  agx_power;   float cam_alt_km;   // AgX look; the camera's altitude for the sky table
    float3 sky_sun_dir;  float pad2;         // the sun and moon themselves, for their disks
    float3 sky_moon_dir; float pad3;
    float3 sun_disk;     float pad4;         // disk radiance seen from the camera, nits
    float3 moon_disk;    float pad5;
};

Texture2D<float>  g_depth   : register(t0);
Texture2D<float4> g_direct  : register(t1);
Texture2D<float4> g_ambient : register(t2);
Texture2D<float>  g_ao      : register(t3);
Texture2D<float4> g_scene   : register(t4);
Texture2D<float4> g_hist0   : register(t5);
Texture2D<float4> g_hist1   : register(t6);
Texture2D<float4> g_skyview : register(t7);   // atmosphere.hlsl: the sky round the camera
Texture3D<float4> g_ap      : register(t8);   // atmosphere.hlsl: in-scatter (rgb), transmittance (a)

RWTexture2D<float>  u_ao    : register(u0);
RWTexture2D<float4> u_scene : register(u1);
RWTexture2D<float4> u_hist0 : register(u2);
RWTexture2D<float4> u_hist1 : register(u3);
RWStructuredBuffer<uint>   u_lumhist : register(u4);   // 256 bins
// [0] (adapted log2 nits, exposure, EV100, this frame's target log2 nits)
// [1] (low percentile nits, high percentile nits, pixels metered, 1 once valid)
RWStructuredBuffer<float4> u_expo    : register(u5);

SamplerState g_linear : register(s0);
SamplerState g_point  : register(s1);
SamplerState g_wrap_u : register(s2);   // the sky table wraps round in azimuth

#include "engine/render/atmosphere.h"
#include "engine/render/lighting_units.h"

float ign(float2 p) {
    return frac(52.9829189f * frac(dot(p, float2(0.06711056f, 0.00583715f))));
}

// World position from a pixel and its depth.
float3 world_at(float2 uv, float z) {
    const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    const float4 h = mul(float4(ndc, z, 1.0f), inv_view_proj);
    return h.xyz / h.w;
}

// View distance from a stored depth: D3D's 0..1 with near and far planes.
float linear_depth(float z) {
    return near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
}

// --- ambient occlusion --------------------------------------------------------

float3 normal_at(int2 p, float3 P) {
    // From depth: of the two neighbours on each axis, the closer surface, so
    // a silhouette edge does not tilt the normal across the gap.
    const int2 mx = int2(width - 1, height - 1);
    const float zl = g_depth[clamp(p - int2(1, 0), 0, mx)], zr = g_depth[clamp(p + int2(1, 0), 0, mx)];
    const float zu = g_depth[clamp(p - int2(0, 1), 0, mx)], zd = g_depth[clamp(p + int2(0, 1), 0, mx)];
    const float2 uv = (float2(p) + 0.5f) * inv_size;
    const float3 L = world_at(uv - float2(inv_size.x, 0), zl), Rr = world_at(uv + float2(inv_size.x, 0), zr);
    const float3 U = world_at(uv - float2(0, inv_size.y), zu), D = world_at(uv + float2(0, inv_size.y), zd);
    const float3 dx = abs(length(L - P)) < abs(length(Rr - P)) ? P - L : Rr - P;
    const float3 dy = abs(length(U - P)) < abs(length(D - P)) ? P - U : D - P;
    float3 n = normalize(cross(dy, dx));
    if (dot(n, cam_pos - P) < 0.0f) n = -n;
    return n;
}

[numthreads(8, 8, 1)]
void cs_ao(uint3 id : SV_DispatchThreadID) {
    const uint hw = (width + 1) / 2, hh = (height + 1) / 2;
    if (id.x >= hw || id.y >= hh) return;
    const int2 p = int2(id.xy) * 2;
    const float z = g_depth[min(p, int2(width - 1, height - 1))];
    if (z >= 1.0f || (flags & 2u) == 0u) { u_ao[id.xy] = 1.0f; return; }
    const float2 uv = (float2(p) + 0.5f) * inv_size;
    const float3 P = world_at(uv, z);
    const float3 n = normal_at(p, P);
    const float dist = length(P - cam_pos);
    // The world radius in pixels: the projected size of ao_radius at this
    // distance. Clamped so distant ground does not march across the screen.
    const float focal = 0.5f * float(height) * view_proj[1][1];
    const float r_px = clamp(ao_radius * focal / max(dist, 0.1f), 2.0f, 64.0f);
    const float rot = (ign(float2(id.xy) + float(frame % 16u) * 5.588238f)) * 6.2831853f;
    float occl = 0.0f;
    [unroll] for (int d = 0; d < 4; ++d) {
        const float a = rot + float(d) * 0.785398f * 2.0f;
        const float2 dir = float2(cos(a), sin(a));
        float best = 0.0f;
        [unroll] for (int s = 1; s <= 6; ++s) {
            const float t = (float(s) - 0.5f * ign(float2(id.yx) + float(s))) / 6.0f;
            const float2 q = float2(p) + dir * r_px * t;
            if (any(q < 0.0f) || q.x >= float(width) || q.y >= float(height)) break;
            const float zq = g_depth[int2(q)];
            const float3 S = world_at((q + 0.5f) * inv_size, zq);
            const float3 v = S - P;
            const float dl = length(v);
            if (dl < 1e-3f) continue;
            const float c = dot(v / dl, n);
            const float fall = saturate(1.0f - dl * dl / (ao_radius * ao_radius));
            best = max(best, saturate(c - 0.1f) * fall);
        }
        occl += best;
    }
    u_ao[id.xy] = saturate(1.0f - occl * 0.25f * 1.1f);
}

// --- the atmosphere ---------------------------------------------------------------

// The sky along a direction: the sky-view table, then the sun's disk, limb
// darkened, and the moon's.
float3 sky_at(float3 d) {
    float u, v;
    atm_sky_uv(d, kAtmGround + cam_alt_km, u, v);
    float3 c = g_skyview.SampleLevel(g_wrap_u, float2(u, v), 0).rgb;
    const float r_sun = 0.2667f * 0.0174533f, r_moon = 0.259f * 0.0174533f;
    // The angle to each centre, from the chord: acos is too coarse this
    // close to 1 in single precision.
    const float a_sun = 2.0f * asin(saturate(0.5f * length(d - sky_sun_dir)));
    const float a_moon = 2.0f * asin(saturate(0.5f * length(d - sky_moon_dir)));
    if (a_sun < r_sun) {
        const float t = a_sun / r_sun;
        c += sun_disk * (1.0f - 0.6f * (1.0f - sqrt(max(0.0f, 1.0f - t * t))));
    }
    if (a_moon < r_moon) c += moon_disk;
    return min(c, kSceneMaxNits);
}

// The haze between the camera and a point `dist` metres away along this
// pixel's ray: (in-scattered light, transmittance). The volume's slice k ends
// at far * ((k + 1) / 32)^2 (atmosphere.hlsl); nearer than slice 0 fades
// from nothing.
float4 aerial(float2 uv, float dist) {
    const float w = sqrt(saturate(dist / far_plane)) * 32.0f - 1.0f;
    float4 s = g_ap.SampleLevel(g_linear, float3(uv, (max(w, 0.0f) + 0.5f) / 32.0f), 0);
    if (w < 0.0f) {
        const float f = w + 1.0f;
        s = float4(s.rgb * f, lerp(1.0f, s.a, f));
    }
    return s;
}

// --- composite ----------------------------------------------------------------

float ao_upsample(int2 p, float z) {
    // The four nearest half-resolution samples, weighted by how close each
    // one's depth is to this pixel's: AO does not bleed across a silhouette.
    const float zl = linear_depth(z);
    // The 2x2 of half-resolution samples (at full-res 2q) round this pixel.
    const int2 base = max(p - 1, 0) / 2;
    const int2 mx = int2((width + 1) / 2 - 1, (height + 1) / 2 - 1);
    float acc = 0.0f, wsum = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        const int2 q = clamp(base + int2(k & 1, k >> 1), int2(0, 0), mx);
        const float zq = linear_depth(g_depth[min(q * 2, int2(width - 1, height - 1))]);
        const float w = 1.0f / (1e-3f + abs(zq - zl) / max(zl, 1.0f));
        acc += g_ao[q] * w;
        wsum += w;
    }
    return acc / wsum;
}

float contact_shadow(int2 p, float z, float3 P) {
    const float dist = length(P - cam_pos);
    if (dist > 80.0f) return 1.0f;
    const float3 L = normalize(sun_dir);
    const int steps = 14;
    const float len = contact_len * (1.0f + dist / 40.0f);
    const float j = ign(float2(p) + float(frame % 16u) * 5.588238f);
    float lit = 1.0f;
    [loop] for (int s = 1; s <= steps; ++s) {
        const float t = (float(s) - j) / float(steps) * len;
        const float3 Q = P + L * (t + 0.02f);
        const float4 c = mul(float4(Q, 1.0f), view_proj);
        if (c.w <= 0.0f) break;
        const float2 ndc = c.xy / c.w;
        const float2 uv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
        if (any(uv < 0.0f) || any(uv > 1.0f)) break;
        const float scene_z = linear_depth(g_depth.SampleLevel(g_point, uv, 0));
        const float ray_z = c.w;
        const float diff = ray_z - scene_z;
        // In front of the ray by a little, not by a lot: a thin occluder
        // like a blade or a branch, not the far side of a hill behind it.
        if (diff > 0.02f && diff < 0.2f + 0.005f * dist) { lit = 0.0f; break; }
    }
    // Fade out with distance, where the march is too coarse to trust.
    return lerp(lit, 1.0f, saturate((dist - 60.0f) / 20.0f));
}

[numthreads(8, 8, 1)]
void cs_composite(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const float z = g_depth[p];
    const float3 dir = g_direct[p].rgb;
    const float3 amb = g_ambient[p].rgb;
    // A debug colour (scene_flat): as is, marked for the final pass.
    if (g_ambient[p].a < -0.5f) { u_scene[p] = float4(amb, 0.0f); return; }
    const float2 uv = (float2(p) + 0.5f) * inv_size;
    if (z >= 1.0f) {
        // Nothing with depth: the sky. The sea beyond the clipmap draws
        // without depth, blending its colour into the direct target and
        // taking its share out of the ambient one, which was cleared to the
        // marker -- so what is left of the marker is how much sky shows.
        const float3 d = normalize(world_at(uv, 1.0f) - cam_pos);
        const float vis = saturate(amb.g / kSkyClearG);
        const float4 apf = aerial(uv, far_plane);
        u_scene[p] = float4(dir * apf.a + apf.rgb * (1.0f - vis) + sky_at(d) * vis, 1.0f);
        return;
    }
    const float3 P = world_at(uv, z);
    const float aow = saturate(g_ambient[p].a);
    const float ao = (flags & 2u) != 0u ? lerp(1.0f, ao_upsample(p, z), aow) : 1.0f;
    const float cmask = g_direct[p].a;
    const float cs = ((flags & 4u) != 0u && cmask > 0.01f && dot(dir, dir) > 1e-6f)
                   ? lerp(1.0f, contact_shadow(p, z, P), 0.7f * saturate(cmask)) : 1.0f;   // never fully black: sky light still reaches a crease
    float3 c = dir * cs + amb * ao;
    // The atmosphere between the eye and the surface.
    const float4 ap = aerial(uv, length(P - cam_pos));
    c = c * ap.a + ap.rgb;
    float mark = 1.0f;
    if (debug > 0.5f && debug < 1.5f) { c = ao.xxx; mark = 0.0f; }
    if (debug > 1.5f && debug < 2.5f) { c = cs.xxx; mark = 0.0f; }
    u_scene[p] = float4(c, mark);
}

// --- TAA ----------------------------------------------------------------------

float3 to_ycocg(float3 c) {
    return float3(0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b, -0.25f * c.r + 0.5f * c.g - 0.25f * c.b);
}
float3 from_ycocg(float3 y) {
    return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z);
}

[numthreads(8, 8, 1)]
void cs_taa(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height) return;
    const int2 p = int2(id.xy);
    const float4 cur4 = g_scene[p];
    const float3 cur = cur4.rgb;
    float3 outc = cur;
    if ((flags & 1u) != 0u && (flags & 8u) != 0u) {
        // Where this pixel's surface was last frame.
        const float z = g_depth[p];
        const float2 uv = (float2(p) + 0.5f) * inv_size;
        const float3 P = world_at(uv, z >= 1.0f ? 0.99999f : z);
        const float4 pc = mul(float4(P, 1.0f), prev_view_proj);
        const float2 pndc = pc.xy / pc.w;
        const float2 puv = float2(pndc.x * 0.5f + 0.5f, 0.5f - pndc.y * 0.5f);
        if (all(puv >= 0.0f) && all(puv <= 1.0f) && pc.w > 0.0f) {
            // This frame's 3x3 neighbourhood, as a mean and spread in YCoCg.
            float3 m1 = 0.0f, m2 = 0.0f;
            [unroll] for (int j = -1; j <= 1; ++j) {
                [unroll] for (int i = -1; i <= 1; ++i) {
                    const float3 s = to_ycocg(g_scene[clamp(p + int2(i, j), int2(0, 0), int2(width - 1, height - 1))].rgb);
                    m1 += s; m2 += s * s;
                }
            }
            m1 /= 9.0f; m2 /= 9.0f;
            const float3 sd = sqrt(max(m2 - m1 * m1, 0.0f));
            const float3 lo = m1 - 1.25f * sd, hi = m1 + 1.25f * sd;
            const float3 hist = hist_cur == 0u ? g_hist1.SampleLevel(g_linear, puv, 0).rgb
                                               : g_hist0.SampleLevel(g_linear, puv, 0).rgb;
            // Clip the history towards the box's centre rather than clamping
            // per channel: a clamped colour can be one the scene never had.
            float3 hy = to_ycocg(hist);
            const float3 cdir = hy - m1;
            const float3 ext = max(hi - m1, 1e-4f);
            const float3 a = abs(cdir / ext);
            const float ma = max(a.x, max(a.y, a.z));
            if (ma > 1.0f) hy = m1 + cdir / ma;
            outc = lerp(from_ycocg(hy), cur, 0.1f);
        }
    }
    // Alpha carries the debug mark (0) through to the final pass.
    if (hist_cur == 0u) u_hist0[p] = float4(outc, cur4.a);
    else u_hist1[p] = float4(outc, cur4.a);
}

// --- exposure -------------------------------------------------------------------
//
// Metered on what TAA produced (what will be shown), every second pixel each
// way. log2 luminance over [-10, 18] in 256 bins, about a ninth of a stop
// each: 0.001 nits, a moonless cave, to 262,000, past a glint off the sea.

static const float kLumLogMin = -10.0f;
static const float kLumLogMax = 18.0f;

float4 metered(int2 p) {
    p = clamp(p, int2(0, 0), int2(width - 1, height - 1));
    if ((flags & 1u) == 0u) return g_scene[p];
    return hist_cur == 0u ? g_hist0[p] : g_hist1[p];
}

[numthreads(256, 1, 1)]
void cs_hist_clear(uint3 id : SV_DispatchThreadID) {
    u_lumhist[id.x] = 0u;
}

groupshared uint gs_bins[256];

[numthreads(16, 16, 1)]
void cs_histogram(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gs_bins[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();
    const int2 p = int2(id.xy) * 2;
    if (p.x < int(width) && p.y < int(height)) {
        const float4 c = metered(p);
        if (c.a > 0.5f) {
            const float lum = dot(max(c.rgb, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
            const float t = (log2(max(lum, 1e-6f)) - kLumLogMin) / (kLumLogMax - kLumLogMin);
            InterlockedAdd(gs_bins[uint(saturate(t) * 255.0f + 0.5f)], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (gs_bins[gi] > 0u) InterlockedAdd(u_lumhist[gi], gs_bins[gi]);
}

float bin_log2(uint b) {
    return kLumLogMin + float(b) / 255.0f * (kLumLogMax - kLumLogMin);
}

[numthreads(256, 1, 1)]
void cs_exposure(uint3 id : SV_DispatchThreadID) {
    gs_bins[id.x] = u_lumhist[id.x];
    GroupMemoryBarrierWithGroupSync();
    if (id.x != 0u) return;
    uint total = 0u;
    for (uint b = 0u; b < 256u; ++b) total += gs_bins[b];
    const float4 prev = u_expo[0];
    const bool valid = u_expo[1].w > 0.5f;
    // The mean log luminance of the band between the two percentiles: the
    // darkest tenth (deep shade, a cave mouth) and the brightest tenth (sky
    // near the sun, a glint) do not pull the exposure around.
    const float lo = float(total) * meter_lo, hi = float(total) * meter_hi;
    float acc = 0.0f, wsum = 0.0f, cum = 0.0f;
    float lo_log = kLumLogMin, hi_log = kLumLogMax;
    bool lo_found = false, hi_found = false;
    for (uint k = 0u; k < 256u; ++k) {
        const float c = float(gs_bins[k]);
        const float a = max(cum, lo), e = min(cum + c, hi);
        if (e > a) { acc += (e - a) * bin_log2(k); wsum += e - a; }
        if (!lo_found && cum + c >= lo && c > 0.0f) { lo_log = bin_log2(k); lo_found = true; }
        if (!hi_found && cum + c >= hi && c > 0.0f) { hi_log = bin_log2(k); hi_found = true; }
        cum += c;
    }
    // EV100 -2 (a dim interior) to 18 (snow in full sun), as log2 nits:
    // EV100 = log2(L * 100 / 12.5), the reflected-light meter's calibration.
    float target = wsum > 0.0f ? acc / wsum : (valid ? prev.w : 10.0f);
    target = clamp(target, -5.0f, 15.0f);
    float adapted = target;
    if (valid && (expo_flags & 1u) == 0u) {
        // Into the light in about a second, out of it in about three: the eye
        // closes faster than it opens.
        const float rate = target > prev.x ? 3.0f : 1.0f;
        adapted = prev.x + (target - prev.x) * (1.0f - exp2(-dt * rate));
    }
    float ev100 = adapted + 3.0f;
    if (ev100_fixed > -100.0f) { ev100 = ev100_fixed; adapted = ev100 - 3.0f; }
    // The metered mean lands on middle grey, 0.18, before the tonemap.
    const float exposure = 0.18f * exp2(ev_comp) / exp2(adapted);
    u_expo[0] = float4(adapted, exposure, ev100, target);
    u_expo[1] = float4(exp2(lo_log), exp2(hi_log), float(total), 1.0f);
}

// --- final --------------------------------------------------------------------

struct FsOut { float4 pos : SV_Position; };

FsOut vs_full(uint vid : SV_VertexID) {
    FsOut o;
    const float2 t = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(t * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

float3 fetch(int2 p) {
    return metered(p).rgb;
}

// The pre-L1 display path, kept as --tonemap 0 for debug views that write
// display values (the sea's --water-debug): no exposure, identity to 0.8,
// then an exponential shoulder towards 1.
float3 tonemap_raw(float3 x) {
    const float k = 0.8f;
    return x <= k ? x : k + (1.0f - k) * (1.0f - exp(-(x - k) / (1.0f - k)));
}

// AgX (Troy Sobotka), with Benjamin Wrensch's polynomial fit of the default
// contrast curve. Linear sRGB in; display-encoded sRGB out, which is what a
// UNORM back buffer wants. Its virtue over a per-channel curve is that a
// bright saturated colour desaturates towards white as it brightens instead
// of skewing hue -- a sunset sky stays orange rather than going yellow.
float3 agx_contrast(float3 x) {
    const float3 x2 = x * x;
    const float3 x4 = x2 * x2;
    return 15.5f * x4 * x2 - 40.14f * x4 * x + 31.96f * x4 - 6.868f * x2 * x + 0.4298f * x2 + 0.1191f * x - 0.00232f;
}

float3 agx(float3 c) {
    // Rows here are the GLSL reference's columns, and mul(v, M) is its M * v.
    const float3x3 inset = float3x3(0.842479062253094f, 0.0423282422610123f, 0.0423756549057051f,
                                    0.0784335999999992f, 0.878468636469772f, 0.0784336f,
                                    0.0792237451477643f, 0.0791661274605434f, 0.879142973793104f);
    const float3x3 outset = float3x3(1.19687900512017f, -0.0528968517574562f, -0.0529716355144438f,
                                     -0.0980208811401368f, 1.15190312990417f, -0.0980434501171241f,
                                     -0.0990297440797205f, -0.0989611768448433f, 1.15107367264116f);
    const float min_ev = -12.47393f, max_ev = 4.026069f;
    c = mul(max(c, 0.0f), inset);
    c = (clamp(log2(max(c, 1e-10f)), min_ev, max_ev) - min_ev) / (max_ev - min_ev);
    c = agx_contrast(c);
    // The look (the reference's agxLook, slope 1): AgX's base output is
    // deliberately flat. A power for contrast, then saturation about the
    // luminance. The default is the reference "punchy" look, power 1.35 and
    // saturation 1.4 (--agx-power, --agx-sat): base AgX drew every view
    // pastel, and punchy matched a fitted ACES on the four A/B stills while
    // keeping AgX's hue handling for what is coming -- a torch's orange and
    // a spell's blue stay themselves as they brighten, where ACES skews them.
    c = pow(max(c, 0.0f), agx_power);
    const float l = dot(c, float3(0.2126f, 0.7152f, 0.0722f));
    c = l + agx_saturation * (c - l);
    return saturate(mul(c, outset));
}

// Narkowicz's fit of the ACES filmic curve, for A/B. Linear in and out.
float3 aces_fitted(float3 x) {
    x *= 0.6f;
    return saturate(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f));
}

float3 linear_to_srgb(float3 c) {
    c = saturate(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * pow(c, 1.0f / 2.4f) - 0.055f;
}

float4 ps_final(FsOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    const float4 c4 = metered(p);
    // A debug colour: straight to the screen.
    if (c4.a < 0.5f) return float4(saturate(c4.rgb), 1.0f);
    const float3 c = c4.rgb;
    float3 o = c;
    if (sharpen > 0.0f) {
        // A contrast-adaptive cross sharpen: pull away from the neighbours'
        // mean, limited to the neighbourhood's own range so it cannot ring.
        const float3 n = fetch(p + int2(1, 0)), s = fetch(p - int2(1, 0));
        const float3 e = fetch(p + int2(0, 1)), w = fetch(p - int2(0, 1));
        const float3 mn = min(c, min(min(n, s), min(e, w)));
        const float3 mx = max(c, max(max(n, s), max(e, w)));
        const float3 mean = 0.25f * (n + s + e + w);
        o = clamp(c + (c - mean) * sharpen, mn, mx);
    }
    o = max(o, 0.0f);
    if (tonemap == 0u) return float4(tonemap_raw(o), 1.0f);
    o *= u_expo[0].y;
    if (tonemap == 2u) return float4(linear_to_srgb(aces_fitted(o)), 1.0f);
    return float4(agx(o), 1.0f);
}
