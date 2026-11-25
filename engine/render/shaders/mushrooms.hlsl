// ---------------------------------------------------------------------------
// Glowing mushrooms in the caves (engine/props/glow_mushrooms.h), instanced
// on the terrain's root signature after the rocks, as the rocks are: the
// frame's constants, the shadow table and the local lights already bound,
// the instance range at root SRV t0.
//
// Lit by the sun, the sky and the bounce (the near field's probes, under
// cover), plus their glow, which goes into the AMBIENT target with no ambient
// occlusion: it is light leaving the surface, and GTAO would darken a gill in
// its own crease. NOT by the local lights: their own cluster's light stands
// a few tens of centimetres over the caps and burnt them to white -- it IS
// their glow. (So a torch held to them does not light them either.)
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/flash.hlsli"
#include "engine/render/shaders/local_lights.hlsli"

cbuffer MeshConstants : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;  float far_plane;
    float  wire;     float slot_vertices;
    float  mesh_debug;
    float  texture_mode;
};

struct MushInst {
    float3 pos;  float scale;
    float4 rot;               // quaternion
    float3 glow; float pad;   // nits at full glow, linear rgb
};
StructuredBuffer<MushInst> g_mush : register(t0);

struct VSIn {
    float3 pos  : POSITION;
    float3 nrm  : NORMAL;
    float  emit : TEXCOORD0;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    float  emit  : TEXCOORD2;
    nointerpolation float3 glow : TEXCOORD3;
};

float3 qrot(float4 q, float3 v) {
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

VSOut vs_mushroom(VSIn v, uint iid : SV_InstanceID) {
    const MushInst m = g_mush[iid];
    VSOut o;
    o.world = m.pos + qrot(m.rot, v.pos * m.scale);
    o.nrm = qrot(m.rot, v.nrm);
    o.clip = mul(float4(o.world, 1.0f), view_proj);
    o.emit = v.emit;
    o.glow = m.glow;
    return o;
}

SceneOut ps_mushroom(VSOut i) {
    const float3 v = normalize(cam_pos - i.world);
    float3 n = normalize(i.nrm);
    if (dot(n, v) < 0.0f) n = -n;   // double sided: a cap seen from under, a gill from over
    // Pale, a little blue where it glows most: the flesh the light comes through.
    const float3 albedo = lerp(float3(0.42f, 0.44f, 0.46f), float3(0.30f, 0.42f, 0.55f), saturate(i.emit));
    const Surface s = make_surface(albedo, 0.55f, n, v);
    const float lam = dot(n, kSunDir);
    const float sh = lam > 0.0f ? shadow_factor(i.world, n) * saturate(lam * 10.0f) : 0.0f;
    // The glow: brightest seen face-on to the gills, a soft rim otherwise.
    const float3 glow = i.glow * i.emit;
    return scene_out(sun_light(s, sh),
                     ambient_light(s, i.world) + flash_light(s, i.world) + glow,
                     i.world, cam_pos, far_plane, 1.0f, 0.0f);
}
