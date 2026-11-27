// ---------------------------------------------------------------------------
// Forage plants (engine/props/forage.h): layer 10's berry plants, instanced
// on the terrain's root signature after the rocks, as the mushrooms are --
// the frame's constants, the shadow table and the local lights already
// bound, the instance range at root SRV t0.
//
// Leaves, stalks, sepals and petals are lit as leaves are (wrapped, lit
// through, trees.hlsl's leaf_light); a berry and a flower's middle by the
// ordinary BRDF, a berry glossy and seeded. Every piece double sided: the
// normal is turned to the eye. A dithered fade over the last 15% of the draw
// distance, flat, as the rocks fade.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/flash.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/wet.hlsli"

cbuffer MeshConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;  float far_plane;
    float  wire;     float slot_vertices;
    float  mesh_debug;
    float  texture_mode;
};

struct ForageInst {
    float3 pos;  float scale;
    float4 rot;               // quaternion
    float3 tint; float fade;  // the leaves' tint; the draw distance, flat metres
};
StructuredBuffer<ForageInst> g_forage : register(t0);

struct VSIn {
    float3 pos  : POSITION;
    float3 nrm  : NORMAL;
    float2 uv   : TEXCOORD0;
    float  kind : TEXCOORD1;
    float3 col  : COLOR0;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    float2 uv    : TEXCOORD2;
    nointerpolation float kind : TEXCOORD3;
    float3 col   : TEXCOORD4;
    nointerpolation float fade : TEXCOORD5;
};

float3 qrot(float4 q, float3 v) {
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

// A 4 x 4 ordered dither, by arithmetic (no indexed array: CLAUDE.md).
float forage_dither(float2 px) {
    const uint x = uint(px.x) & 3u, y = uint(px.y) & 3u;
    const uint lo = 2u * ((x ^ y) & 1u) + (y & 1u);
    const uint hi = 2u * (((x ^ y) >> 1u) & 1u) + ((y >> 1u) & 1u);
    return (float(4u * lo + hi) + 0.5f) / 16.0f;
}

VSOut vs_forage(VSIn v, uint iid : SV_InstanceID) {
    const ForageInst m = g_forage[iid];
    VSOut o;
    o.world = m.pos + qrot(m.rot, v.pos * m.scale);
    o.nrm = qrot(m.rot, v.nrm);
    o.clip = mul(float4(o.world, 1.0f), view_proj);
    o.uv = v.uv;
    o.kind = v.kind;
    // Leaves and stalks take the plant's own tint; berries and flowers keep theirs.
    o.col = v.kind < 1.5f || abs(v.kind - 3.0f) < 0.5f ? v.col * m.tint : v.col;
    o.fade = m.fade;
    return o;
}

SceneOut ps_forage(VSOut i) {
    const float fade = saturate((i.fade - length(cam_pos.xz - i.world.xz)) / (0.15f * i.fade));
    clip(fade - forage_dither(i.clip.xy));

    const float3 v = normalize(cam_pos - i.world);
    float3 n = normalize(i.nrm);
    if (dot(n, v) < 0.0f) n = -n;   // double sided
    const float3 ng = n;
    float3 albedo = i.col;
    float rough = 0.6f;
    const int kind = int(i.kind + 0.5f);
    if (kind == 0) {
        // A leaflet: a paler midrib and veins, darker towards the edge.
        const float across = abs(i.uv.x);
        const float vein = saturate(1.0f - across * 12.0f) + 0.35f * saturate(1.0f - abs(frac(i.uv.y * 4.0f + across * 2.0f) - 0.5f) * 10.0f) * (1.0f - across);
        albedo *= lerp(1.05f, 0.82f, across) * (1.0f + 0.25f * vein);
        rough = 0.55f;
    } else if (kind == 2) {
        // A berry: glossy, its seeds in offset rows, paler towards the shoulder.
        const float rows = 7.0f;
        const float2 g = float2(i.uv.x * 11.0f + 0.5f * floor(i.uv.y * rows), i.uv.y * rows);
        const float2 c = frac(g) - 0.5f;
        const float seed = saturate(1.0f - length(c * float2(1.0f, 1.3f)) * 5.0f) * saturate(i.uv.y * 6.0f);
        albedo = lerp(albedo * lerp(0.75f, 1.0f, saturate(i.uv.y * 3.0f)), float3(0.80f, 0.62f, 0.22f), seed);
        rough = lerp(0.28f, 0.6f, seed);
    } else if (kind == 5) {
        rough = 0.7f;
    }
    const float wet = wet_at(i.world, ng);
    float3 ns = n;
    wet_surface(albedo, rough, ns, ng, kind == 2 ? 0.1f : 0.5f, wet);

    const float lam = dot(ng, kSunDir);
    const float sh = shadow_factor(i.world, ng) * saturate(abs(lam) * 10.0f + 0.2f);
    const Surface s = make_surface(albedo, rough, ns, v);
    float3 direct, local;
    if (kind == 2 || kind == 5) {
        direct = sun_light(s, lam > 0.0f ? sh : 0.0f);
        local = local_lights(s, i.world, i.clip);
    } else {
        direct = leaf_sun_light(albedo, ns, v, 0.35f, 0.5f, sh);
        local = local_lights_leaf(albedo, ns, v, 0.35f, 0.5f, i.world, i.clip);
    }
    return scene_out(direct, ambient_light(s, i.world) + local + flash_light(s, i.world),
                     i.world, cam_pos, far_plane, kind == 2 ? 1.0f : 0.3f, 0.7f);
}
