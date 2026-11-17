#ifndef SCAN_HLSLI
#define SCAN_HLSLI
// ---------------------------------------------------------------------------
// The CC0 scans, sampled the same way on everything that wears them: the
// terrain (mesh.hlsl) and the rocks (rocks.hlsl). Needs the shadow constants
// (shadow.hlsli: l_shadow_frame.y is the parallax switch) and kSunDir before
// it is included.
// ---------------------------------------------------------------------------
// The CC0 scans (scan_library.h): A sRGB albedo + height, B normal xy,
// roughness, occlusion; one layer per set. And per biome, which layers its
// ground and rock take and how to retint them.
struct ScanMaterial {
    float ground_layer, rock_layer, ground_inv_tile, rock_inv_tile;
    float3 ground_tint; float ground_rough;
    float3 rock_tint;   float rock_rough;
    float ground_scan_rough, rock_scan_rough;
    float ground_depth, rock_depth;   // parallax depth, metres (pom.hlsli)
};
Texture2DArray<float4>         g_scan_a    : register(t44);
Texture2DArray<float4>         g_scan_b    : register(t45);
StructuredBuffer<ScanMaterial> g_scan_mat  : register(t46);
SamplerState                   g_scan_samp : register(s5);   // anisotropic, wrap
#include "engine/render/shaders/pom.hlsli"

// ---------------------------------------------------------------------------
// The scans. Triplanar, because the voxel mesh has no uvs: three planar
// projections weighted by the normal, each plane's tangent normal folded
// into world space by the whiteout blend, with the u axis mirrored on the
// back faces so a scan is not reversed on them (Golus, Normal Mapping for a
// Triplanar Shader). A plane under 1% weight is not sampled.
// ---------------------------------------------------------------------------
struct ScanSample {
    float3 albedo;   // linear, before the tint
    float  height;
    float3 n;        // world, unnormalised sum
    float  rough;
    float  ao;
};

ScanSample scan_triplanar(float layer, float inv_tile, float3 p, float3 n, float3 bw, float3 dpx, float3 dpy) {
    ScanSample o = (ScanSample)0;
    const float3 sg = float3(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f, n.z < 0.0f ? -1.0f : 1.0f);
    float3 nsum = 0.0f;
    [unroll] for (int k = 0; k < 3; ++k) {
        const float w = bw[k];
        if (w < 0.01f) continue;
        float2 uv = k == 0 ? p.zy : (k == 1 ? p.xz : p.xy);
        float2 gx = k == 0 ? dpx.zy : (k == 1 ? dpx.xz : dpx.xy);
        float2 gy = k == 0 ? dpy.zy : (k == 1 ? dpy.xz : dpy.xy);
        const float flip = k == 0 ? sg.x : (k == 1 ? sg.y : -sg.z);
        uv.x *= flip;
        gx.x *= flip;
        gy.x *= flip;
        uv *= inv_tile;
        gx *= inv_tile;
        gy *= inv_tile;
        // Explicit gradients: the parallax shift is discontinuous, and its
        // derivatives would pick wild mips along every step it takes.
        const float4 a = g_scan_a.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
        const float4 b = g_scan_b.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
        float2 t = b.xy * 2.0f - 1.0f;
        t.x *= flip;
        const float tz = sqrt(saturate(1.0f - dot(t, t)));
        float3 wn;
        if (k == 0)      { const float3 q = float3(t + n.zy, tz * abs(n.x)); wn = float3(q.z * sg.x, q.y, q.x); }
        else if (k == 1) { const float3 q = float3(t + n.xz, tz * abs(n.y)); wn = float3(q.x, q.z * sg.y, q.y); }
        else             { const float3 q = float3(t + n.xy, tz * abs(n.z)); wn = float3(q.x, q.y, q.z * sg.z); }
        o.albedo += w * a.rgb;
        o.height += w * a.a;
        nsum += w * wn;
        o.rough += w * b.z;
        o.ao += w * b.w;
    }
    o.n = nsum;
    return o;
}

// Parallax in the triplanar plane the normal faces most (pom.hlsli): the
// world-space shift to move a sample point by, and the self-shadow towards
// the key light in `psh`. Zero shift where parallax is off, out of reach, or
// no plane clearly dominates -- where two weigh alike the dominant one flips
// from pixel to pixel and the shift with it, which drew a dark seam along a
// cliff's crease.
float3 scan_pom_shift(float layer, float inv_tile, float depth_m, float3 p, float3 n, float3 v, float3 bw,
                      float3 dpx, float3 dpy, float dist, out float psh) {
    psh = 1.0f;
    if (l_shadow_frame.y < 0.5f || dist >= 45.0f || layer < 0.0f) return float3(0.0f, 0.0f, 0.0f);
    const float dom = saturate((max(bw.x, max(bw.y, bw.z)) - 0.55f) / 0.3f);
    const float depth = depth_m * pom_fade(dist, depth_m) * dom;
    if (depth <= 0.0005f) return float3(0.0f, 0.0f, 0.0f);
    const float3 sg = float3(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f, n.z < 0.0f ? -1.0f : 1.0f);
    float3 U, V, A;
    float2 uv;
    if (bw.y >= bw.x && bw.y >= bw.z) {
        U = float3(sg.y, 0.0f, 0.0f); V = float3(0.0f, 0.0f, 1.0f); A = float3(0.0f, sg.y, 0.0f);
        uv = float2(p.x * sg.y, p.z);
    } else if (bw.x >= bw.z) {
        U = float3(0.0f, 0.0f, sg.x); V = float3(0.0f, 1.0f, 0.0f); A = float3(sg.x, 0.0f, 0.0f);
        uv = float2(p.z * sg.x, p.y);
    } else {
        U = float3(-sg.z, 0.0f, 0.0f); V = float3(0.0f, 1.0f, 0.0f); A = float3(0.0f, 0.0f, sg.z);
        uv = float2(-p.x * sg.z, p.y);
    }
    uv *= inv_tile;
    const float2 gx = float2(dot(dpx, U), dot(dpx, V)) * inv_tile;
    const float2 gy = float2(dot(dpy, U), dot(dpy, V)) * inv_tile;
    const float scale = depth * inv_tile;
    const float vz = max(dot(v, A), 0.2f);
    const float2 view_step = -float2(dot(v, U), dot(v, V)) / vz * scale;
    const int steps = int(lerp(6.0f, 20.0f, saturate(1.0f - dot(v, A))));
    const float th = pom_march(uv, layer, view_step, gx, gy, steps);
    const float lz = dot(kSunDir, A);
    if (lz > 0.05f)
        psh = pom_shadow_faded(uv + view_step * th, th, layer,
                               float2(dot(kSunDir, U), dot(kSunDir, V)) / lz * scale, lz, gx, gy);
    return (U * view_step.x + V * view_step.y) * (th / inv_tile);
}

#endif
