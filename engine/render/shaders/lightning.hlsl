// ---------------------------------------------------------------------------
// The lightning bolt (ADR-005 C5; lightning.h): the channel's segments, each
// drawn as two camera-facing quads -- a white-hot core at least ~1.2 px wide
// and a glow ten times wider -- added onto the lit scene after the rain and
// before TAA. Depth tested by hand against the scene (a hill in front hides
// it), dimmed by the air over kilometres.
// ---------------------------------------------------------------------------

cbuffer LightningConstants : register(b0) {
    row_major float4x4 view_proj;   // this frame, jittered
    float3 cam_pos;     float channel;      // 0..1 how bright the channel is now
    float  near_plane;  float far_plane;   float pixel_angle;  float count;
    float2 inv_size;    float2 pad;
};

struct Seg { float3 a; float bright; float3 b; float width; };
Texture2D<float>       g_depth : register(t0);
StructuredBuffer<Seg>  g_segs  : register(t1);

struct BoltOut {
    float4 clip : SV_Position;
    float  x    : TEXCOORD0;    // across, -1..1
    float  glow : TEXCOORD1;    // 0 core, 1 glow
    float  k    : TEXCOORD2;    // brightness: the segment's x the air's
};

BoltOut vs_bolt(uint vid : SV_VertexID) {
    const uint si = vid / 12u, layer = (vid % 12u) / 6u, corner = vid % 6u;
    const Seg s = g_segs[si];
    const float3 mid = 0.5f * (s.a + s.b);
    const float dist = length(mid - cam_pos);
    float3 along = s.b - s.a;
    const float len = length(along);
    along /= max(len, 1e-3f);
    const float3 side = normalize(cross(along, mid - cam_pos));
    // The core never thinner than ~1.2 px; the glow ten times the core.
    const float core = max(s.width, dist * pixel_angle * 1.2f);
    const float w = layer == 0u ? core : core * 10.0f;
    const float2 c = corner == 0u ? float2(-1, 0) : corner == 1u ? float2(1, 0) : corner == 2u ? float2(-1, 1)
                   : corner == 3u ? float2(1, 0) : corner == 4u ? float2(1, 1) : float2(-1, 1);
    // Each quad overlaps its neighbours by half a width so the joints close.
    const float3 p = lerp(s.a - along * (0.5f * w), s.b + along * (0.5f * w), c.y) + side * (c.x * 0.5f * w);
    BoltOut o;
    o.clip = mul(float4(p, 1.0f), view_proj);
    o.x = c.x;
    o.glow = float(layer);
    o.k = s.bright * exp(-dist / 12000.0f) * (s.width / max(core, 1e-3f) * 0.5f + 0.5f);
    return o;
}

float linear_depth(float z) {
    return near_plane * far_plane / (far_plane - z * (far_plane - near_plane));
}

float4 ps_bolt(BoltOut i) : SV_Target0 {
    const float z = g_depth.Load(int3(int2(i.clip.xy), 0));
    if (z < 1.0f && linear_depth(z) < i.clip.w) discard;
    const float x2 = i.x * i.x;
    // Radiance in nits: the core far past white, the glow a halo round it.
    const float core = (1.0f - x2) * (1.0f - x2) * 3e6f;
    const float glow = exp(-x2 * 5.0f) * 2.5e4f;
    const float L = (i.glow > 0.5f ? glow : core) * i.k * channel;
    return float4(float3(0.85f, 0.9f, 1.0f) * L, 0.0f);
}
