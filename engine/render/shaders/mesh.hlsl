// ---------------------------------------------------------------------------
// B2: draw the surface-nets mesh.
//
// Shades the same way the B1 raymarch does, on purpose. The two are rendered
// from the same camera and the only intended difference is that one marches
// the field and the other draws triangles built from it, so any difference in
// the image is a meshing difference and not a lighting one.
// ---------------------------------------------------------------------------

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
    float  wire;     float slot_vertices;  float2 pad1;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    nointerpolation float2 fade : TEXCOORD2;
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

float4 ps_main(VSOut i) : SV_Target {
    if (i.fade.x > 0.5f) {
        const float t = dither_threshold(i.clip.xy);
        if (i.fade.x < 1.5f) clip(i.fade.y - t);   // fading in
        else                 clip(t - i.fade.y);   // fading out
    }
    if (wire > 0.5f) return float4(1.0f, 0.25f, 0.65f, 1.0f);
    const float3 n = normalize(i.nrm);
    const float3 sun = normalize(float3(0.45f, 0.78f, 0.35f));
    const float  lam = saturate(dot(n, sun));
    const float  sky = saturate(n.y * 0.5f + 0.5f);

    float3 rock = float3(0.46f, 0.40f, 0.34f);
    rock = lerp(rock, float3(0.34f, 0.46f, 0.28f), saturate(n.y * n.y));

    float3 col = rock * (0.22f + 0.78f * lam) + float3(0.10f, 0.13f, 0.18f) * sky;

    const float dist = length(i.world - cam_pos);
    const float fog = saturate(dist / far_plane);
    col = lerp(col, float3(0.40f, 0.50f, 0.66f), fog * fog);

    return float4(col, 1.0f);
}
