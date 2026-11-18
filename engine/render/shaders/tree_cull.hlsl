// ---------------------------------------------------------------------------
// Tree culling and level selection, on the GPU, every frame.
//
// cs_clear zeroes one counter per draw; cs_cull tests every resident tree
// against the frustum and the draw distance, picks a level (LOD 0 mesh, LOD 1
// mesh, impostor) with a dithered cross-fade band at each boundary and a fade
// out at the far end, and appends it to that draw's list; cs_args turns the
// counts into ExecuteIndirect arguments. Nothing comes back to the CPU.
//
// A visible entry packs: instance index (20 bits), fade weight (8), the
// dither side (1), and which mesh level drew it (1, for the debug tint).
// ---------------------------------------------------------------------------

struct TreeInstance {
    float3 pos;
    float  scale;
    float4 rot;
    uint   model;
    uint   mode;
    float  hit_time;
    uint   id;
};

struct ModelInfo {
    float radius;
    float top;
    float bound;
    float slice;
    uint  first0, count0, base0;
    uint  first1, count1, base1;
    uint  species;
    uint  pad;
};

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
    uint   frames;
    uint   quad_first;
    uint   quad_base;
    float  wind;
    float  cut_height;
    float  debug;
    float3 bake_dir;   float bake_r;
    float3 bake_right; float bake_centre_y;
    float3 bake_up;    float shadow_pass;
};

StructuredBuffer<TreeInstance> g_instances : register(t1);
StructuredBuffer<ModelInfo>    g_models    : register(t3);
StructuredBuffer<uint2>        g_slots     : register(t7);   // (offset, capacity) per draw

RWStructuredBuffer<uint> g_counters : register(u0);
RWStructuredBuffer<uint> g_visible  : register(u1);
RWStructuredBuffer<uint> g_args     : register(u2);
RWStructuredBuffer<uint> g_stats    : register(u3);   // [0] overflowed appends

uint draw_count() { return model_count * 2u + 1u; }

[numthreads(64, 1, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < draw_count()) g_counters[id.x] = 0u;
    if (id.x == 0u) g_stats[0] = 0u;
}

void append(uint d, uint entry) {
    // --tree-debug 2/3/4: only LOD 0 / LOD 1 / impostors, for isolating a
    // level's artefacts.
    if (debug > 1.5f) {
        const uint lvl = d >= model_count * 2u ? 2u : (d & 1u);
        if (lvl != uint(debug - 2.0f + 0.5f)) return;
    }
    uint at;
    InterlockedAdd(g_counters[d], 1u, at);
    const uint2 s = g_slots[d];
    if (at < s.y) g_visible[s.x + at] = entry;
    else InterlockedAdd(g_stats[0], 1u, at);
}

uint pack(uint idx, float w, uint side, uint lod1) {
    return (idx & 0xFFFFFu) | (uint(saturate(w) * 255.0f + 0.5f) << 20) | (side << 28) | (lod1 << 29);
}

[numthreads(64, 1, 1)]
void cs_cull(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    if (i >= instance_count) return;
    const TreeInstance t = g_instances[i];
    const ModelInfo m = g_models[t.model];

    // Bounding sphere round the crown and trunk. A felled log is lying down,
    // so its sphere is centred on the trunk's midpoint wherever that went.
    const float half = 0.5f * m.top * t.scale;
    float3 axis = float3(0.0f, 1.0f, 0.0f);
    {
        const float3 q = t.rot.xyz;
        const float3 tt = 2.0f * cross(q, axis);
        axis = axis + t.rot.w * tt + cross(q, tt);
    }
    const float3 c = t.pos + axis * half;
    const float r = max(m.radius * t.scale, half) * 1.1f;

    const float3 dv = c - cam_pos;
    const float d = length(dv);
    // The draw distance and its fade are HORIZONTAL, as the tile streamer and
    // the canopy shell (forest.hlsli) measure theirs; the level is chosen by
    // the 3D distance, which is what decides how big a tree is on screen.
    // A 3D draw distance left a hole under a camera high in the air: trees
    // below were past it while the shell, measured flat, had not begun.
    const float dh = length(dv.xz);
    if (dh - r > max_dist) return;
    [unroll] for (int k = 0; k < 6; ++k) {
        if (dot(frustum[k].xyz, c) + frustum[k].w < -r) return;
    }

    const uint mesh0 = t.model * 2u, mesh1 = t.model * 2u + 1u;
    const uint imp = model_count * 2u;

    // Logs and stumps are few and near; always the full mesh.
    if (t.mode != 0u) {
        append(mesh0, pack(i, 1.0f, 0u, 0u));
        return;
    }

    // Shadow cascades: the level by distance as for the camera, but never
    // finer than the cascade can show -- the nearest cascade's texels are
    // 3 cm, the next 10, the last 34. Every tree near the camera used to go
    // into all three at full detail, 2.9 ms of a forest frame. No fade bands:
    // a shadow does not need to cross-fade, and a dithered caster is holes.
    if (shadow_pass > 0.5f) {
        if (dh > max_dist) return;
        uint lvl = d < lod0_dist ? 0u : (d < lod1_dist ? 1u : 2u);
        const uint floor_lvl = shadow_pass > 2.5f ? 2u : (shadow_pass > 1.5f ? 1u : 0u);
        lvl = max(lvl, floor_lvl);
        if (lvl == 0u) append(mesh0, pack(i, 1.0f, 0u, 0u));
        else if (lvl == 1u) append(mesh1, pack(i, 1.0f, 0u, 1u));
        else append(imp, pack(i, 1.0f, 0u, 0u));
        return;
    }

    // The far fade: the last 12% of the draw distance, dithered out.
    const float far_band = 0.12f * max_dist;
    const float far_w = saturate((max_dist - dh) / far_band);

    // Cross-fade bands at each level boundary, 15% of the boundary distance:
    // both levels draw, on complementary sides of one dither threshold.
    const float b0 = 0.15f * lod0_dist, b1 = 0.15f * lod1_dist;
    if (d < lod0_dist) {
        const float w = saturate((lod0_dist - d) / b0);
        append(mesh0, pack(i, w, 0u, 0u));
        if (w < 1.0f) append(mesh1, pack(i, w, 1u, 1u));
    } else if (d < lod1_dist) {
        const float w = saturate((lod1_dist - d) / b1);
        append(mesh1, pack(i, w * far_w, 0u, 1u));
        if (w < 1.0f) append(imp, pack(i, w, 1u, 0u));
    } else {
        append(imp, pack(i, far_w, 0u, 0u));
    }
}

[numthreads(64, 1, 1)]
void cs_args(uint3 id : SV_DispatchThreadID) {
    const uint d = id.x;
    if (d >= draw_count()) return;
    const uint2 s = g_slots[d];
    const uint n = min(g_counters[d], s.y);
    uint first = 0u, count = 0u, base = 0u;
    if (d < model_count * 2u) {
        const ModelInfo m = g_models[d >> 1];
        if ((d & 1u) == 0u) { first = m.first0; count = m.count0; base = m.base0; }
        else                { first = m.first1; count = m.count1; base = m.base1; }
    } else {
        first = quad_first; count = 6u; base = quad_base;
    }
    const uint a = d * 6u;
    g_args[a + 0] = s.x;              // root constant: where this draw's list starts
    g_args[a + 1] = count;            // IndexCountPerInstance
    g_args[a + 2] = count > 0u ? n : 0u;
    g_args[a + 3] = first;            // StartIndexLocation
    g_args[a + 4] = base;             // BaseVertexLocation
    g_args[a + 5] = 0u;               // StartInstanceLocation
}
