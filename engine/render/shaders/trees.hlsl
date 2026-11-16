// ---------------------------------------------------------------------------
// Trees: the mesh pass (LOD 0 and 1), the octahedral impostor pass, and the
// bake that makes the impostors.
//
// Everything is drawn through ExecuteIndirect from lists tree_cull.hlsl
// fills, one draw per (model, level) plus one for every impostor. The
// per-draw root constant says where that draw's visible list starts, because
// SV_InstanceID does not include StartInstanceLocation.
//
// Lit the way mesh.hlsl lights the ground -- same sun, same sky term, same
// fog -- so a tree and the terrain under it never disagree about the light.
// Bark and leaves are procedural: no image files, as for the ground.
// ---------------------------------------------------------------------------

struct TreeVertex {
    float3 pos;
    float3 nrm;
    float2 uv;
    float  sway;
    float  ao;
    float  kind;   // 0 bark, 1 leaf, 2 needle, 3 cap up, 4 cap down
    float  seed;
};

struct TreeInstance {
    float3 pos;
    float  scale;
    float4 rot;
    uint   model;
    uint   mode;    // 0 standing, 1 felled log, 2 stump
    float  hit_time;
    uint   id;
};

struct ModelInfo {
    float radius;     // metres at nominal, horizontal
    float top;
    float bound;      // impostor half-extent: the bake's ortho radius
    float slice;      // impostor atlas slice
    uint  first0, count0, base0;
    uint  first1, count1, base1;
    uint  species;
    uint  pad;
};

struct SpeciesLook {
    float3 bark;
    float  leaf_kind;
    float3 leaf;
    float  pad;
};

cbuffer DrawOffset : register(b1) { uint g_list_offset; };

cbuffer TreeFrame : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;    float time;
    float3 sun_dir;    float max_dist;
    float3 cam_right;  float lod0_dist;
    float3 cam_up;     float lod1_dist;
    float3 cam_fwd;    float far_plane;
    float4 frustum[6];
    uint   instance_count;
    uint   model_count;
    uint   frames;      // impostor frames per side
    uint   quad_first;  // the impostor quad in the shared index buffer
    uint   quad_base;
    float  wind;
    float  cut_height;
    float  debug;
    // The bake's view, set per frame of the atlas.
    float3 bake_dir;   float bake_r;
    float3 bake_right; float bake_centre_y;
    float3 bake_up;    float shadow_pass;
};

#include "engine/render/shaders/shadow.hlsli"

StructuredBuffer<TreeVertex>   g_verts     : register(t0);
StructuredBuffer<TreeInstance> g_instances : register(t1);
StructuredBuffer<uint>         g_visible   : register(t2);
StructuredBuffer<ModelInfo>    g_models    : register(t3);
StructuredBuffer<SpeciesLook>  g_looks     : register(t4);
Texture2DArray<float4>         g_imp_albedo : register(t5);
Texture2DArray<float4>         g_imp_normal : register(t6);
SamplerState                   g_linear    : register(s0);

static const float3 kSky = float3(0.40f, 0.50f, 0.66f);

#include "engine/render/shaders/leaf.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/local_lights.hlsli"

float3 qrot(float4 q, float3 v) {
    const float3 t = 2.0f * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}

bool keep_pixel(float2 px, uint packed) {
    const float w = float((packed >> 20) & 0xFFu) / 255.0f;
    const uint side = (packed >> 28) & 1u;
    const float d = dither(px);
    return side == 0u ? d < w : d >= w;
}

// --- shading ----------------------------------------------------------------

// Bark: vertical fissures for dark bark; for pale bark (a birch's), dark
// horizontal dashes instead. Chosen by the albedo's brightness, not by name.
float3 bark_albedo(float3 base, float2 uv, float footprint) {
    const float lum = dot(base, float3(0.3f, 0.5f, 0.2f));
    const float fade = saturate(1.0f - footprint * 3.0f);
    if (lum > 0.6f) {
        const float dash = step(0.82f, vnoise(float2(uv.x * 3.0f, uv.y * 14.0f)));
        const float blot = vnoise(uv * float2(1.5f, 0.6f));
        return base * (1.0f - fade * (0.75f * dash + 0.25f * step(0.7f, blot)));
    }
    const float ridge = vnoise(float2(uv.x * 7.0f, uv.y * 0.9f));
    const float fine = vnoise(uv * float2(18.0f, 4.0f));
    return base * (1.0f + fade * (0.45f * (ridge - 0.5f) + 0.15f * (fine - 0.5f)));
}

// `sh` is the sun's visibility from shadow.hlsli: it scales the direct and
// the transmitted light, never the sky. `albedo` is as authored (sRGB).
//
// In physical units since ADR-004 L1 (lighting.hlsli). The shapes are the
// ones that were tuned by eye before it, rescaled against the terrain's. The
// ambient arrives per vertex (gi in MeshOut), with the fallback's 1.3x fill
// -- a stand-in for bounce between leaves and off the floor under a crown --
// already in it where the probes did not answer, and gone where they did
// (lighting.hlsli's gi_fill).
SceneOut shade(float3 albedo_srgb, float3 n, float ao, float3 world, bool foliage, float sh, float4 clip) {
    const float3 albedo = srgb_to_linear(albedo_srgb);
    const float3 v = normalize(cam_pos - world);
    float3 dir, amb;
    if (foliage) {
        // A crown is lit THROUGH as much as on: wrapped diffuse, light that
        // leaks through the leaves when the sun is behind them, and skylight
        // that reaches the underside. With the ground's plain Lambert term
        // the underside of every crown went black -- shards of black card
        // against the sky, which is what the first close-up showed.
        dir = leaf_sun_light(albedo, n, v, 0.6f, 0.7f, sh) * (0.85f * (0.45f + 0.55f * ao));
        amb = albedo / kPi * gi_diffuse(world, n, v) * (0.45f + 0.55f * ao) +
              local_lights_leaf(albedo, n, v, 0.6f, 0.7f, world, clip) * (0.85f * (0.45f + 0.55f * ao));
    } else {
        // Bark gets more fill than open ground: under a crown, light that
        // bounced off leaves and the floor reaches the shaded side, and with
        // the ground's fill every trunk's shadow side read as black.
        const Surface s = make_surface(albedo, 0.85f, n, v);
        dir = sun_light(s, sh) * (0.9f * (0.35f + 0.65f * ao));
        amb = ambient_light(s, world) * ao + local_lights(s, world, clip) * (0.35f + 0.65f * ao);
    }
    return scene_out(dir, amb, world, cam_pos, far_plane, foliage ? 0.0f : 1.0f, foliage ? 0.6f : 1.0f);
}

// --- mesh pass ----------------------------------------------------------------

struct MeshOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    float2 uv    : TEXCOORD2;
    float  ao    : TEXCOORD3;
    nointerpolation float kind : TEXCOORD4;
    nointerpolation float seed : TEXCOORD5;
    float  local_y : TEXCOORD6;
    nointerpolation uint packed : TEXCOORD7;
    nointerpolation uint mode : TEXCOORD8;
    nointerpolation float3 bark : TEXCOORD9;
    nointerpolation float3 leaf : TEXCOORD10;
    nointerpolation float tint : TEXCOORD11;
    nointerpolation float scale : TEXCOORD12;
    nointerpolation float narrow : TEXCOORD13;
    float4 gi    : TEXCOORD14;   // gi_diffuse per vertex: irradiance, and its scale for specular (lighting.hlsli)
};

MeshOut degenerate() {
    MeshOut o = (MeshOut)0;
    o.clip = float4(0.0f, 0.0f, 0.0f, 1.0f);
    return o;
}

// `lit`: the camera's pass, which reads the canopy map; the shadow passes
// bind other constants where the map's would be and need no sky.
MeshOut mesh_vertex(uint vid, uint iid, bool lit) {
    const uint packed = g_visible[g_list_offset + iid];
    const TreeInstance inst = g_instances[packed & 0xFFFFFu];
    const TreeVertex v = g_verts[vid];
    const ModelInfo mi = g_models[inst.model];
    const SpeciesLook lk = g_looks[mi.species];

    // Which parts of the mesh this instance draws. Whole cards and caps are
    // dropped here, by collapsing every vertex to one point; the bark split
    // at the cut is per pixel, below.
    const bool cap_up = v.kind > 2.5f && v.kind < 3.5f;
    const bool cap_dn = v.kind > 3.5f;
    const bool foliage = v.kind > 0.5f && v.kind < 2.5f;
    if (inst.mode == 0u && (cap_up || cap_dn)) return degenerate();
    if (inst.mode == 1u && cap_up) return degenerate();
    if (inst.mode == 2u && (cap_dn || foliage)) return degenerate();

    float3 p = v.pos;
    if (inst.mode == 0u) {
        // Wind: the whole tree sways with height, cards flutter on top.
        const float phase = float(inst.id & 1023u) * 0.37f;
        const float h = saturate(p.y / max(mi.top, 1.0f));
        const float3 wdir = normalize(float3(1.0f, 0.0f, 0.35f));
        float sway = sin(time * 0.9f + phase) * 0.6f + sin(time * 2.1f + phase * 1.7f) * 0.25f;
        float3 off = wdir * sway * (0.004f * mi.top) * h * h * wind;
        if (foliage) {
            off += float3(sin(time * 5.3f + v.seed * 40.0f), sin(time * 4.1f + v.seed * 23.0f) * 0.5f,
                          cos(time * 4.7f + v.seed * 31.0f)) * (0.05f * v.sway * wind);
        }
        // A chop shakes the tree for half a second.
        const float since = time - inst.hit_time;
        if (since >= 0.0f && since < 0.6f) {
            off += float3(1.0f, 0.0f, 0.6f) * sin(since * 55.0f) * (1.0f - since / 0.6f) * 0.03f * mi.top * h;
        }
        p += off / max(inst.scale, 0.05f);
    }

    const float3 world = inst.pos + qrot(inst.rot, p * inst.scale);
    MeshOut o;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.world = world;
    o.nrm = qrot(inst.rot, v.nrm);
    o.uv = v.uv;
    o.ao = v.ao;
    o.kind = v.kind;
    o.seed = v.seed;
    o.local_y = v.pos.y;
    o.packed = packed;
    o.mode = inst.mode;
    o.bark = lk.bark;
    o.leaf = lk.leaf;
    o.tint = hash11(float(inst.id) * 0.013f);
    o.scale = inst.scale;
    o.narrow = lk.leaf_kind;
    o.gi = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (lit) {
        o.gi.rgb = gi_diffuse(world, normalize(o.nrm), normalize(cam_pos - world));
        o.gi.rgb *= gi_fill(1.3f);
        o.gi.a = g_gi_all;
    }
    return o;
}

MeshOut vs_mesh(uint vid : SV_VertexID, uint iid : SV_InstanceID) { return mesh_vertex(vid, iid, true); }
MeshOut vs_mesh_shadow(uint vid : SV_VertexID, uint iid : SV_InstanceID) { return mesh_vertex(vid, iid, false); }

SceneOut ps_mesh(MeshOut i, bool front : SV_IsFrontFace) {
    if (!keep_pixel(i.clip.xy, i.packed)) discard;
    // The cut: a log keeps the trunk above it, a stump below it.
    if (i.mode == 1u && i.kind < 0.5f) clip(i.local_y - cut_height + 0.002f);
    if (i.mode == 2u && i.kind < 0.5f) clip(cut_height + 0.002f - i.local_y);

    float3 n = normalize(i.nrm);
    float3 albedo;
    bool foliage = false;
    float ao = i.ao;
    if (i.kind < 0.5f) {
        if (!front) n = -n;
        albedo = bark_albedo(i.bark, i.uv * i.scale, length(fwidth(i.uv)));
    } else if (i.kind < 2.5f) {
        foliage = true;
        const float blur = length(fwidth(i.uv));
        const float a = leaf_coverage(i.uv, i.kind, i.seed, blur, i.narrow);
        // Coverage tested against the dither once the card is small enough
        // to shimmer, against 0.5 while it is sharp.
        const float thr = lerp(0.5f, dither(i.clip.xy), saturate(blur * 6.0f));
        if (a < thr) discard;
        const float var = 0.82f + 0.36f * frac(i.seed * 7.31f) ;
        albedo = i.leaf * var * (0.9f + 0.2f * i.tint);
        // Sunlit cards a touch yellower, shaded ones bluer: the cheapest
        // depth cue a crown has.
        albedo = lerp(albedo * float3(0.85f, 0.95f, 1.1f), albedo * float3(1.08f, 1.04f, 0.85f), ao);
    } else {
        // End grain: growth rings, darker heartwood, a bark rim.
        const float r = i.uv.x / max(i.seed, 0.01f);
        const float rings = 0.5f + 0.5f * sin(r * 60.0f + vnoise(i.world.xz * 6.0f) * 3.0f);
        albedo = lerp(float3(0.66f, 0.52f, 0.34f), float3(0.48f, 0.34f, 0.20f), r * 0.6f) * (0.9f + 0.1f * rings);
        if (r > 0.9f) albedo = i.bark;
        ao = 1.0f;
    }
    if (debug > 0.5f && debug < 1.5f) {
        // 1: which level drew it -- LOD 0 red, LOD 1 green (packed bit 29).
        return scene_flat(((i.packed >> 29) & 1u) != 0u ? float3(0.2f, 0.8f, 0.2f) : float3(0.9f, 0.2f, 0.2f));
    }
    g_gi_vertex = i.gi;
    return shade(albedo, n, ao, i.world, foliage, shadow_factor(i.world, n), i.clip);
}

// The mesh as a shadow caster: the same cut, the same leaf coverage, no
// colour. Without the leaf test every card would cast a solid square.
void ps_mesh_shadow(MeshOut i) {
    if (!keep_pixel(i.clip.xy, i.packed)) discard;
    if (i.mode == 1u && i.kind < 0.5f) clip(i.local_y - cut_height + 0.002f);
    if (i.mode == 2u && i.kind < 0.5f) clip(cut_height + 0.002f - i.local_y);
    if (i.kind > 0.5f && i.kind < 2.5f) {
        if (shadow_pass > 1.5f) {
            // Past the first cascade a texel is 10 cm or more: a leaf is
            // sub-texel, so test the card's round outline at its average
            // coverage instead of evaluating every leaf.
            const float rim = 1.0f - saturate((length(i.uv - 0.5f) - 0.40f) / 0.1f);
            if (rim * 0.6f < dither(i.clip.xy)) discard;
            return;
        }
        const float blur = length(fwidth(i.uv));
        const float a = leaf_coverage(i.uv, i.kind, i.seed, blur, i.narrow);
        const float thr = lerp(0.5f, dither(i.clip.xy), saturate(blur * 6.0f));
        if (a < thr) discard;
    }
}

// --- octahedral impostors -----------------------------------------------------
//
// Each model is baked from frames x frames directions over the upper
// hemisphere, laid out by the hemi-octahedral map: a direction's x and z,
// divided by |x| + |y| + |z|, rotated 45 degrees, fill the square. A pixel
// finds the four frames nearest its view direction and blends them, each
// sampled where the pixel's ray crosses that frame's plane -- which is what
// keeps a tree solid as the view swings between frames.

float2 hemi_oct_encode(float3 d) {
    d.y = max(d.y, 0.0f);
    const float s = abs(d.x) + abs(d.y) + abs(d.z);
    const float2 p = float2(d.x, d.z) / s;
    return float2(p.x + p.y, p.x - p.y) * 0.5f + 0.5f;
}

float3 hemi_oct_decode(float2 uv) {
    const float2 e = uv * 2.0f - 1.0f;
    const float x = 0.5f * (e.x + e.y), z = 0.5f * (e.x - e.y);
    return normalize(float3(x, 1.0f - abs(x) - abs(z), z));
}

// The bake's basis for a view direction, used identically on both sides.
void frame_basis(float3 dir, out float3 right, out float3 up) {
    const float3 w = abs(dir.y) > 0.999f ? float3(0.0f, 0.0f, 1.0f) : float3(0.0f, 1.0f, 0.0f);
    right = normalize(cross(w, dir));
    up = cross(dir, right);
}

struct ImpOut {
    float4 clip   : SV_Position;
    float3 world  : TEXCOORD0;
    nointerpolation float3 centre : TEXCOORD1;
    nointerpolation float  bound  : TEXCOORD2;
    nointerpolation float  slice  : TEXCOORD3;
    nointerpolation float2 yaw    : TEXCOORD4;   // cos, sin
    nointerpolation uint   packed : TEXCOORD5;
    nointerpolation float  tint   : TEXCOORD6;
    float4 gi     : TEXCOORD7;   // gi_diffuse per corner (lighting.hlsli)
};

ImpOut impostor_vertex(uint vid, uint iid, bool lit) {
    const uint packed = g_visible[g_list_offset + iid];
    const TreeInstance inst = g_instances[packed & 0xFFFFFu];
    const ModelInfo mi = g_models[inst.model];
    const uint corner = vid & 3u;   // quad_base is a multiple of 4
    const float2 c = float2((corner == 1u || corner == 2u) ? 1.0f : -1.0f,
                            (corner >= 2u) ? 1.0f : -1.0f);
    const float r = mi.bound * inst.scale;
    const float3 centre = inst.pos + float3(0.0f, (mi.top * 0.5f) * inst.scale, 0.0f);
    const float3 world = centre + (cam_right * c.x + cam_up * c.y) * r;
    ImpOut o;
    o.clip = mul(float4(world, 1.0f), view_proj);
    o.world = world;
    o.centre = centre;
    o.bound = r;
    o.slice = mi.slice;
    // Yaw from the quaternion; the lean is a few degrees and the impostor
    // ignores it.
    const float yaw = 2.0f * atan2(inst.rot.y, inst.rot.w);
    o.yaw = float2(cos(yaw), sin(yaw));
    o.packed = packed;
    o.tint = hash11(float(inst.id) * 0.013f);
    o.gi = float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (lit) {
        o.gi.rgb = gi_diffuse(o.world, float3(0.0f, 1.0f, 0.0f), normalize(cam_pos - o.world));
        o.gi.rgb *= gi_fill(1.3f);
        o.gi.a = g_gi_all;
    }
    return o;
}

ImpOut vs_impostor(uint vid : SV_VertexID, uint iid : SV_InstanceID) { return impostor_vertex(vid, iid, true); }
ImpOut vs_impostor_shadow(uint vid : SV_VertexID, uint iid : SV_InstanceID) { return impostor_vertex(vid, iid, false); }

// World -> the model's own frame (undo the yaw) and back.
float3 to_local(float3 v, float2 cs) { return float3(cs.x * v.x - cs.y * v.z, v.y, cs.y * v.x + cs.x * v.z); }
float3 to_world(float3 v, float2 cs) { return float3(cs.x * v.x + cs.y * v.z, v.y, -cs.y * v.x + cs.x * v.z); }

// The four nearest baked views, sampled where a ray along `rd` through this
// pixel's point on the card crosses each view's plane. The ray starts at the
// pixel's own world point, which works for the camera's perspective ray and
// for the sun's parallel one alike.
bool impostor_fetch(ImpOut i, float3 rd, out float4 acc_a, out float4 acc_n) {
    const float3 vloc = to_local(-rd, i.yaw);
    const float n = float(frames);
    const float2 g = hemi_oct_encode(vloc) * n - 0.5f;
    const float2 g0 = floor(g);
    const float2 f = g - g0;

    acc_a = 0.0f; acc_n = 0.0f;
    float wsum = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        const float2 o = float2(k & 1, k >> 1);
        const float2 fr = clamp(g0 + o, 0.0f, n - 1.0f);
        const float w = (o.x > 0.5f ? f.x : 1.0f - f.x) * (o.y > 0.5f ? f.y : 1.0f - f.y);
        const float3 dl = hemi_oct_decode((fr + 0.5f) / n);
        float3 rl, ul;
        frame_basis(dl, rl, ul);
        // Where this pixel's ray crosses the frame's plane through the centre.
        const float3 dw = to_world(dl, i.yaw);
        const float den = dot(rd, dw);
        if (abs(den) < 1e-4f) continue;
        const float t = dot(i.centre - i.world, dw) / den;
        const float3 hit = i.world + rd * t - i.centre;
        const float3 hl = to_local(hit, i.yaw);
        const float2 q = float2(dot(hl, rl), dot(hl, ul)) / i.bound;
        if (any(abs(q) > 1.0f)) continue;
        const float2 auv = (fr + float2(q.x * 0.5f + 0.5f, 0.5f - q.y * 0.5f)) / n;
        acc_a += w * g_imp_albedo.Sample(g_linear, float3(auv, i.slice));
        acc_n += w * g_imp_normal.Sample(g_linear, float3(auv, i.slice));
        wsum += w;
    }
    if (wsum <= 0.0f) return false;
    // A fixed threshold. A dithered one kept the averaged fraction of pixels
    // and drew every distant crown as speckle; the bake makes cards nearly
    // solid instead, so the averaged alpha of a crown stays above this.
    return acc_a.a / wsum >= 0.45f;
}

SceneOut ps_impostor(ImpOut i) {
    if (!keep_pixel(i.clip.xy, i.packed)) discard;
    float4 acc_a, acc_n;
    if (!impostor_fetch(i, normalize(i.world - cam_pos), acc_a, acc_n)) discard;
    const float3 albedo = (acc_a.rgb / max(acc_a.a, 1e-4f)) * (0.9f + 0.2f * i.tint);
    const float3 nl = normalize(acc_n.rgb / max(acc_n.a, 1e-4f) * 2.0f - 1.0f);
    const float3 nw = to_world(nl, i.yaw);
    if (debug > 0.5f && debug < 1.5f) return scene_flat(float3(0.2f, 0.3f, 0.95f));
    // The card runs through the middle of the crown, so a shadow lookup on it
    // would find the tree's own front half and darken every distant tree.
    // Look from just outside its bound on the sun's side instead: hills and
    // other trees still shade it, its own crown does not.
    const float sh = shadow_factor(i.world + kSunDir * i.bound, kSunDir);
    g_gi_vertex = i.gi;
    return shade(albedo, nw, 1.0f, i.world, true, sh, i.clip);
}

// An impostor casting: its card already faces the sun (the shadow pass hands
// the light's basis in as the camera's), and it is sampled along the sun.
void ps_impostor_shadow(ImpOut i) {
    // One view, alpha only. A shadow needs neither the normal nor the blend
    // between views, and the far cascade holds thousands of impostors
    // overlapping under a high sun: four blended views of albedo and normal
    // each was most of the shadow pass's cost.
    // The trees' own constant, not the frame light's: in a shadow pass the
    // shadow-constant slot is bound to this pass's buffer, not the frame's.
    const float3 rd = -normalize(sun_dir);
    const float n = float(frames);
    const float2 fr = clamp(floor(hemi_oct_encode(to_local(-rd, i.yaw)) * n), 0.0f, n - 1.0f);
    const float3 dl = hemi_oct_decode((fr + 0.5f) / n);
    float3 rl, ul;
    frame_basis(dl, rl, ul);
    const float3 dw = to_world(dl, i.yaw);
    const float t = dot(i.centre - i.world, dw) / dot(rd, dw);
    const float3 hl = to_local(i.world + rd * t - i.centre, i.yaw);
    const float2 q = float2(dot(hl, rl), dot(hl, ul)) / i.bound;
    if (any(abs(q) > 1.0f)) discard;
    const float2 auv = (fr + float2(q.x * 0.5f + 0.5f, 0.5f - q.y * 0.5f)) / n;
    if (g_imp_albedo.SampleLevel(g_linear, float3(auv, i.slice), 1.0f).a < 0.45f) discard;
}

// --- the bake ---------------------------------------------------------------
//
// One model from one direction, orthographic, into one frame of the atlas.
// Albedo is stored with the baked occlusion multiplied in and premultiplied
// by coverage, so the mip chain can be a plain average; the normal is in the
// model's own frame, premultiplied the same way.

struct BakeOut {
    float4 clip  : SV_Position;
    float3 nrm   : TEXCOORD0;
    float2 uv    : TEXCOORD1;
    float  ao    : TEXCOORD2;
    nointerpolation float kind : TEXCOORD3;
    nointerpolation float seed : TEXCOORD4;
    nointerpolation float3 bark : TEXCOORD5;
    nointerpolation float3 leaf : TEXCOORD6;
    nointerpolation float narrow : TEXCOORD7;
};

BakeOut vs_bake(uint vid : SV_VertexID) {
    const TreeVertex v = g_verts[vid];
    const ModelInfo mi = g_models[instance_count];   // the model being baked
    const SpeciesLook lk = g_looks[mi.species];
    BakeOut o = (BakeOut)0;
    if (v.kind > 2.5f) { o.clip = float4(0, 0, 0, 1); return o; }
    const float3 rel = v.pos - float3(0.0f, bake_centre_y, 0.0f);
    const float x = dot(rel, bake_right) / bake_r;
    const float y = dot(rel, bake_up) / bake_r;
    const float z = 0.5f - 0.5f * dot(rel, bake_dir) / bake_r;
    o.clip = float4(x, y, z, 1.0f);
    o.nrm = v.nrm;
    o.uv = v.uv;
    o.ao = v.ao;
    o.kind = v.kind;
    o.seed = v.seed;
    o.bark = lk.bark;
    o.leaf = lk.leaf;
    o.narrow = lk.leaf_kind;
    return o;
}

struct BakeTargets {
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
};

BakeTargets ps_bake(BakeOut i, bool front : SV_IsFrontFace) {
    float3 n = normalize(i.nrm);
    float3 albedo;
    float ao = i.ao;
    if (i.kind < 0.5f) {
        if (!front) n = -n;
        albedo = bark_albedo(i.bark, i.uv, 1.0f);
    } else {
        // The bake is coarse (a frame is a few tens of pixels across a tree),
        // so leaf coverage is judged soft and thresholded low: a crown should
        // stay as dense as the mesh reads from the same distance.
        // Near-solid cards: at a few tens of pixels per tree the leaf pattern
        // is sub-pixel, and its gaps averaged through the mips into speckle.
        const float a = leaf_coverage(i.uv, i.kind, i.seed, 0.6f, i.narrow);
        if (a < 0.08f) discard;
        albedo = i.leaf * (0.82f + 0.36f * frac(i.seed * 7.31f));
        albedo = lerp(albedo * float3(0.85f, 0.95f, 1.1f), albedo * float3(1.08f, 1.04f, 0.85f), ao);
    }
    BakeTargets t;
    t.albedo = float4(albedo * (0.45f + 0.55f * ao), 1.0f);
    t.normal = float4(n * 0.5f + 0.5f, 1.0f);
    return t;
}
