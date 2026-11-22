// ---------------------------------------------------------------------------
// Shading by local lights (ADR-004 L3): a pixel walks the lights of its
// froxel cluster (lights.hlsl) and adds each one's light through the same
// BRDF the sun uses (lighting.hlsli), shadowed by the light atlas where the
// light has a shadow.
//
// Include after shadow.hlsli (the cluster parameters and the camera ride in
// ShadowConstants) and lighting.hlsli. The buffers sit at fixed slots of the
// renderer's shader-visible heap, reached through a second range of the
// table every lit pipeline already binds for the sun's shadow map: no root
// parameter changed.
//
// The light goes into the AMBIENT target, so GTAO darkens it in creases. For
// a point light that is not quite right -- AO is meant for light from the
// whole sky -- and it is the cheapest honest place for it until the post
// chain has a third target to keep it in.
// ---------------------------------------------------------------------------
#ifndef ENGINE_LOCAL_LIGHTS_HLSLI
#define ENGINE_LOCAL_LIGHTS_HLSLI

struct LocalLightGpu {
    float3 pos;      float range;
    float3 intensity; float type;      // candela per channel; 0 point, 1 spot
    float3 spot_dir; float cos_outer;
    float  cos_inner; int shadow;      // first face in g_light_faces, or -1
    float2 pad;
};

struct LightFaceGpu {
    row_major float4x4 view_proj;
    float4 rect;                       // atlas uv: x, y, size, one texel
};

StructuredBuffer<LocalLightGpu> g_lights      : register(t20);
StructuredBuffer<uint>          g_cluster_n   : register(t21);
StructuredBuffer<uint>          g_cluster_ix  : register(t22);
Texture2D<float>                g_light_atlas : register(t23);
StructuredBuffer<LightFaceGpu>  g_light_faces : register(t24);

static const uint kClusterMaxItems = 128;  // local_lights.h, lights.hlsl

uint cluster_of(float3 world, float4 clip) {
    const uint nx = uint(l_cluster.x), ny = uint(l_cluster.y), nz = uint(l_cluster.z);
    const uint tx = min(uint(clip.x * l_cluster.x / l_cluster2.z), nx - 1u);
    const uint ty = min(uint(clip.y * l_cluster.y / l_cluster2.w), ny - 1u);
    const float z = max(dot(world - shadow_cam, l_cam_fwd.xyz), l_cluster2.x);
    const uint tz = min(uint(log(z / l_cluster2.x) * l_cluster2.y), nz - 1u);
    return (tz * ny + ty) * nx + tx;
}

// Illuminance (lux) the light puts on a surface facing it, and the unit
// vector towards it. Inverse square, windowed to zero at the range
// (Karis/Frostbite: (1 - (d/r)^4)^2), never closer than 10 cm.
float3 light_illuminance(LocalLightGpu L, float3 world, out float3 l) {
    const float3 v = L.pos - world;
    const float d2 = max(dot(v, v), 1e-6f);
    l = v * rsqrt(d2);
    const float x = d2 / (L.range * L.range);
    const float w = saturate(1.0f - x * x);
    float3 E = L.intensity * (w * w / max(d2, 0.01f));
    if (L.type > 0.5f) E *= smoothstep(L.cos_outer, L.cos_inner, dot(-l, L.spot_dir));
    return E;
}

// The light's visibility at `world`, from its cube (or, for a spot, its one
// face) in the atlas. 2x2 PCF through the comparison sampler, clamped
// inside the tile so a neighbour's depths never bleed in.
float local_shadow_taps(LocalLightGpu L, float3 world, float3 n, float3 l, bool one_tap) {
    if (L.shadow < 0) return 1.0f;
    const float3 d = world - L.pos;
    const float3 a = abs(d);
    uint face = 0u;
    if (L.type < 0.5f) {
        if (a.x >= a.y && a.x >= a.z) face = d.x > 0.0f ? 0u : 1u;
        else if (a.y >= a.z)          face = d.y > 0.0f ? 2u : 3u;
        else                          face = d.z > 0.0f ? 4u : 5u;
    }
    const LightFaceGpu f = g_light_faces[uint(L.shadow) + face];
    // A texel's world size at this distance (a ~95 degree face over 512
    // texels), for a normal offset of a texel and a half and a push towards
    // the light: what stops a surface shadowing itself.
    const float texel = 2.2f * length(d) / 512.0f;
    const float3 p = world + n * (1.5f * texel) + l * (0.02f + texel);
    const float4 c = mul(float4(p, 1.0f), f.view_proj);
    if (c.w <= 0.0f) return 1.0f;
    const float3 ndc = c.xyz / c.w;
    const float2 tuv = float2(ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f);
    const float m = 1.5f / 512.0f;
    const float2 uv = f.rect.xy + clamp(tuv, m, 1.0f - m) * f.rect.z;
    const float t = f.rect.w;
    if (one_tap) return g_light_atlas.SampleCmpLevelZero(g_shadow_cmp, uv, ndc.z);
    float s = 0.0f;
    s += g_light_atlas.SampleCmpLevelZero(g_shadow_cmp, uv + float2(-0.5f, -0.5f) * t, ndc.z);
    s += g_light_atlas.SampleCmpLevelZero(g_shadow_cmp, uv + float2( 0.5f, -0.5f) * t, ndc.z);
    s += g_light_atlas.SampleCmpLevelZero(g_shadow_cmp, uv + float2(-0.5f,  0.5f) * t, ndc.z);
    s += g_light_atlas.SampleCmpLevelZero(g_shadow_cmp, uv + float2( 0.5f,  0.5f) * t, ndc.z);
    return s * 0.25f;
}
float local_shadow(LocalLightGpu L, float3 world, float3 n, float3 l) {
    return local_shadow_taps(L, world, n, l, false);
}

// A light's bounce, instantly: a soft fill, kLocalFill of what the light
// puts at a distance of at least kFillNear (the fill is room-scale; the
// inverse square at the flame is not), wrapped round the normal, diffuse
// only. Rock bounces a quarter to a third of what lands on it in an
// enclosure; 0.2 is a little under. It came through the near field's
// probes before, which faded it in over seconds and pulsed (near_field.hlsl).
// Its visibility is the light's own shadow, tested from a point moved
// towards the light by up to kFillReach: it wraps round a pillar or a
// person, never through rock thicker than that. A light with no shadow
// map lights through everything, fill included.
static const float kLocalFill = 0.2f;
static const float kFillNear = 1.5f;
static const float kFillReach = 1.5f;
float3 local_fill_irradiance(LocalLightGpu L, float3 world, float3 n) {
    const float3 v = L.pos - world;
    const float d2 = max(dot(v, v), 1e-6f);
    const float d = sqrt(d2);
    const float3 l = v / d;
    const float x = d2 / (L.range * L.range);
    const float w = saturate(1.0f - x * x);
    if (w <= 0.0f) return float3(0.0f, 0.0f, 0.0f);
    float3 E = L.intensity * (w * w / max(d2, kFillNear * kFillNear));
    if (L.type > 0.5f) E *= smoothstep(L.cos_outer, L.cos_inner, dot(-l, L.spot_dir));
    const float3 q = world + l * min(kFillReach, 0.5f * d);
    // One tap: the fill is soft; the direct light's 2x2 PCF here cost 1.2 ms
    // with 64 campfires.
    const float vis = local_shadow_taps(L, q, l, l, true);
    return E * (kLocalFill * vis * (0.5f + 0.5f * dot(n, l)));
}

// Radiance from every local light in this pixel's cluster: direct, and its
// fill (diffuse, so the albedo over pi).
float3 local_lights(Surface s, float3 world, float4 clip) {
    // No lights this frame -- a uniform branch, the whole draw takes it --
    // and no cluster is read at all: walking by day, 2.26 -> 2.18 ms (L2: 2.10).
    if (l_cluster.w < 0.5f) return float3(0.0f, 0.0f, 0.0f);
    const uint c = cluster_of(world, clip);
    const uint n = g_cluster_n[c];
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    [loop] for (uint i = 0; i < n; ++i) {
        const LocalLightGpu L = g_lights[g_cluster_ix[c * kClusterMaxItems + i]];
        float3 l;
        const float3 E = light_illuminance(L, world, l);
        if (max(E.r, max(E.g, E.b)) <= 0.0f) continue;
        acc += s.albedo * (1.0f - s.metal) / kPi * local_fill_irradiance(L, world, s.n);
        if (dot(s.n, l) <= 0.0f) continue;
        acc += direct_light(s, l, E, local_shadow(L, world, s.n, l));
    }
    return acc;
}

// The same for leaves and blades: wrapped, and lit through.
float3 local_lights_leaf(float3 albedo, float3 n, float3 v, float wrap, float through,
                         float3 world, float4 clip) {
    if (l_cluster.w < 0.5f) return float3(0.0f, 0.0f, 0.0f);
    const uint c = cluster_of(world, clip);
    const uint cnt = g_cluster_n[c];
    float3 acc = float3(0.0f, 0.0f, 0.0f);
    [loop] for (uint i = 0; i < cnt; ++i) {
        const LocalLightGpu L = g_lights[g_cluster_ix[c * kClusterMaxItems + i]];
        float3 l;
        const float3 E = light_illuminance(L, world, l);
        if (max(E.r, max(E.g, E.b)) <= 0.0f) continue;
        acc += leaf_light(albedo, n, v, l, E, wrap, through, local_shadow(L, world, n, l));
        acc += albedo / kPi * local_fill_irradiance(L, world, n);
    }
    return acc;
}

#endif
