// ---------------------------------------------------------------------------
// Rocks (layer 9): boulders and outcrops, instanced. Drawn with the terrain's
// root signature right after the terrain, so the frame's constants, the
// shadow table and the scans are already bound; the instance range arrives
// as root SRV t0, rebound per draw at an offset (rock_renderer.cpp).
//
// Shading is the terrain's: the biome's rock scan, triplanar in world space,
// with parallax and its self-shadow (scan.hlsli), retinted to the authored
// rock colour; the same sun, shadows, ambient and local lights.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/scan.hlsli"

cbuffer MeshConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;  float far_plane;
    float  wire;     float slot_vertices;
    float  mesh_debug;
    float  texture_mode;
};

struct RockInst {
    float3 pos;  float scale;
    float4 rot;               // quaternion
    uint   biome;  float draw_dist;  float2 pad;
};
StructuredBuffer<RockInst> g_rocks : register(t0);

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    nointerpolation uint  biome : TEXCOORD2;
    nointerpolation float draw_dist : TEXCOORD3;
};

float3 qrot(float4 q, float3 v) {
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

VSOut vs_rock(VSIn v, uint iid : SV_InstanceID) {
    const RockInst r = g_rocks[iid];
    VSOut o;
    o.world = r.pos + qrot(r.rot, v.pos * r.scale);
    o.nrm = qrot(r.rot, v.nrm);
    o.clip = mul(float4(o.world, 1.0f), view_proj);
    o.biome = r.biome;
    o.draw_dist = r.draw_dist;
    return o;
}

float4 vs_rock_shadow(VSIn v, uint iid : SV_InstanceID) : SV_Position {
    const RockInst r = g_rocks[iid];
    return mul(float4(r.pos + qrot(r.rot, v.pos * r.scale), 1.0f), view_proj);
}

float rock_dither(float2 px) {
    const uint x = uint(px.x) & 3u, y = uint(px.y) & 3u;
    const uint m[16] = {0u, 8u, 2u, 10u, 12u, 4u, 14u, 6u, 3u, 11u, 1u, 9u, 15u, 7u, 13u, 5u};
    return (float(m[y * 4u + x]) + 0.5f) / 16.0f;
}

// The heightfield GI's top-down view (height_gi.h): the rock's albedo as
// lighting sees it -- the scan's smallest mip, its mean, times the retint,
// which is the authored rock colour exactly.
float4 ps_rock_top(VSOut i) : SV_Target0 {
    const ScanMaterial M = g_scan_mat[i.biome];
    float3 albedo = float3(0.16f, 0.15f, 0.13f);
    if (M.rock_layer >= 0.0f)
        albedo = g_scan_a.SampleLevel(g_scan_samp, float3(0.5f, 0.5f, M.rock_layer), 16.0f).rgb * M.rock_tint;
    return float4(albedo, 1.0f);
}

SceneOut ps_rock(VSOut i) {
    const float dist = length(cam_pos - i.world);
    // A dithered fade over the last 12% of the draw distance, measured flat
    // like the draw distance itself (rock_renderer.cpp).
    const float fade = saturate((i.draw_dist - length(cam_pos.xz - i.world.xz)) / (0.12f * i.draw_dist));
    clip(fade - rock_dither(i.clip.xy));

    const float3 n = normalize(i.nrm);
    const float3 v = normalize(cam_pos - i.world);
    const ScanMaterial M = g_scan_mat[i.biome];
    float3 albedo = float3(0.16f, 0.15f, 0.13f);   // no scan: a plain dark rock
    float rough = M.rock_rough > 0.0f ? M.rock_rough : 0.8f;
    float3 ns = n;
    float ao = 1.0f;
    float psh = 1.0f;
    const bool scans = (texture_mode > 0.5f && texture_mode < 1.5f) || texture_mode > 2.5f;
    if (scans && M.rock_layer >= 0.0f) {
        const float3 dpx = ddx(i.world), dpy = ddy(i.world);
        float3 bw = abs(n);
        bw = bw * bw * bw * bw;
        bw /= (bw.x + bw.y + bw.z);
        const float3 p = i.world + scan_pom_shift(M.rock_layer, M.rock_inv_tile, M.rock_depth, i.world, n, v,
                                                  bw, dpx, dpy, dist, psh);
        const ScanSample s = scan_triplanar(M.rock_layer, M.rock_inv_tile, p, n, bw, dpx, dpy);
        albedo = s.albedo * M.rock_tint;
        rough = saturate(s.rough * M.rock_rough / M.rock_scan_rough);
        ns = normalize(s.n);
        ao = s.ao;
    }
    const float lam = dot(n, kSunDir);
    // No sun where the GEOMETRIC surface faces away from it, and a fade over
    // the last six degrees before: the shading normal is the scan's, and on
    // a slope turned a few degrees from a low sun it still caught the light.
    // The lookup used to be skipped there with visibility 1 -- harmless while
    // the shading normal was the mesh's, since N.L was <= 0 anyway -- so
    // every gentle dip facing away from the sun lit up as a golden oval,
    // unshadowed under a dark wood (fixed after the scans landed).
    const float sh = lam > 0.0f ? shadow_factor(i.world, n) * psh * saturate(lam * 10.0f) : 0.0f;
    const Surface s = make_surface(albedo, rough, ns, v);
    return scene_out(sun_light(s, sh), ambient_light(s, i.world) * ao + local_lights(s, i.world, i.clip),
                     i.world, cam_pos, far_plane);
}
