// ---------------------------------------------------------------------------
// B2: draw the surface-nets mesh.
//
// Shades the same way the B1 raymarch does, on purpose. The two are rendered
// from the same camera and the only intended difference is that one marches
// the field and the other draws triangles built from it, so any difference in
// the image is a meshing difference and not a lighting one.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"

struct Vertex {
    float3 pos;
    float3 nrm;
};

StructuredBuffer<Vertex> g_vertices : register(t0);
// Per-slot LOD cross-fade: (mode, v). 0 drawn; 1 fading in, drawn where the
// dither threshold is below v; 2 fading out, drawn where it is at or above v.
// Streamer::fill_visibility writes it.
StructuredBuffer<float2> g_fade : register(t1);

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
};

// No input layout. With an index buffer bound, SV_VertexID is already the
// index-buffer-resolved vertex, so the attributes can come straight out of the
// buffer the mesher wrote -- there is no vertex struct to keep in sync across
// the C++/HLSL boundary, which is one fewer thing that can silently disagree.
VSOut vs_main(uint vid : SV_VertexID) {
    const Vertex v = g_vertices[vid];
    VSOut o;
    o.clip = mul(float4(v.pos, 1.0f), view_proj);
    o.world = v.pos;
    o.nrm = v.nrm;
    // Indices are absolute, so the vertex id says which slot this is.
    o.fade = g_fade[vid / uint(slot_vertices)];
    // Exact: the kernel stores a skirt vertex's normal at half length. A
    // surface triangle is 0 everywhere; a curtain triangle is not.
    o.skirt = dot(v.nrm, v.nrm) < 0.5f ? 1.0f : 0.0f;
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

float4 ps_main(VSOut i) : SV_Target {
    if (i.fade.x > 0.5f) {
        const float t = dither_threshold(i.clip.xy);
        if (i.fade.x < 1.5f) clip(i.fade.y - t);   // fading in
        else                 clip(t - i.fade.y);   // fading out
    }
    if (wire > 0.5f) return float4(1.0f, 0.25f, 0.65f, 1.0f);
    // Debug views. The first attempt at "where is the curtain visible" tested
    // the interpolated normal's length, which a sharp CREASE also shortens --
    // it counted the terrain's ridge lines as seams and sent a whole
    // investigation the wrong way. The skirt flag is exact; the crease view
    // is the normal-length test, but only where the flag is zero.
    if (mesh_debug > 0.5f && mesh_debug < 1.5f && i.skirt > 0.001f)
        return float4(1.0f, 0.0f, 1.0f, 1.0f);
    if (mesh_debug > 1.5f && i.skirt <= 0.001f && length(i.nrm) < 0.9f)
        return float4(0.0f, 1.0f, 1.0f, 1.0f);
    const float3 n = normalize(i.nrm);
    const float3 sun = normalize(float3(0.45f, 0.78f, 0.35f));
    const float  lam = saturate(dot(n, sun));
    const float  sky = saturate(n.y * 0.5f + 0.5f);

    float3 rock = float3(0.46f, 0.40f, 0.34f);
    rock = lerp(rock, float3(0.34f, 0.46f, 0.28f), saturate(n.y * n.y));
    rock = apply_surface_texture(rock, i.world, n);

    // Sun shadow: from the terrain itself and from the trees (shadow.hlsli).
    const float sh = lam > 0.0f ? shadow_factor(i.world, n) : 1.0f;
    float3 col = rock * (0.22f + 0.78f * lam * sh) + float3(0.10f, 0.13f, 0.18f) * sky;

    const float dist = length(i.world - cam_pos);
    const float fog = saturate(dist / far_plane);
    col = lerp(col, float3(0.40f, 0.50f, 0.66f), fog * fog);

    return float4(col, 1.0f);
}
