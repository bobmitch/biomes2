#ifndef FOREST_HLSLI
#define FOREST_HLSLI
// ---------------------------------------------------------------------------
// The canopy shell (forest_map.h): past the trees' draw distance the terrain
// is lifted to the canopy and shaded as canopy wherever the forest map says
// trees stand. mesh.hlsl's vertex shader lifts -- in every pass that draws
// the terrain, so the cascades, the far shadow and the heightfield's top-down
// map all see it -- and its pixel shader shades. Needs the shadow constants
// (shadow.hlsli) and the linear sampler (ddgi.hlsli) first.
// ---------------------------------------------------------------------------

Texture2D<float4> g_forest_a : register(t47);   // cover, canopy height, conifer share
Texture2D<float4> g_forest_b : register(t48);   // leaf albedo, linear

struct Forest {
    float  cover;     // canopy fraction, 0..1
    float  height;    // canopy height, metres
    float  conifer;
    float3 leaf;      // linear albedo
    float  w;         // the shell's weight here: 0 inside the trees' reach
};

// How far up the canopy's surface sits, as a share of the trees' height.
static const float kShellLift = 0.85f;
// The canopy's mean light: the share of the sun and of the sky a crown
// surface takes, the rest lost to leaves shading leaves. Matched over the
// shell's own footprint against the real trees drawn to 2 km (no mist, EV100
// 14, 16:00, from 260 m up): real (88.9, 114.8, 71.1), shell (85.3, 114.9,
// 63.5). Shaded as leaves are (mesh.hlsl), the trees' own leaf albedo needs
// no tint; on the ground's BRDF it read pale and blue.
static const float kShellSun = 0.7f;
static const float kShellSky = 0.85f;

Forest forest_at(float3 world) {
    Forest f = (Forest)0;
    if (l_forest.w < 0.5f) return f;
    // Horizontal distance from the camera: the band follows the trees'
    // draw distance (l_forest2.xy, metres).
    const float d = length(world.xz - shadow_cam.xz);
    f.w = saturate((d - l_forest2.x) / (l_forest2.y - l_forest2.x));
    if (f.w <= 0.0f) return f;
    const float2 uv = (world.xz - l_forest.xy) / l_forest.z;
    if (any(uv < 0.0f) || any(uv > 1.0f)) { f.w = 0.0f; return f; }
    const float4 a = g_forest_a.SampleLevel(g_ddgi_linear, uv, 0.0f);
    const float4 b = g_forest_b.SampleLevel(g_ddgi_linear, uv, 0.0f);
    // Cover through a contrast curve: a wood's edge is a few metres wide and
    // the map's texels are twenty, so bilinear cover blurred every edge and
    // clump the real trees draw crisply.
    f.cover = smoothstep(0.15f, 0.55f, saturate(a.x));
    f.height = a.y;
    f.conifer = a.z;
    // The leaf colour is averaged over trees; where cover is thin it was
    // averaged with nothing, so normalise it back by what was there.
    f.leaf = b.rgb;
    return f;
}

// The lift at a vertex: the canopy's top where the ground faces up.
float forest_lift(float3 world, float3 nrm) {
    const Forest f = forest_at(world);
    if (f.w <= 0.0f || f.cover <= 0.0f) return 0.0f;
    const float up = saturate((normalize(nrm).y - 0.55f) / 0.3f);
    return f.cover * f.height * kShellLift * f.w * up;
}

#endif
