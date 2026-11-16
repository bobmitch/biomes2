// ---------------------------------------------------------------------------
// The far terrain shadow's march (far_shadow.h): per texel of an 8 km
// top-down height map, towards the key light, the least clearance any
// terrain leaves the ray, softened by the penumbra a 0.53-degree sun casts
// at that distance -- 1 lit, 0 in shadow.
// ---------------------------------------------------------------------------

cbuffer FarConstants : register(b0) {
    float2 origin_xz;  float y_top;   float y_range;
    float3 key_dir;    float extent;
};

Texture2D<float>   g_depth : register(t0);
SamplerState       g_clamp : register(s0);
RWTexture2D<float> u_vis   : register(u0);

static const uint  kSize = 1024u;
static const float kNone = -100000.0f;

float height_at(float2 uv) {
    const float d = g_depth.SampleLevel(g_clamp, uv, 0.0f);
    return d >= 1.0f ? kNone : y_top - d * y_range;
}

[numthreads(8, 8, 1)]
void cs_far_march(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kSize || id.y >= kSize) return;
    const float texel = extent / float(kSize);
    const float d0 = g_depth.Load(int3(id.xy, 0));
    if (d0 >= 1.0f) { u_vis[id.xy] = 1.0f; return; }
    const float h = y_top - d0 * y_range;
    if (key_dir.y <= 0.0f) { u_vis[id.xy] = 0.0f; return; }   // the key light is below the horizon
    const float2 xz = float2(origin_xz.x + (float(id.x) + 0.5f) * texel,
                             origin_xz.y + extent - (float(id.y) + 0.5f) * texel);
    const float lh = length(key_dir.xz);
    if (lh < 1e-4f) { u_vis[id.xy] = 1.0f; return; }
    const float2 dir = key_dir.xz / lh;
    const float rise = key_dir.y / lh;
    float vis = 1.0f;
    float d = texel;
    // Out to ~3.6 km in steps that grow 13% each: 50 taps. A texel's own
    // height error is up to half a texel of slope, so the start sits a
    // couple of metres up.
    [loop] for (int k = 0; k < 50 && vis > 0.0f; ++k) {
        d *= 1.13f;
        const float2 q = xz + dir * d;
        const float2 uv = float2((q.x - origin_xz.x) / extent, 1.0f - (q.y - origin_xz.y) / extent);
        if (any(uv < 0.0f) || any(uv > 1.0f)) break;
        const float hs = height_at(uv);
        if (hs < kNone * 0.5f) continue;
        const float ray = h + 2.0f + d * rise;
        // The sun's penumbra at distance d (0.0093 rad), and the map's
        // texel, set how gradually a ridge closes the light.
        vis = min(vis, saturate((ray - hs) / (0.0093f * d + texel)));
    }
    u_vis[id.xy] = vis;
}
