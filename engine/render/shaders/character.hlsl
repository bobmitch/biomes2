// ---------------------------------------------------------------------------
// The player's character: a skinned mesh (engine/character/). Drawn with the
// terrain's root signature right after the rocks, so the frame's constants,
// the shadow constants and the shadow table are already bound -- the bone
// matrices arrive as root SRV t0 (model -> world, 3 x 4 rows, one per joint)
// and the character's textures through the table's range at t49..t50.
//
// Four joints a vertex, linear blend. Shaded as everything else is: the same
// sun through the cascades, sky and bounce, local lights, the flash and the
// wet world. No tangents in the vertex: the normal map's frame comes from the
// screen-space derivatives of position and uv, as the bark's does.
// ---------------------------------------------------------------------------

#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/wet.hlsli"
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

struct Bone { float4 r0, r1, r2; };
StructuredBuffer<Bone> g_bones : register(t0);

// Per material (layer): albedo (sRGB), and normal x, normal y DOWN the
// texture, roughness (tools/models/pack_character.py).
Texture2DArray<float4> g_char_a : register(t49);
Texture2DArray<float4> g_char_b : register(t50);
SamplerState           g_char_samp : register(s5);   // anisotropic, wrap

struct VSIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float2 uv  : TEXCOORD0;
    uint4  jnt : BLENDINDICES;
    float4 wgt : BLENDWEIGHT;
    uint   mat : TEXCOORD1;
};

struct VSOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    float2 uv    : TEXCOORD2;
    nointerpolation uint mat : TEXCOORD3;
};

void skin(VSIn v, out float3 p, out float3 n) {
    float4 r0 = 0.0f, r1 = 0.0f, r2 = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        const Bone b = g_bones[v.jnt[k]];
        r0 += b.r0 * v.wgt[k];
        r1 += b.r1 * v.wgt[k];
        r2 += b.r2 * v.wgt[k];
    }
    const float4 h = float4(v.pos, 1.0f);
    p = float3(dot(r0, h), dot(r1, h), dot(r2, h));
    n = float3(dot(r0.xyz, v.nrm), dot(r1.xyz, v.nrm), dot(r2.xyz, v.nrm));
}

VSOut vs_character(VSIn v) {
    VSOut o;
    skin(v, o.world, o.nrm);
    o.clip = mul(float4(o.world, 1.0f), view_proj);
    o.uv = v.uv;
    o.mat = v.mat;
    return o;
}

float4 vs_character_shadow(VSIn v) : SV_Position {
    float3 p, n;
    skin(v, p, n);
    return mul(float4(p, 1.0f), view_proj);
}

// A local light's shadow face (Renderer::record_lights). The caller puts
// the light's position in cam_pos and a clear radius in far_plane: the
// carried torch sits just over the fist, and the hand round it would black
// out its whole light, so nothing of the body within that radius of the
// flame casts. 0 for every other light.
struct LightShadowOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
};

LightShadowOut vs_character_light_shadow(VSIn v) {
    LightShadowOut o;
    float3 n;
    skin(v, o.world, n);
    o.clip = mul(float4(o.world, 1.0f), view_proj);
    return o;
}

void ps_character_light_shadow(LightShadowOut i) {
    const float3 d = i.world - cam_pos;
    if (dot(d, d) < far_plane * far_plane) discard;
}

SceneOut ps_character(VSOut i, bool front : SV_IsFrontFace) {
    // Every material in the kit is double sided, and the draw culls nothing:
    // a back face takes the other side's normal.
    float3 n = normalize(i.nrm) * (front ? 1.0f : -1.0f);
    const float3 v = normalize(cam_pos - i.world);
    const float3 layer = float3(i.uv, float(i.mat));
    const float4 a = g_char_a.Sample(g_char_samp, layer);
    const float4 b = g_char_b.Sample(g_char_samp, layer);
    // The normal map, in a frame from the derivatives (Schueler).
    const float3 dp1 = ddx(i.world), dp2 = ddy(i.world);
    const float2 du1 = ddx(i.uv), du2 = ddy(i.uv);
    const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
    const float3 T = dp2perp * du1.x + dp1perp * du2.x;
    const float3 B = dp2perp * du1.y + dp1perp * du2.y;
    const float tb = max(dot(T, T), dot(B, B));
    float3 ns = n;
    if (tb > 1e-24f) {
        const float inv = rsqrt(tb);
        const float2 tn = b.xy * 2.0f - 1.0f;
        const float tz = sqrt(saturate(1.0f - dot(tn, tn)));
        const float3 p = T * inv * tn.x + B * inv * tn.y + n * tz;
        if (dot(p, p) > 1e-8f) ns = normalize(p);
    }
    float3 albedo = a.rgb;
    float rough = clamp(b.z, 0.08f, 1.0f);
    // Rain: cloth and skin are porous-ish; between soil and rock.
    wet_surface(albedo, rough, ns, n, 0.6f, wet_at(i.world, n));
    const float lam = dot(n, kSunDir);
    const float sh = lam > 0.0f ? shadow_factor(i.world, n) * saturate(lam * 10.0f) : 0.0f;
    const Surface s = make_surface(albedo, rough, ns, v);
    return scene_out(sun_light(s, sh), ambient_light(s, i.world) + local_lights(s, i.world, i.clip) +
                     flash_light(s, i.world),
                     i.world, cam_pos, far_plane);
}
