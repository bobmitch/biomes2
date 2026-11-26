// ---------------------------------------------------------------------------
// B2: draw the surface-nets mesh.
//
// Shades the same way the B1 raymarch does, on purpose. The two are rendered
// from the same camera and the only intended difference is that one marches
// the field and the other draws triangles built from it, so any difference in
// the image is a meshing difference and not a lighting one.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/wet.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/flash.hlsli"
#include "engine/render/shaders/local_lights.hlsli"

struct Vertex {
    float3 pos;
    float3 nrm;
};

StructuredBuffer<Vertex> g_vertices : register(t0);
// Per-slot LOD cross-fade: (mode, v). 0 drawn; 1 fading in, drawn where the
// dither threshold is below v; 2 fading out, drawn where it is at or above v.
// Streamer::fill_visibility writes it.
StructuredBuffer<float2> g_fade : register(t1);
// Each vertex's surface material, parallel to g_vertices (surface_nets.hlsl:
// terrain_material). (ground, rock): sqrt(linear albedo) in RGB, roughness in
// A, 8 bits each.
StructuredBuffer<uint2> g_vertex_mat : register(t2);
// Each vertex's share of every biome's material, 8 bits x 12 (surface_nets.hlsl
// g_vertex_wts): biome index order, the seabed after the last biome.
StructuredBuffer<uint3> g_vertex_wts : register(t3);

#include "engine/render/shaders/scan.hlsli"
#include "engine/render/shaders/forest.hlsli"
#include "engine/render/water_wave.h"

cbuffer MeshConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;  float far_plane;
    // Non-zero when the mesh is being drawn as wireframe over the raymarch.
    // The overlay is useless if the edges are shaded like the terrain they sit
    // on, which is how the first attempt came out.
    float  wire;     float slot_vertices;
    // 0 off; 1 skirt curtain magenta; 2 sharp creases cyan. See ps_main.
    float  mesh_debug;
    // Surface texture: 0 flat, 1 noise detail, 2 metric grid, 3 both.
    float  texture_mode;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    nointerpolation float2 fade : TEXCOORD2;
    float  skirt : TEXCOORD3;   // 1 at a skirt vertex, 0 at a surface vertex
    float4 ground : TEXCOORD4;  // linear albedo, roughness
    float4 rock   : TEXCOORD5;
    float4 w0 : TEXCOORD6;     // material weights 0..3, 4..7, 8..11
    float4 w1 : TEXCOORD7;
    float4 w2 : TEXCOORD8;
    float  shell : TEXCOORD9;  // the canopy shell's lift as a share of its full height (forest.hlsli)
};

float4 unpack_weights(uint u) {
    return float4(u & 0xFFu, (u >> 8) & 0xFFu, (u >> 16) & 0xFFu, u >> 24) / 255.0f;
}

float4 unpack_material(uint m) {
    const float4 q = float4(m & 0xFFu, (m >> 8) & 0xFFu, (m >> 16) & 0xFFu, m >> 24) / 255.0f;
    return float4(q.rgb * q.rgb, q.a);
}

// No input layout. With an index buffer bound, SV_VertexID is already the
// index-buffer-resolved vertex, so the attributes can come straight out of the
// buffer the mesher wrote -- there is no vertex struct to keep in sync across
// the C++/HLSL boundary, which is one fewer thing that can silently disagree.
VSOut vs_main(uint vid : SV_VertexID) {
    const Vertex v = g_vertices[vid];
    VSOut o;
    // The canopy shell lifts far forest to its canopy (forest.hlsli). In
    // every pass that draws the terrain, so its shadows come with it.
    float shell_frac;
    const float3 pos = v.pos + float3(0.0f, forest_lift(v.pos, v.nrm, shell_frac), 0.0f);
    o.shell = shell_frac;
    o.clip = mul(float4(pos, 1.0f), view_proj);
    o.world = pos;
    o.nrm = v.nrm;
    // Indices are absolute, so the vertex id says which slot this is.
    o.fade = g_fade[vid / uint(slot_vertices)];
    // Exact: the kernel stores a skirt vertex's normal at half length. A
    // surface triangle is 0 everywhere; a curtain triangle is not.
    o.skirt = dot(v.nrm, v.nrm) < 0.5f ? 1.0f : 0.0f;
    const uint2 m = g_vertex_mat[vid];
    o.ground = unpack_material(m.x);
    o.rock = unpack_material(m.y);
    const uint3 w = g_vertex_wts[vid];
    o.w0 = unpack_weights(w.x);
    o.w1 = unpack_weights(w.y);
    o.w2 = unpack_weights(w.z);
    return o;
}

// 4x4 ordered dither, thresholds in (0, 1). A LOD swap is a screen-door
// cross-fade: the outgoing chunk keeps the pixels whose threshold is at or
// above v and the incoming one takes the rest, so at any v every pixel is
// drawn by exactly one of them -- no double surface to z-fight, no gap.
static const float kBayer[16] = {
     0.0f,  8.0f,  2.0f, 10.0f,
    12.0f,  4.0f, 14.0f,  6.0f,
     3.0f, 11.0f,  1.0f,  9.0f,
    15.0f,  7.0f, 13.0f,  5.0f};

float dither_threshold(float2 px) {
    const uint x = uint(px.x) & 3u;
    const uint y = uint(px.y) & 3u;
    return (kBayer[y * 4u + x] + 0.5f) / 16.0f;
}

// ---------------------------------------------------------------------------
// Triplanar surface texture, procedural -- no image files.
//
// A flat-shaded surface gives the eye nothing to track: motion and scale
// read only from the silhouette, which is why speed is hard to judge low
// over open ground. Two cues, projected on the three world planes and
// blended by the normal so steep faces do not smear:
//   detail -- value noise at ~4 m and ~0.5 m, a few percent of albedo;
//   grid   -- lines every metre and heavier ones every ten, for scale.
// Both are antialiased with fwidth and fade where a pixel spans too much of
// the pattern to show it honestly, the same rule the octave fade follows.
// This is presentation: invariant 9 does not reach it (no hash, no save).
// ---------------------------------------------------------------------------
float hash21(float2 c) {
    const uint2 q = uint2(int2(c) + int2(0x4000, 0x4000));
    uint h = q.x * 0x8da6b343u ^ q.y * 0xd8163841u;
    h = (h ^ (h >> 13)) * 0xcb1ab31fu;
    h = h ^ (h >> 16);
    return float(h & 0xFFFFu) / 65535.0f;
}

float value_noise(float2 p) {
    const float2 c = floor(p);
    const float2 f = p - c;
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = hash21(c), b = hash21(c + float2(1, 0));
    const float d = hash21(c + float2(0, 1)), e = hash21(c + float2(1, 1));
    return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
}

// One plane's detail, in [-1, 1], faded out where a pixel covers a cell.
float plane_detail(float2 p, float footprint) {
    const float coarse = value_noise(p * 0.25f) * 2.0f - 1.0f;
    const float fine = value_noise(p * 2.0f) * 2.0f - 1.0f;
    const float fine_w = saturate(1.0f - footprint * 2.0f);    // 0.5 m cells
    const float coarse_w = saturate(1.0f - footprint * 0.25f); // 4 m cells
    return 0.6f * coarse * coarse_w + 0.4f * fine * fine_w;
}

// One plane's grid coverage in [0, 1]: thin lines on whole metres, thicker on
// tens, each faded once its spacing is under ~4 pixels.
float plane_grid(float2 p) {
    const float2 fw = max(fwidth(p), float2(1e-5f, 1e-5f));
    const float2 d1 = abs(frac(p + 0.5f) - 0.5f) / fw;           // pixels to a 1 m line
    const float2 d10 = abs(frac(p * 0.1f + 0.5f) - 0.5f) * 10.0f / fw;
    const float minor = (1.0f - saturate(min(d1.x, d1.y) - 0.5f)) * saturate(1.0f - max(fw.x, fw.y) * 4.0f);
    const float major = (1.0f - saturate(min(d10.x, d10.y) * 0.6f - 0.5f)) * saturate(1.0f - max(fw.x, fw.y) * 0.4f);
    return max(minor * 0.45f, major * 0.8f);
}

float3 apply_surface_texture(float3 albedo, float3 p, float3 n) {
    if (texture_mode < 0.5f) return albedo;
    float3 w = abs(n);
    w = w * w * w * w;
    w /= (w.x + w.y + w.z);
    float shade = 1.0f;
    if (texture_mode > 0.5f && texture_mode < 1.5f || texture_mode > 2.5f) {
        const float fp = length(fwidth(p));
        const float det = w.x * plane_detail(p.zy, fp) + w.y * plane_detail(p.xz, fp) +
                          w.z * plane_detail(p.xy, fp);
        shade *= 1.0f + 0.18f * det;
    }
    if (texture_mode > 1.5f) {
        const float g = w.x * plane_grid(p.zy) + w.y * plane_grid(p.xz) + w.z * plane_grid(p.xy);
        shade *= 1.0f - 0.55f * g;
    }
    return albedo * shade;
}

// Up to four candidates -- the two heaviest biomes, each its ground and its
// rock -- blended by HEIGHT as well as weight: where two meet, the higher
// texel wins (stones through sand) instead of both fading through each
// other. kScanBlendDepth is how far below the top a candidate still shows.
static const float kScanBlendDepth = 0.2f;
static const float kScanHeightGain = 0.6f;

struct ScanResult { float3 albedo; float rough; float3 n; float ao; float height; bool ok; };

// The two heaviest biomes at the pixel and their weights (the second 0 when
// it is under 2%).
void top_two(VSOut i, out uint m0, out uint m1, out float b0, out float b1) {
    const float wts[12] = {i.w0.x, i.w0.y, i.w0.z, i.w0.w, i.w1.x, i.w1.y, i.w1.z, i.w1.w,
                           i.w2.x, i.w2.y, i.w2.z, i.w2.w};
    m0 = 0u; m1 = 0u;
    b0 = -1.0f; b1 = -1.0f;
    [unroll] for (uint m = 0u; m < 12u; ++m) {
        const float w = wts[m];
        if (w > b0) { m1 = m0; b1 = b0; m0 = m; b0 = w; }
        else if (w > b1) { m1 = m; b1 = w; }
    }
    if (b1 < 0.02f) b1 = 0.0f;
}

// One candidate's share of the height blend (scan_surface).
void scan_blend_add(ScanSample s, float w, float top, float3 tint, float rscale,
                    inout float wsum, inout ScanResult r, inout float3 nsum) {
    if (w <= 0.0f) return;
    const float hb = max(s.height * kScanHeightGain + w - top + kScanBlendDepth, 0.0f);
    wsum += hb;
    r.albedo += hb * s.albedo * tint;
    r.rough += hb * saturate(s.rough * rscale);
    r.ao += hb * s.ao;
    r.height += hb * s.height;
    nsum += hb * s.n;
}

// `dpx`, `dpy`: the world position's screen derivatives, taken by the caller
// before any branch -- this runs only inside the detail fade.
// The four candidates are four NAMED variables, never an array: written as
// arrays (sc[4], wc[4], tint[4], rscale[4]) and indexed by the heaviest and
// the hex-tiled candidate's index, FXC kept them in dynamically indexed
// temps (dcl_indexableTemp, 32 float4), which this GPU's compiler puts in
// scratch memory. That was most of what the scans cost: at 4K in a lit pit,
// terrain 8.1 -> 6.0 ms, the image the same to 2 levels in 255.
ScanResult scan_surface(VSOut i, float3 n, float level, float3 v, float3 dpx, float3 dpy, out float psh) {
    ScanResult r = (ScanResult)0;
    psh = 1.0f;
    uint m0, m1;
    float b0, b1;
    top_two(i, m0, m1, b0, b1);
    const float tot = b0 + b1;
    if (tot <= 0.0f) return r;
    const ScanMaterial M0 = g_scan_mat[m0], M1 = g_scan_mat[m1];
    if (M0.ground_layer < 0.0f || M0.rock_layer < 0.0f || (b1 > 0.0f && (M1.ground_layer < 0.0f || M1.rock_layer < 0.0f)))
        return r;   // no scan for this material: the caller keeps the old path

    float3 bw = abs(n);
    bw = bw * bw * bw * bw;
    bw /= (bw.x + bw.y + bw.z);
    float3 p = i.world;

    // The candidates: the first biome's ground and rock, then the second's.
    float w0 = b0 / tot * level, w1 = b0 / tot * (1.0f - level);
    float w2 = b1 / tot * level, w3 = b1 / tot * (1.0f - level);
    w0 = w0 < 0.005f ? 0.0f : w0;
    w1 = w1 < 0.005f ? 0.0f : w1;
    w2 = w2 < 0.005f ? 0.0f : w2;
    w3 = w3 < 0.005f ? 0.0f : w3;
    const bool hex = l_look3.x > 0.5f;

    // Parallax (pom.hlsli): the heaviest candidate's height map, in the
    // plane the normal faces most; its shift moves every candidate's sample
    // point, and its self-shadow darkens the key light. Not on hex-tiled
    // ground: the march reads the height map straight, and under a scan
    // turned and offset per hex its shift would not match what is drawn.
    // Ground parallax is 3-5 cm out to ~10 m, barely seen under grass.
    float wmax = w0;
    bool d_second = false, d_rock = false;
    if (w1 > wmax) { wmax = w1; d_second = false; d_rock = true; }
    if (w2 > wmax) { wmax = w2; d_second = true; d_rock = false; }
    if (w3 > wmax) { wmax = w3; d_second = true; d_rock = true; }
    if (d_rock || !hex) {
        ScanMaterial MD = M0;   // FXC takes no ternary on a struct
        if (d_second) MD = M1;
        p += scan_pom_shift(d_rock ? MD.rock_layer : MD.ground_layer, d_rock ? MD.rock_inv_tile : MD.ground_inv_tile,
                            d_rock ? MD.rock_depth : MD.ground_depth, p, n, v, bw, dpx, dpy,
                            length(cam_pos - i.world), psh);
    }
    // Hex-tiling for the heaviest GROUND candidate only: the second biome's
    // share at a boundary and the rock on a slope show no grid worth three
    // fetches.
    const bool g_second = w2 > w0;
    ScanSample s0 = (ScanSample)0, s1 = (ScanSample)0, s2 = (ScanSample)0, s3 = (ScanSample)0;
    if (w0 > 0.0f) s0 = scan_triplanar(M0.ground_layer, M0.ground_inv_tile, p, n, bw, dpx, dpy, hex && !g_second);
    if (w1 > 0.0f) s1 = scan_triplanar(M0.rock_layer, M0.rock_inv_tile, p, n, bw, dpx, dpy, false);
    if (w2 > 0.0f) s2 = scan_triplanar(M1.ground_layer, M1.ground_inv_tile, p, n, bw, dpx, dpy, hex && g_second);
    if (w3 > 0.0f) s3 = scan_triplanar(M1.rock_layer, M1.rock_inv_tile, p, n, bw, dpx, dpy, false);

    float top = -1e9f;
    if (w0 > 0.0f) top = max(top, s0.height * kScanHeightGain + w0);
    if (w1 > 0.0f) top = max(top, s1.height * kScanHeightGain + w1);
    if (w2 > 0.0f) top = max(top, s2.height * kScanHeightGain + w2);
    if (w3 > 0.0f) top = max(top, s3.height * kScanHeightGain + w3);
    float wsum = 0.0f;
    float3 nsum = 0.0f;
    scan_blend_add(s0, w0, top, M0.ground_tint, M0.ground_rough / M0.ground_scan_rough, wsum, r, nsum);
    scan_blend_add(s1, w1, top, M0.rock_tint, M0.rock_rough / M0.rock_scan_rough, wsum, r, nsum);
    scan_blend_add(s2, w2, top, M1.ground_tint, M1.ground_rough / M1.ground_scan_rough, wsum, r, nsum);
    scan_blend_add(s3, w3, top, M1.rock_tint, M1.rock_rough / M1.rock_scan_rough, wsum, r, nsum);
    if (wsum <= 0.0f) return r;
    r.albedo /= wsum;
    r.rough /= wsum;
    r.ao /= wsum;
    r.height /= wsum;
    r.n = normalize(nsum);
    // Far detail: the heaviest GROUND candidate's own scan at a much larger
    // scale, as a colour ratio over the blend -- structure over tens of
    // metres, coming in from 10 m out as the near scan's own goes to its mips.
    const float wfar = l_look3.y * smoothstep(10.0f, 40.0f, length(cam_pos - i.world)) * level;
    if (wfar > 0.0f && (g_second ? w2 : w0) > 0.0f) {
        ScanMaterial MG = M0;
        if (g_second) MG = M1;
        r.albedo *= lerp(float3(1.0f, 1.0f, 1.0f),
                         scan_far_ratio(MG.ground_layer, MG.ground_inv_tile, l_look3.z, p, n, bw, dpx, dpy), wfar);
    }
    r.ok = true;
    return r;
}

// The scans' own MEANS, for ground past the detail fade: each candidate's
// 1 x 1 mip (exactly the mean the retint maps to the authored colour) times
// its tint, weighted as scan_surface weights them but without the height
// blend, which averages out at this distance. One fetch a candidate, where
// scan_surface takes a parallax march and up to three planes a candidate.
ScanResult scan_mean(VSOut i, float level) {
    ScanResult r = (ScanResult)0;
    uint m0, m1;
    float b0, b1;
    top_two(i, m0, m1, b0, b1);
    const float tot = b0 + b1;
    if (tot <= 0.0f) return r;
    const ScanMaterial M0 = g_scan_mat[m0], M1 = g_scan_mat[m1];
    if (M0.ground_layer < 0.0f || M0.rock_layer < 0.0f || (b1 > 0.0f && (M1.ground_layer < 0.0f || M1.rock_layer < 0.0f)))
        return r;
    const float wc[4] = {b0 / tot * level, b0 / tot * (1.0f - level), b1 / tot * level, b1 / tot * (1.0f - level)};
    float wsum = 0.0f;
    [unroll] for (int c = 0; c < 4; ++c) {
        if (wc[c] < 0.005f) continue;
        ScanMaterial M = M1;   // FXC takes no ternary on a struct
        if (c < 2) M = M0;
        const bool ground = (c & 1) == 0;
        const float layer = ground ? M.ground_layer : M.rock_layer;
        const float4 a = g_scan_a.SampleLevel(g_scan_samp, float3(0.5f, 0.5f, layer), 16.0f);
        const float4 b = g_scan_b.SampleLevel(g_scan_samp, float3(0.5f, 0.5f, layer), 16.0f);
        const float rs = ground ? M.ground_rough / M.ground_scan_rough : M.rock_rough / M.rock_scan_rough;
        r.albedo += wc[c] * a.rgb * (ground ? M.ground_tint : M.rock_tint);
        r.rough += wc[c] * saturate(b.z * rs);
        r.ao += wc[c] * b.w;
        r.height += wc[c] * a.a;
        wsum += wc[c];
    }
    if (wsum <= 0.0f) return r;
    r.albedo /= wsum;
    r.rough /= wsum;
    r.ao /= wsum;
    r.height /= wsum;
    r.ok = true;
    return r;
}

// Macro variation: what bare ground does over hundreds of metres, which
// neither scale of the scan carries. Value noise at 97 and 41 m for
// brightness, a 63 m noise for dry and lush patches, and the terrain's own
// shape from the heightfield GI's map (hfgi.hlsli, 512 m round the camera):
// hollows a little darker and lusher, crests paler and drier. `strength` 1
// is the authored amount. The 17 and 7 m octaves went when the far detail
// layer (scan_far_ratio) came to carry that band from the scan itself: in
// the terrain shader every noise is paid by every pixel.
float3 ground_macro(float3 albedo, float3 world, float strength) {
    if (strength <= 0.0f) return albedo;
    const float2 q = world.xz;
    // Value noise sits near its middle; x2.2 takes the sum to about +-0.5
    // at its usual extremes.
    const float b = 2.2f * (0.6f * value_noise(q / 97.0f) + 0.4f * value_noise(q / 41.0f + 13.1f) - 0.5f);
    float dry = smoothstep(0.38f, 0.66f, value_noise(q / 63.0f + 5.3f));
    // The shape: the heightfield map's own curvature channel, one read
    // (height_gi.hlsl computes it per texel at each rebuild). Reading five
    // heights here per pixel cost 0.15 ms of a 1440p frame at eye level.
    const float shape = clamp(hf_curvature(q), -0.6f, 0.6f);
    dry = saturate(dry + 0.8f * shape);
    // About +-0.3 of a stop from the noise at its usual range, +-0.2 from
    // the shape.
    const float gain = exp2(strength * (0.6f * b + 0.35f * shape));
    // Dry: warmer and paler -- towards straw on grassland, towards dust on
    // earth -- a little desaturated; lush: deeper and cooler. A first dry
    // tone went to the luminance and read as grey ash on a meadow.
    const float lum = dot(albedo, float3(0.2126f, 0.7152f, 0.0722f));
    const float3 dried = lerp(albedo, float3(lum, lum, lum), 0.2f) * float3(1.3f, 1.12f, 0.72f);
    const float3 lush = albedo * float3(0.82f, 0.95f, 0.8f);
    const float3 tone = lerp(lush, dried, dry);
    return max(lerp(albedo, tone, saturate(0.7f * strength)) * gain, 0.0f);
}

// The canopy shell's crowns (forest.hlsli): a jittered dome per cell, the
// tallest wins where they overlap, so crowns are round with dark gaps
// between them rather than value noise's soft square blobs, which read as
// low resolution beside the real trees. Returns the dome height (0 gap .. 1
// top), its gradient in q units, and a per-crown random for its tone.
float crown_dome(float2 q, out float2 grad, out float tone) {
    const float2 c = floor(q);
    float best = 0.0f;
    grad = float2(0.0f, 0.0f);
    tone = 0.5f;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float2 cell = c + float2(i, j);
            const float h0 = hash21(cell), h1 = hash21(cell + float2(37.0f, 11.0f));
            const float h2 = hash21(cell + float2(-23.0f, 59.0f));
            const float2 p = cell + 0.5f + 0.38f * float2(h0 * 2.0f - 1.0f, h1 * 2.0f - 1.0f);
            const float r = 0.5f + 0.3f * h2;
            const float2 dq = q - p;
            const float h = 1.0f - dot(dq, dq) / (r * r);
            if (h > best) {
                best = h;
                grad = -2.0f * dq / (r * r);
                tone = h0;
            }
        }
    }
    return saturate(best);
}

// The heightfield GI's top-down view (height_gi.h): the terrain's albedo as
// lighting sees it, ground or rock by slope. Fades and skirts do not matter
// from above: the depth test keeps the top.
float4 ps_top(VSOut i) : SV_Target0 {
    const float3 n = normalize(i.nrm);
    float3 albedo = lerp(i.rock.rgb, i.ground.rgb, smoothstep(0.62f, 0.86f, n.y));
    const Forest f = forest_at(i.world);
    albedo = lerp(albedo, f.leaf, f.cover * f.w * saturate((n.y - 0.55f) / 0.3f));
    return float4(albedo, 1.0f);
}

// The sea's bed (renderer.cpp record_water_bed): the terrain from just over
// sea level down, the first surface under each texel. A floor gives its
// height. A CEILING seen first means the camera's column is inside rock at
// sea level -- a tunnel's roof under the land, a dome -- and is marked 1e4,
// rock: no water stands there. Skirts are not the surface: a curtain hangs a
// metre into the rock under every chunk border, and from under a beach just
// over the top it read as a bed below sea level.
float ps_bed(VSOut i) : SV_Target0 {
    if (i.skirt > 0.001f) discard;
    return i.nrm.y < 0.0f ? 1.0e4f : i.world.y;
}

// A chunk cross-fading between LOD levels: the 4x4 dither it is drawn
// through, complementary for the one coming in and the one going out.
void lod_fade_clip(VSOut i) {
    if (i.fade.x > 0.5f) {
        const float t = dither_threshold(i.clip.xy);
        if (i.fade.x < 1.5f) clip(i.fade.y - t);   // fading in
        else                 clip(t - i.fade.y);   // fading out
    }
}

// The terrain's depth prepass (renderer.cpp): depth only, through the same
// fade dither as ps_main, so the colour pass's EQUAL test passes on exactly
// the pixels it would have drawn. Both run vs_main, so the depths match bit
// for bit, as the trees' prepass does.
void ps_depth(VSOut i) {
    lod_fade_clip(i);
}

SceneOut ps_main(VSOut i) {
    lod_fade_clip(i);
    if (wire > 0.5f) return scene_flat(float3(1.0f, 0.25f, 0.65f));
    // Debug views. The first attempt at "where is the curtain visible" tested
    // the interpolated normal's length, which a sharp CREASE also shortens --
    // it counted the terrain's ridge lines as seams and sent a whole
    // investigation the wrong way. The skirt flag is exact; the crease view
    // is the normal-length test, but only where the flag is zero.
    if (mesh_debug > 0.5f && mesh_debug < 1.5f && i.skirt > 0.001f)
        return scene_flat(float3(1.0f, 0.0f, 1.0f));
    if (mesh_debug > 1.5f && i.skirt <= 0.001f && length(i.nrm) < 0.9f)
        return scene_flat(float3(0.0f, 1.0f, 1.0f));
    const float3 n = normalize(i.nrm);
    const float3 v = normalize(cam_pos - i.world);
    // The pixel's footprint, for the rain's ripples to fade by (before any
    // branch: it takes derivatives).
    const float foot = length(fwidth(i.world.xz));

    // The biome's material (ADR-004 L1): its ground on the level, its rock on
    // anything steeper than about 40 degrees. Was one hard-coded rock and
    // one green, blended by n.y squared, in every biome alike.
    float level = smoothstep(0.62f, 0.86f, n.y);
    // Under the real top surface (a cave, a dug tunnel) a floor is the
    // rock's, not the meadow's: grass on a cave floor 10 m down read as a
    // lawn. One load of the near field's top map; past its 128 m nothing is
    // close enough to tell.
    float under_dh;
    if (nf_depth(i.world, under_dh)) level *= 1.0f - smoothstep(-1.0f, -2.5f, under_dh);
    const float4 mat = lerp(i.rock, i.ground, level);
    float3 albedo = mat.rgb;
    float rough = mat.a;
    float3 ns = n;
    float ao = 1.0f;
    // T: 0 flat, 1 the scans (noise detail where a material has none), 2 the
    // metric grid, 3 scans and grid.
    const bool scans = (texture_mode > 0.5f && texture_mode < 1.5f) || texture_mode > 2.5f;
    // Past the near field the scan fades into its own mean (GroundLook): a
    // 2.5 m scan repeated over a clearing reads as a grid from ~40 m, and
    // past the fade it is not sampled at all. The macro variation below is
    // what gives that ground its structure instead.
    const float dist = length(cam_pos - i.world);
    const float detail = 1.0f - smoothstep(l_look.y, l_look.z, dist);
    const float3 dpx = ddx(i.world), dpy = ddy(i.world);
    ScanResult sr = (ScanResult)0;
    float psh = 1.0f;   // the scans' parallax self-shadow
    if (scans && detail > 0.0f) sr = scan_surface(i, n, level, v, dpx, dpy, psh);
    if (scans && detail < 1.0f) {
        const ScanResult sm = scan_mean(i, level);
        if (sm.ok && sr.ok) {
            sr.albedo = lerp(sm.albedo, sr.albedo, detail);
            sr.rough = lerp(sm.rough, sr.rough, detail);
            sr.ao = lerp(sm.ao, sr.ao, detail);
            sr.height = lerp(sm.height, sr.height, detail);
            sr.n = normalize(lerp(n, sr.n, detail));
            psh = lerp(1.0f, psh, detail);
        } else if (sm.ok) {
            sr = sm;
            sr.n = n;
        }
    }
    if (sr.ok) {
        albedo = sr.albedo;
        rough = sr.rough;
        ns = sr.n;
        ao = sr.ao;
        if (texture_mode > 2.5f) albedo = apply_surface_texture(albedo, i.world, n);
    } else {
        albedo = apply_surface_texture(mat.rgb, i.world, n);
    }
    // Macro variation, on ground more than on rock.
    if (texture_mode < 1.5f) albedo = ground_macro(albedo, i.world, l_look.x * lerp(0.4f, 1.0f, level));

    // The canopy shell (forest.hlsli): far forest as a surface of crowns,
    // shaded the way trees.hlsl shades leaves -- wrapped diffuse and light
    // through the leaves, no specular, skylight by diffuse GI alone -- then
    // blended over the ground by the shell's weight. On the ground's BRDF a
    // first version reflected grey-blue sky off the canopy that real crowns
    // do not, and read pale beside the trees. A crown mass seen from above
    // is also leaves shading leaves: kShellSun and kShellSky are the shares
    // of sun and sky its surface takes, matched in a still against the
    // trees drawn to 2 km. Crown-sized bumps while a crown spans a few
    // pixels, faded into their average past it: unfaded, 6 m bumps aliased
    // into ripples a kilometre out.
    float  cw = 0.0f, shell_sun = 1.0f, shell_sky = 1.0f, shell_w = 0.0f;
    float3 leaf_alb = 0.0f, leaf_n = n;
    {
        const Forest f = forest_at(i.world);
        cw = f.cover * f.w * saturate((n.y - 0.55f) / 0.3f);
        // The wall where the canopy ends at a clearing: cover falls from 1 to
        // 0 across one twenty-metre texel, so the lifted surface drops the
        // canopy's height in about that -- and the cover sampled ON the wall
        // was partial, so most of it shaded as bare ground, a brown mesa
        // round every wood and clump. The lift share interpolated from the
        // vertices says how far up the wall a pixel is: above its foot it
        // is the side of the wood.
        const float wall_up = saturate(i.shell * 1.6f);
        cw = max(cw, wall_up);
        // The face's own normal, before any branch (it takes derivatives).
        float3 nf = normalize(cross(ddy(i.world), ddx(i.world)));
        if (dot(nf, v) < 0.0f) nf = -nf;
        const float steep = saturate((0.85f - nf.y) / 0.45f);
        shell_w = f.w;
        if (cw > 0.002f) {
            // Two scales, each faded into its average as its pixel footprint
            // grows -- a procedural mip chain: crowns ~7 m (4 m conifers),
            // and leaf clusters a fifth of that on them.
            const float sz = lerp(7.0f, 4.0f, f.conifer);
            const float fp = length(fwidth(i.world.xz));
            const float k = saturate(1.6f - 0.9f * fp / sz);           // crowns while ~2+ px across
            const float k2 = saturate(1.6f - 0.9f * fp / (0.2f * sz)); // clusters likewise
            float p = 0.5f, tone = 0.5f;
            float3 cn = float3(0.0f, 1.0f, 0.0f);
            if (k > 0.0f) {
                float2 g;
                p = crown_dome(i.world.xz / sz, g, tone);
                // The dome's slope, crowns half as tall as they are wide.
                cn = normalize(float3(-g.x * 0.5f, 1.0f, -g.y * 0.5f));
                if (k2 > 0.0f) {
                    const float2 q2 = i.world.xz / (0.2f * sz);
                    const float c0 = value_noise(q2), cx = value_noise(q2 + float2(0.3f, 0.0f));
                    const float cz = value_noise(q2 + float2(0.0f, 0.3f));
                    p = saturate(p * (0.8f + 0.4f * lerp(0.5f, c0, k2)));
                    cn = normalize(cn + k2 * float3(-(cx - c0) * 1.5f, 0.0f, -(cz - c0) * 1.5f));
                }
            }
            // The average of a dome field's height is about a third; the
            // shading below is calibrated round 0.5, so map it there.
            const float pat = lerp(0.5f, saturate(p * 1.4f), k);
            const float crown_tone = lerp(1.0f, 0.82f + 0.36f * tone, k);
            leaf_alb = f.leaf * (0.7f + 0.6f * pat) * crown_tone;
            leaf_n = normalize(lerp(float3(0.0f, 1.0f, 0.0f), cn, k));
            // On a wall the canopy's side faces out: shade it with the face's
            // own normal (the mesh's normals are the unlifted ground's), and
            // let a side take less sky than a top.
            if (steep > 0.0f)
                leaf_n = normalize(lerp(leaf_n, normalize(nf + 0.3f * (leaf_n - float3(0.0f, 1.0f, 0.0f))), steep));
            shell_sun = kShellSun * (0.4f + 1.2f * pat);
            shell_sky = kShellSky * (0.5f + pat) * (1.0f - 0.35f * steep);
        } else {
            cw = 0.0f;
        }
    }

    // The wet world (ADR-005 C4, wet.hlsli): rain soaks the ground -- soil
    // darkens most, rock takes a glossy film -- and puddles stand in the
    // hollows of flat ground, ringed while it rains. Sheltered under crowns
    // and overhangs.
    float pud = 0.0f;
    if (l_wet.x > 0.0f) {
        const float shelter = wet_shelter(i.world);
        const float wet = l_wet.x * shelter * lerp(0.7f, 1.0f, saturate(n.y));
        wet_surface(albedo, rough, ns, n, lerp(0.35f, 0.9f, level), wet);
        pud = puddle_amount(i.world, n, sr.ok ? sr.height : 0.5f, level, shelter);
        puddle_surface(albedo, rough, ns, i.world, pud, foot);
    }

    // Wet ground at the shore (water_wave.h's swash, the sheet the sea runs up
    // the beach). The band the uprush reaches every few sets stays damp --
    // water in the pores: darker, and smoother than dry sand -- and the strip
    // it has just drained from carries a film: glossy, its grain filled in.
    // Before, the sand was dry to the still-water line in a gale as in a calm.
    if (l_swash.w > 0.5f) {
        const float above = i.world.y - l_swash.x;
        const float runup = l_swash.y;
        if (above > -1.5f && above < runup + 0.6f) {
            WwSwash sw;
            sw.amp[0] = l_swash_a.x; sw.amp[1] = l_swash_a.y; sw.amp[2] = l_swash_a.z; sw.amp[3] = l_swash_a.w;
            sw.amp[4] = l_swash_b.x; sw.amp[5] = l_swash_b.y; sw.amp[6] = l_swash_b.z; sw.amp[7] = l_swash_b.w;
            sw.runup = runup;
            const float now = ww_swash(i.world.x, i.world.z, l_swash.z, sw);
            // Damp to the highest run-up and a little over (spray, the
            // capillary rise in sand), fading over 40 cm: at 0.85 of it the
            // band ended about where the sheet already stood and barely showed.
            const float damp = saturate((runup + 0.2f - above) / 0.4f);
            const float film = above > now ? saturate(1.0f - (above - now) / 0.3f) : 1.0f;
            const float wet = max(damp, film);
            albedo *= lerp(1.0f, 0.55f, wet);
            rough = lerp(rough, min(rough, 0.35f), damp);
            rough = lerp(rough, 0.06f, film);
            ns = normalize(lerp(ns, n, 0.8f * film));
        }
    }

    // Sun shadow: from the terrain itself and from the trees (shadow.hlsli).
    // The geometric normal decides it and offsets the lookup; the scan's
    // normal shades.
    const float lam = dot(n, kSunDir);
    // No sun where the GEOMETRIC surface faces away from it, and a fade over
    // the last six degrees before: the shading normal is the scan's, and on
    // a slope turned a few degrees from a low sun it still caught the light.
    // The lookup used to be skipped there with visibility 1 -- harmless while
    // the shading normal was the mesh's, since N.L was <= 0 anyway -- so
    // every gentle dip facing away from the sun lit up as a golden oval,
    // unshadowed under a dark wood (fixed after the scans landed).
    // A canopy wraps the light round its crowns, so the shell looks up its
    // shadow a little past the ground's terminator.
    const float sf = (lam > 0.0f || (cw > 0.0f && lam > -0.3f)) ? shadow_factor(i.world, n) : 0.0f;
    const float sh = lam > 0.0f ? sf * psh * saturate(lam * 10.0f) : 0.0f;
    const Surface s = make_surface(albedo, rough, ns, v);
    // Direct and ambient apart (scene_out.hlsli): the post chain shades
    // each with its own occlusion. The scan's occlusion is ambient only.
    float3 dir = sun_light(s, sh);
    float3 amb = ambient_light(s, i.world) * ao + local_lights(s, i.world, i.clip);
    // Standing water's reflection leaves the ambient for the puddle mask
    // (scene_out.hlsli): the composite marches it through the depth buffer
    // and lays what it finds -- trees, terrain, clouds -- in place of this
    // sky, which stays the fallback where the ray leaves the screen. Taken
    // out exactly as ambient_light put it in, without the AO: a mirror is
    // not occluded by the creases round it.
    float4 pmask = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (pud > 0.0f) {
        const float nv = max(dot(ns, v), 1e-4f);
        const float3 env = env_brdf(float3(0.04f, 0.04f, 0.04f), rough, nv);
        const float3 refl = env * gi_specular(i.world, ns, v, rough) * pud;
        amb -= refl * ao;
        pmask = float4(refl, env.x * pud);
    }
    if (cw > 0.0f) {
        // The shell's sun comes from the FAR terrain shadow, which holds the
        // terrain and the lifted shell but no trees -- not from the cascades.
        // The shell already darkens itself for leaves shading leaves
        // (kShellSun), and the cascades also hold the real trees: the last
        // row of them cast their shadows onto the shell behind it, and the
        // double darkening drew a band between the trees and the shell.
        const float sf_shell = lam > -0.3f ? far_shadow(i.world) : 0.0f;
        const float3 ldir = leaf_sun_light(leaf_alb, leaf_n, v, 0.6f, 0.7f, sf_shell * shell_sun) * 0.85f;
        const float3 lamb = leaf_alb / kPi * gi_diffuse(i.world, leaf_n, v) * shell_sky;
        dir = lerp(dir, ldir, cw);
        amb = lerp(amb, lamb, cw);
        pmask *= 1.0f - cw;
    }
    // Lightning (ADR-005 C5): the ground, or the canopy shell as leaves.
    if (l_flash.w > 0.0f)
        amb += lerp(flash_light(s, i.world), flash_leaf(leaf_alb, leaf_n, v, i.world, 0.6f, 0.7f), cw);
    SceneOut o = scene_out(dir, amb, i.world, cam_pos, far_plane);
    o.puddle = pmask;
    return o;
}
