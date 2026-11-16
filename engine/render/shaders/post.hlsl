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
    row_major float4x4 prev_view_proj;   // TAA: this frame's UNJITTERED clip space to last frame's (composed in double)
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
    float3 sky_sun_dir;  float ssgi_radius;     // the sun and moon themselves, for their disks; the bounce's reach, m (0 off)
    float3 sky_moon_dir; float ssgi_thickness;  // the thickness assumed behind each depth sample, m
    float3 sun_disk;     float cam_sky;      // disk radiance seen from the camera, nits; cam_sky: DDGI's share of open sky at the eye
    float3 moon_disk;    float pad5;
    float3 cam_fwd;      float fog_on;       // the mist (L4): fog.hlsl's integrated volume
    float  fog_near;     float fog_log_ratio; float rt_shadows; float rt_sky;   // L5's traced key-light visibility, L6's traced sky
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
Texture3D<float4> g_fog     : register(t9);   // fog.hlsl: in-scatter (rgb), transmittance (a), to each slice's far side
Texture2D<float2> g_rtvis   : register(t10);  // rt_shadow.hlsl: x the key light's visibility, y the sky's, traced
Texture2D<float4> g_ssgi_view : register(t11); // cs_ssgi's output, for --post-debug 3 (UAV-bound in cs_ssgi itself)

RWTexture2D<float>  u_ao    : register(u0);
RWTexture2D<float4> u_scene : register(u1);
RWTexture2D<float4> u_hist0 : register(u2);
RWTexture2D<float4> u_hist1 : register(u3);
RWTexture2D<float4> u_ssgi  : register(u6);   // cs_ssgi: irradiance (rgb, lux), distance from the camera (a)
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

// --- screen-space indirect light (ADR-004 L6, the no-RT GI) ------------------
//
// The irradiance a surface gets from the surfaces round it that are on
// screen: per half-resolution pixel, three slices, each swept both ways in
// screen space, every depth sample a thick occluder whose angular extent in
// the slice sets bits in a 32-sector visibility mask (Therrien et al. 2023).
// The light a sample sends is what this frame's two scene targets say leaves
// it -- the sun's direct light and everything else, the bounce included --
// and it counts only for the sectors it is the first to cover, weighted by
// the cosine at the receiver. The scene reads the result NEXT frame through
// gi_diffuse (ssgi.hlsli), reprojected; that frame's ambient then carries it
// into this pass again, which is every further bounce, a frame at a time.

[numthreads(8, 8, 1)]
void cs_ssgi(uint3 id : SV_DispatchThreadID) {
    const uint hw = (width + 1) / 2, hh = (height + 1) / 2;
    if (id.x >= hw || id.y >= hh) return;
    const int2 mx = int2(width - 1, height - 1);
    const int2 p = min(int2(id.xy) * 2, mx);
    const float z = g_depth[p];
    if (z >= 1.0f || ssgi_radius <= 0.0f) { u_ssgi[id.xy] = float4(0.0f, 0.0f, 0.0f, 0.0f); return; }
    const float2 uv = (float2(p) + 0.5f) * inv_size;
    const float3 P = world_at(uv, z);
    const float3 n = normal_at(p, P);
    const float dist = length(P - cam_pos);
    const float3 V = (cam_pos - P) / max(dist, 1e-4f);
    const float focal = 0.5f * float(height) * view_proj[1][1];
    const float r_px = clamp(ssgi_radius * focal / max(dist, 0.1f), 4.0f, 256.0f);
    const float thick = ssgi_thickness + 0.02f * dist;
    const float noise = ign(float2(id.xy) + float(frame % 32u) * 5.588238f);
    const float kPi_ = 3.14159265f, kHalfPi = 1.5707963f;
    float3 lsum = float3(0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    [loop] for (int k = 0; k < 3; ++k) {
        const float a = (noise + float(k)) * (kPi_ / 3.0f);
        const float2 d = float2(cos(a), sin(a));   // pixels: x right, y down
        // The slice in the world: a step along d on screen at P's depth,
        // made perpendicular to the view, is its tangent.
        float3 T = world_at(uv + d * inv_size * 4.0f, z) - P;
        T -= V * dot(T, V);
        const float tl = length(T);
        if (tl < 1e-6f) continue;
        T /= tl;
        const float3 A = cross(T, V);
        float3 np = n - A * dot(n, A);
        const float nl = length(np);
        if (nl < 1e-4f) continue;
        np /= nl;
        const float g = atan2(dot(np, T), dot(np, V));   // the projected normal, from the view direction
        uint mask = 0u;
        float3 ls = float3(0.0f, 0.0f, 0.0f);
        [unroll] for (int side = 0; side < 2; ++side) {
            const float sgn = side == 0 ? 1.0f : -1.0f;
            [loop] for (int j = 0; j < 8; ++j) {
                // Denser near the pixel, where occluders matter most.
                const float t = (float(j) + frac(noise + 0.618034f * float(j + 1))) / 8.0f;
                const float2 q = float2(p) + 0.5f + sgn * d * (r_px * t * t + 1.0f);
                if (any(q < 0.0f) || q.x >= float(width) || q.y >= float(height)) break;
                const int2 qi = int2(q);
                const float zq = g_depth[qi];
                if (zq >= 1.0f) continue;   // sky: the fallback's sky term already has it
                const float3 S = world_at((float2(qi) + 0.5f) * inv_size, zq);
                const float3 dv = S - P;
                const float dl = length(dv);
                if (dl < 0.02f || dl > ssgi_radius) continue;
                // The occluder's front and, a thickness behind it, its back.
                // Thicker with distance: eight samples a side over 30 m leave
                // gaps, and a sliver 0.75 m thick marked a sunlit slope across
                // a valley as a few thin sectors -- the bounce came out a
                // fraction of DDGI's. Ground is solid; most of what is out
                // there is thick.
                const float3 db = dv - V * max(thick, 0.35f * dl);
                const float af = atan2(dot(dv, T), dot(dv, V));
                const float ab = atan2(dot(db, T), dot(db, V));
                const float lo = clamp(min(af, ab) - g, -kHalfPi, kHalfPi);
                const float hi = clamp(max(af, ab) - g, -kHalfPi, kHalfPi);
                const uint b0 = min(31u, uint((lo / kPi_ + 0.5f) * 32.0f));
                const uint b1 = min(32u, uint(ceil((hi / kPi_ + 0.5f) * 32.0f)));
                if (b1 <= b0) continue;
                const uint nb = b1 - b0;
                const uint bits = (nb >= 32u ? 0xFFFFFFFFu : ((1u << nb) - 1u)) << b0;
                const uint fresh = bits & ~mask;
                if (fresh == 0u) continue;
                // The cosine-weighted share of the hemisphere the interval
                // covers, times the part of it nothing nearer already took.
                const float w = 0.5f * (sin(hi) - sin(lo)) * float(countbits(fresh)) / float(nb);
                ls += (g_direct[qi].rgb + g_ambient[qi].rgb) * w;
                mask |= bits;
            }
        }
        lsum += ls * nl;
        wsum += nl;
    }
    const float3 E = wsum > 0.0f ? kPi_ * lsum / wsum : float3(0.0f, 0.0f, 0.0f);
    u_ssgi[id.xy] = float4(min(E, float3(60000.0f, 60000.0f, 60000.0f)), dist);
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

// The mist between the eye and view depth z (L4). Slice s of the volume holds
// what has accumulated by the FAR side of the slice, view depth
// near * e^((s + 1) / 128 * ln(far / near)); nearer than slice 0's far side
// fades from nothing.
float4 mist(float2 uv, float z) {
    if (fog_on < 0.5f) return float4(0.0f, 0.0f, 0.0f, 1.0f);
    const float n = 128.0f;
    const float z1 = fog_near * exp(fog_log_ratio / n);
    if (z <= z1) {
        const float4 s0 = g_fog.SampleLevel(g_linear, float3(uv, 0.5f / n), 0);
        const float f = saturate(z / z1);
        return float4(s0.rgb * f, lerp(1.0f, s0.a, f));
    }
    const float w = log(z / fog_near) / fog_log_ratio * n;
    return g_fog.SampleLevel(g_linear, float3(uv, saturate((w - 0.5f) / n)), 0);
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
        float3 c = dir * apf.a + apf.rgb * (1.0f - vis) + sky_at(d) * vis;
        // The whole depth of the mist: it hides the horizon, as it should.
        const float4 mf = mist(uv, 1e9f);
        u_scene[p] = float4(c * mf.a + mf.rgb, 1.0f);
        return;
    }
    const float3 P = world_at(uv, z);
    const float aow = saturate(g_ambient[p].a);
    const float ao = (flags & 2u) != 0u ? lerp(1.0f, ao_upsample(p, z), aow) : 1.0f;
    const float cmask = g_direct[p].a;
    const float cs = ((flags & 4u) != 0u && cmask > 0.01f && dot(dir, dir) > 1e-6f)
                   ? lerp(1.0f, contact_shadow(p, z, P), 0.7f * saturate(cmask)) : 1.0f;   // never fully black: sky light still reaches a crease
    // Under --rt-shadows the surfaces wrote their direct light unshadowed
    // (shadow.hlsli) and the traced visibility takes the cascades' place.
    const float rtv = rt_shadows > 0.5f ? g_rtvis[p].x : 1.0f;
    // Under --rt-sky (L6) the ambient takes the traced sky visibility --
    // terrain, trunks and the crowns' volumes -- which is the shade under a
    // canopy. It scales all of the ambient target, so a local light under a
    // tree is dimmed with the sky until DDGI separates them.
    const float skv = rt_sky > 0.5f ? g_rtvis[p].y : 1.0f;
    float3 c = dir * (cs * rtv) + amb * (ao * skv);
    // The atmosphere between the eye and the surface.
    // The haze assumes sunlit, sky-lit air the whole way. Near the camera
    // the air is lit as the camera is -- in a sealed chamber, not at all --
    // so its in-scattering takes the probes' share of open sky at the eye
    // over the first 64 m, then fades back to the full haze by 256 m, where
    // the air is somewhere else. (A fade starting at the eye left 6% of the
    // haze 4 m into a sealed chamber: 0.2 nits, the whole residue.)
    const float dist = length(P - cam_pos);
    const float4 ap = aerial(uv, dist);
    c = c * ap.a + ap.rgb * lerp(cam_sky, 1.0f, saturate((dist - 64.0f) / 192.0f));
    // Then the mist, which is nearer than the bulk of the air.
    const float4 mf = mist(uv, dot(P - cam_pos, cam_fwd));
    c = c * mf.a + mf.rgb;
    float mark = 1.0f;
    if (debug > 0.5f && debug < 1.5f) { c = ao.xxx; mark = 0.0f; }
    if (debug > 1.5f && debug < 2.5f) { c = cs.xxx; mark = 0.0f; }
    // The screen-space bounce's irradiance, as the radiance a white
    // Lambertian surface would send back: exposed like the scene.
    if (debug > 2.5f && debug < 3.5f) c = g_ssgi_view[min(p / 2, int2((width + 1) / 2 - 1, (height + 1) / 2 - 1))].rgb / 3.14159265f;
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
        // Where this pixel's surface was last frame -- for the UNJITTERED
        // pixel, straight from clip space to clip space. It went through
        // world space with the JITTERED inverse once: a still camera then
        // fetched history from uv - jitter, and every frame the accumulated
        // image moved by that frame's jitter, a shake of up to a pixel left
        // and right (--shake: 1.0 px between frames, more than the jitter).
        const float z = g_depth[p];
        const float2 uv = (float2(p) + 0.5f) * inv_size;
        const float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
        const float4 pc = mul(float4(ndc, z >= 1.0f ? 0.99999f : z, 1.0f), prev_view_proj);
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
    // EV100 -1 to 18 (snow in full sun), as log2 nits: EV100 = log2(L * 100 /
    // 12.5), the reflected-light meter's calibration. The floor is where the
    // eye stops compensating, and it decides what night looks like: at EV100
    // -2 a full-moon meadow metered all the way up to a dim blue day. At -1
    // moonlight renders a stop under middle grey (0 was tried: two stops read
    // as near black) -- ambience -- and a
    // torch or a fire, far above the scene's mean, carries the picture.
    float target = wsum > 0.0f ? acc / wsum : (valid ? prev.w : 10.0f);
    target = clamp(target, -4.0f, 15.0f);
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
