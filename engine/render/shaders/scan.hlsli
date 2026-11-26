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
// ---------------------------------------------------------------------------
// Hex-tiling (Mikkelsen, "Practical Real-Time Hex-Tiling", JCGT 11(2), 2022).
// A scan repeated on one square lattice shows its blotches as a grid from
// ~10 m out. Here a hexagonal lattice over the uv gives every hex its own
// random offset into the scan and its own rotation; each pixel takes the
// three hexes round it, blended by barycentric weight raised to a power and
// biased by luminance, which keeps the scan's contrast where a plain blend
// greys it out. The normal map's xy is turned back by each hex's rotation.
// Three fetches where there was one; ground only (scan_surface).
// ---------------------------------------------------------------------------
static const float kHexScale = 1.0f;       // hexes along a tile: a hex is ~0.35 tile across its flats
static const float kHexContrast = 0.6f;    // Mikkelsen's g_fallOffContrast
static const float kHexExp = 7.0f;         // ... g_exp
// Rock (scan_rock) fetches a hex corner only from 10% of the heaviest weight
// (w >= 0.72 of it), ground from 1%: in every plane at two scales, the
// overlap band cost ~0.06 ms of a 1440p frame half filled by a cliff.
static const float kRockHexCut = 0.1f;

float2 hex_hash2(int2 v) {
    uint h = uint(v.x) * 0x8da6b343u ^ uint(v.y) * 0xd8163841u;
    h = (h ^ (h >> 13)) * 0xcb1ab31fu;
    h ^= h >> 16;
    uint g = h * 0x9e3779b9u;
    g ^= g >> 15;
    return float2(float(h & 0xFFFFu), float(g & 0xFFFFu)) / 65535.0f;
}

// The triangle of the hex lattice a point is in: its three corners and the
// barycentric weights (Mikkelsen's TriangleGrid).
void hex_grid(float2 st, out float3 w, out int2 v1, out int2 v2, out int2 v3) {
    st *= 3.46410162f;   // 2 sqrt 3
    const float2 sk = float2(st.x, -0.57735027f * st.x + 1.15470054f * st.y);
    const int2 base = int2(floor(sk));
    float3 t = float3(frac(sk), 0.0f);
    t.z = 1.0f - t.x - t.y;
    const float s = step(0.0f, -t.z);
    const float s2 = 2.0f * s - 1.0f;
    w = float3(-t.z * s2, s - t.y * s2, s - t.x * s2);
    const int si = int(s);
    v1 = base + int2(si, si);
    v2 = base + int2(si, 1 - si);
    v3 = base + int2(1 - si, si);
}

float2 hex_centre(int2 v) {
    // The inverse of the skew, back in st units.
    const float2 f = float2(v);
    return float2(f.x, 0.5f * f.x + 0.86602540f * f.y) / 3.46410162f;
}

// One hex's own frame: the scan is read at mul(R, uv) + off. `rot` says how
// far a hex may turn: below 0 any angle (ground); otherwise the tangent of
// the most it turns either way, 0 for offsets alone. Rock's streaks and beds
// follow gravity on a wall, and turned at random they read as wrong.
void hex_corner(int2 v, float rot, out float2x2 R, out float2 off) {
    const float2 h = hex_hash2(v);
    // A rotation from the hash as a direction, no trigonometry.
    const float2 d = rot < 0.0f ? normalize(float2(h.x - 0.5f, h.y - 0.5f) + float2(1e-4f, 0.0f))
                                : normalize(float2(1.0f, (2.0f * h.x - 1.0f) * rot));
    R = float2x2(d.x, -d.y, d.y, d.x);
    const float2 c = hex_centre(v) / kHexScale;
    off = c - mul(R, c) + h * 7.31f;   // the hash again for the offset, off its period
}

// The scan at `uv` through the hex lattice: A (albedo, height) and, when
// asked, B with its normal xy turned back into the uv frame.
void hex_sample(float layer, float2 uv, float2 gx, float2 gy, bool want_b, float rot, float cut, out float4 a, out float4 b) {
    float3 w;
    int2 v[3];
    hex_grid(uv * kHexScale, w, v[0], v[1], v[2]);
    // The blend raises each corner's weight to the 7th power, so over most
    // of a hex one corner carries nearly all of it: a corner under 1% of the
    // heaviest is not fetched at all. Most pixels take one sample, not three.
    const float wp[3] = {pow(max(w.x, 0.0f), kHexExp), pow(max(w.y, 0.0f), kHexExp), pow(max(w.z, 0.0f), kHexExp)};
    const float wcut = cut * max(wp[0], max(wp[1], wp[2]));
    a = float4(0.0f, 0.0f, 0.0f, 0.0f);
    b = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    [unroll] for (int k = 0; k < 3; ++k) {
        if (wp[k] >= wcut) {
            float2x2 R;
            float2 off;
            hex_corner(v[k], rot, R, off);
            const float2 u = mul(R, uv) + off;
            const float4 sa = g_scan_a.SampleGrad(g_scan_samp, float3(u, layer), mul(R, gx), mul(R, gy));
            float4 sb = float4(0.5f, 0.5f, 0.5f, 1.0f);
            if (want_b) {
                sb = g_scan_b.SampleGrad(g_scan_samp, float3(u, layer), mul(R, gx), mul(R, gy));
                // The texture is turned by R^-1 in uv, so is its normal.
                sb.xy = mul(transpose(R), sb.xy * 2.0f - 1.0f) * 0.5f + 0.5f;
            }
            const float W = wp[k] * lerp(1.0f, dot(sa.rgb, float3(0.2126f, 0.7152f, 0.0722f)), kHexContrast);
            a += W * sa;
            b += W * sb;
            wsum += W;
        }
    }
    a /= max(wsum, 1e-6f);
    b /= max(wsum, 1e-6f);
}

struct ScanSample {
    float3 albedo;   // linear, before the tint
    float  height;
    float3 n;        // world, unnormalised sum
    float  rough;
    float  ao;
};

// The whiteout blend (Golus): a plane's tangent normal `t` (u mirrored on a
// back face already) folded round the geometric normal into world space.
float3 triplanar_normal(int k, float2 t, float3 n, float3 sg) {
    const float tz = sqrt(saturate(1.0f - dot(t, t)));
    if (k == 0) { const float3 q = float3(t + n.zy, tz * abs(n.x)); return float3(q.z * sg.x, q.y, q.x); }
    if (k == 1) { const float3 q = float3(t + n.xz, tz * abs(n.y)); return float3(q.x, q.z * sg.y, q.y); }
    const float3 q = float3(t + n.xy, tz * abs(n.z));
    return float3(q.x, q.y, q.z * sg.z);
}

// `hex`: the plane the normal faces most through the hex lattice
// (hex_sample); the others straight -- their weight is small where the
// ground is level enough to show a grid, and hex-tiling every plane of every
// candidate was +1.1 ms of a 1440p frame.
ScanSample scan_triplanar(float layer, float inv_tile, float3 p, float3 n, float3 bw, float3 dpx, float3 dpy,
                          bool hex = false) {
    ScanSample o = (ScanSample)0;
    const float3 sg = float3(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f, n.z < 0.0f ? -1.0f : 1.0f);
    const int kmax = (bw.y >= bw.x && bw.y >= bw.z) ? 1 : (bw.x >= bw.z ? 0 : 2);
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
        float4 a, b;
        if (hex && k == kmax) {
            hex_sample(layer, uv, gx, gy, true, -1.0f, 0.01f, a, b);
        } else {
            a = g_scan_a.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
            b = g_scan_b.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
        }
        float2 t = b.xy * 2.0f - 1.0f;
        t.x *= flip;
        o.albedo += w * a.rgb;
        o.height += w * a.a;
        nsum += w * triplanar_normal(k, t, n, sg);
        o.rough += w * b.z;
        o.ao += w * b.w;
    }
    o.n = nsum;
    return o;
}

// The heaviest rock candidate (mesh.hlsl, RockLook), at two scales:
//  - NEAR, the scan at its own tile, through the hex lattice in EVERY plane
//    when `hex` (a slope near 45 degrees weighs two planes alike and the
//    dominant one swaps across it: hex in one plane only would seam there);
//  - FAR, the same scan at `far_scale` x its tile, always hex-tiled, offset
//    in the world so the two lattices never line up, one mip coarser than
//    its footprint asks (it is structure; the near scan is the grain) --
//    cross-faded in by `frock`: colour, height, normal and all.
// A cliff at 40-140 m showed the 5 m scan repeating along it, and the near
// normal map has gone to its mips there: the far layer's ledges and cracks
// are the relief a cliff reads by. Each hex turns at most by `rot`
// (hex_corner): rock's streaks and beds follow gravity.
// ONE [loop] over (scale, plane), so the hex lattice's code is written once:
// FXC inlines everything, and hex_sample unrolled at two call sites in three
// planes each cost the whole terrain shader -- a wall filling half a 1440p
// frame went 1.68 -> 2.2 ms with the samples it took, and 1.55 with the same
// samples less the hex code at the far site.
ScanSample scan_rock(float layer, float inv_tile, float far_scale, float frock, float3 p, float3 n, float3 bw,
                     float3 dpx, float3 dpy, bool hex, float rot) {
    ScanSample o = (ScanSample)0;
    const float3 sg = float3(n.x < 0.0f ? -1.0f : 1.0f, n.y < 0.0f ? -1.0f : 1.0f, n.z < 0.0f ? -1.0f : 1.0f);
    float3 nsum = 0.0f;
    const int s0 = frock >= 1.0f ? 1 : 0, s1 = frock > 0.0f ? 2 : 1;
    [loop] for (int it = s0 * 3; it < s1 * 3; ++it) {
        const int s = it / 3, k = it - s * 3;
        const float w = (k == 0 ? bw.x : (k == 1 ? bw.y : bw.z)) * (s == 0 ? 1.0f - frock : frock);
        if ((k == 0 ? bw.x : (k == 1 ? bw.y : bw.z)) < 0.01f) continue;
        const float3 q = s == 0 ? p : p + float3(13.7f, 7.3f, 29.1f);
        float2 uv = k == 0 ? q.zy : (k == 1 ? q.xz : q.xy);
        float2 gx = k == 0 ? dpx.zy : (k == 1 ? dpx.xz : dpx.xy);
        float2 gy = k == 0 ? dpy.zy : (k == 1 ? dpy.xz : dpy.xy);
        const float flip = k == 0 ? sg.x : (k == 1 ? sg.y : -sg.z);
        uv.x *= flip;
        gx.x *= flip;
        gy.x *= flip;
        const float it_k = s == 0 ? inv_tile : inv_tile / far_scale;
        uv *= it_k;
        gx *= it_k * (s == 0 ? 1.0f : 2.0f);
        gy *= it_k * (s == 0 ? 1.0f : 2.0f);
        float4 a, b;
        if (hex || s == 1) {
            hex_sample(layer, uv, gx, gy, true, rot, kRockHexCut, a, b);
        } else {
            a = g_scan_a.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
            b = g_scan_b.SampleGrad(g_scan_samp, float3(uv, layer), gx, gy);
        }
        float2 t = b.xy * 2.0f - 1.0f;
        t.x *= flip;
        o.albedo += w * a.rgb;
        o.height += w * a.a;
        nsum += w * triplanar_normal(k, t, n, sg);
        o.rough += w * b.z;
        o.ao += w * b.w;
    }
    o.n = nsum;
    return o;
}

// The scan at a much LARGER scale (its tile x `scale`), hex-tiled, in the
// plane the normal faces most, as a colour ratio to the scan's own mean
// (its 1 x 1 mip): what the ground's own texture does over tens of metres --
// clumps, bare patches, colour shifts -- where the scan at its own tile has
// gone to its mean. Multiplied over the near scan by the caller.
float3 scan_far_ratio(float layer, float inv_tile, float scale, float3 p, float3 n, float3 bw, float3 dpx, float3 dpy) {
    float2 uv, gx, gy;
    float flip;
    if (bw.y >= bw.x && bw.y >= bw.z) { uv = p.xz; gx = dpx.xz; gy = dpy.xz; flip = n.y < 0.0f ? -1.0f : 1.0f; }
    else if (bw.x >= bw.z) { uv = p.zy; gx = dpx.zy; gy = dpy.zy; flip = n.x < 0.0f ? -1.0f : 1.0f; }
    else { uv = p.xy; gx = dpx.xy; gy = dpy.xy; flip = n.z < 0.0f ? 1.0f : -1.0f; }
    uv.x *= flip;
    gx.x *= flip;
    gy.x *= flip;
    const float k = inv_tile / scale;
    float4 a, b;
    // Offset from the near scan's lattice so the two scales never line up.
    // Two mips coarser than the footprint asks (gradients x4): this layer is
    // structure over metres, the near scan carries the grain -- and the fine
    // mips of a scan magnified 5x were most of its 0.3 ms at 1440p.
    hex_sample(layer, uv * k + float2(0.37f, 0.71f), gx * (4.0f * k), gy * (4.0f * k), false, -1.0f, 0.01f, a, b);
    const float3 mean = g_scan_a.SampleLevel(g_scan_samp, float3(0.5f, 0.5f, layer), 16.0f).rgb;
    return clamp(a.rgb / max(mean, 1e-3f), 0.3f, 2.5f);
}

// Parallax in the triplanar plane the normal faces most (pom.hlsli): the
// world-space shift to move a sample point by, and the self-shadow towards
// the key light in `psh`. Zero shift where parallax is off, out of reach, or
// no plane clearly dominates -- where two weigh alike the dominant one flips
// from pixel to pixel and the shift with it, which drew a dark seam along a
// cliff's crease.
//
// `hexed`: the scan in that plane is drawn through the hex lattice (rock,
// scan_rock, turned by `rot`). The march then reads the
// height map in the frame of the hex the point is most in -- the uv turned
// by its R and moved by its offset, the view and light steps turned with
// it. Where hexes blend (a narrow band, weights to the 7th) the drawn height
// is a mix and the march follows only the heaviest; that band was not seen.
float3 scan_pom_shift(float layer, float inv_tile, float depth_m, float3 p, float3 n, float3 v, float3 bw,
                      float3 dpx, float3 dpy, float dist, out float psh, bool hexed, float rot) {
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
    float2 gx = float2(dot(dpx, U), dot(dpx, V)) * inv_tile;
    float2 gy = float2(dot(dpy, U), dot(dpy, V)) * inv_tile;
    const float scale = depth * inv_tile;
    const float vz = max(dot(v, A), 0.2f);
    const float2 view_step = -float2(dot(v, U), dot(v, V)) / vz * scale;
    const int steps = int(lerp(6.0f, 20.0f, saturate(1.0f - dot(v, A))));
    float2 lstep = 0.0f;
    const float lz = dot(kSunDir, A);
    if (lz > 0.05f) lstep = float2(dot(kSunDir, U), dot(kSunDir, V)) / lz * scale;
    // The march's own frame: the plain uv, or the heaviest hex's.
    float2 uvm = uv, vstep = view_step;
    if (hexed) {
        float3 w;
        int2 c0, c1, c2;
        hex_grid(uv * kHexScale, w, c0, c1, c2);
        int2 c = c0;
        if (w.y > w.x && w.y >= w.z) c = c1;
        else if (w.z > w.x && w.z > w.y) c = c2;
        float2x2 R;
        float2 off;
        hex_corner(c, rot, R, off);
        uvm = mul(R, uv) + off;
        vstep = mul(R, view_step);
        lstep = mul(R, lstep);
        gx = mul(R, gx);
        gy = mul(R, gy);
    }
    const float th = pom_march(uvm, layer, vstep, gx, gy, steps);
    if (lz > 0.05f) psh = pom_shadow_faded(uvm + vstep * th, th, layer, lstep, lz, gx, gy);
    // th is a fraction of the depth in either frame; the shift is in world.
    return (U * view_step.x + V * view_step.y) * (th / inv_tile);
}

#endif
