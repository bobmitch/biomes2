// ---------------------------------------------------------------------------
// The post chain, after the scene has been drawn into two HDR targets --
// the sun's direct light and everything else (scene_out.hlsli) -- and depth.
//
//   cs_ao        half resolution: horizon-based ambient occlusion from depth,
//                normals reconstructed from depth, a rotating pattern per
//                pixel and frame that TAA integrates.
//   cs_composite full resolution: direct x screen-space contact shadow
//                (a short march towards the sun through the depth buffer)
//                + ambient x AO (depth-aware upsample) -> one HDR colour.
//   cs_taa       reproject last frame's result by depth, clip it to this
//                frame's neighbourhood (YCoCg variance box), blend 1:9.
//   ps_final     sharpen, then a tonemap that is the IDENTITY below 0.8 and
//                rolls off above: the sky's clear colour passes through bit
//                for bit, which keeps the external crack detector (sky colour
//                below the horizon = a hole) meaning what it says.
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
};

Texture2D<float>  g_depth   : register(t0);
Texture2D<float4> g_direct  : register(t1);
Texture2D<float4> g_ambient : register(t2);
Texture2D<float>  g_ao      : register(t3);
Texture2D<float4> g_scene   : register(t4);
Texture2D<float4> g_hist0   : register(t5);
Texture2D<float4> g_hist1   : register(t6);

RWTexture2D<float>  u_ao    : register(u0);
RWTexture2D<float4> u_scene : register(u1);
RWTexture2D<float4> u_hist0 : register(u2);
RWTexture2D<float4> u_hist1 : register(u3);

SamplerState g_linear : register(s0);
SamplerState g_point  : register(s1);

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
    if (z >= 1.0f) { u_scene[p] = float4(dir + amb, 1.0f); return; }
    const float2 uv = (float2(p) + 0.5f) * inv_size;
    const float3 P = world_at(uv, z);
    const float aow = saturate(g_ambient[p].a);
    const float ao = (flags & 2u) != 0u ? lerp(1.0f, ao_upsample(p, z), aow) : 1.0f;
    const float cmask = g_direct[p].a;
    const float cs = ((flags & 4u) != 0u && cmask > 0.01f && dot(dir, dir) > 1e-6f)
                   ? lerp(1.0f, contact_shadow(p, z, P), 0.7f * saturate(cmask)) : 1.0f;   // never fully black: sky light still reaches a crease
    float3 c = dir * cs + amb * ao;
    if (debug > 0.5f && debug < 1.5f) c = ao.xxx;
    if (debug > 1.5f && debug < 2.5f) c = cs.xxx;
    u_scene[p] = float4(c, 1.0f);
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
    const float3 cur = g_scene[p].rgb;
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
    if (hist_cur == 0u) u_hist0[p] = float4(outc, 1.0f);
    else u_hist1[p] = float4(outc, 1.0f);
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
    p = clamp(p, int2(0, 0), int2(width - 1, height - 1));
    if ((flags & 1u) == 0u) return g_scene[p].rgb;
    return hist_cur == 0u ? g_hist0[p].rgb : g_hist1[p].rgb;
}

float3 tonemap(float3 x) {
    // Identity to 0.8, then an exponential shoulder towards 1. Everything the
    // old LDR image showed is unchanged; only highlights that would have
    // clipped are compressed.
    const float k = 0.8f;
    return x <= k ? x : k + (1.0f - k) * (1.0f - exp(-(x - k) / (1.0f - k)));
}

float4 ps_final(FsOut i) : SV_Target {
    const int2 p = int2(i.pos.xy);
    const float3 c = fetch(p);
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
    return float4(tonemap(max(o, 0.0f)), 1.0f);
}
