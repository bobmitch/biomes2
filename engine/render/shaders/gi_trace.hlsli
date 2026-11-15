// ---------------------------------------------------------------------------
// The tracer (ADR-004 L5): "what does a ray hit", the one place that knows
// how the scene is represented for rays. Its first implementation is DXR 1.1
// inline RayQuery against rt_scene.h's TLAS; the ADR's planned second (L6b)
// sphere-traces a distance field from the density function behind the same
// two calls. Everything that casts rays -- shadows now, probe updates next --
// goes through them.
//
// SM 6.5 only: include from a shader compiled with DXC (cs_6_5).
// ---------------------------------------------------------------------------
#ifndef ENGINE_GI_TRACE_HLSLI
#define ENGINE_GI_TRACE_HLSLI

#include "engine/render/shaders/leaf.hlsli"

struct GiRay { float3 origin; float tmin; float3 dir; float tmax; };

struct GiHit {
    bool   hit;
    float  t;
    float3 normal;     // world space, facing the ray
    float3 albedo;     // linear
    float3 emission;   // nothing emits yet
    uint   material;   // 0 terrain, 1 bark, 2 leaves
};

RaytracingAccelerationStructure g_tlas : register(t30);

struct TerrainVertex { float3 pos; float3 nrm; };
StructuredBuffer<TerrainVertex> g_rt_tvert : register(t31);   // the mesh pool's vertices
StructuredBuffer<uint>          g_rt_tidx  : register(t32);   // its indices, absolute
StructuredBuffer<uint2>         g_rt_tmat  : register(t33);   // per vertex: ground, rock (surface_nets.hlsl)

struct TreeRtInfo { uint vbase; uint ibase_bark; uint ibase_leaf; uint narrow; float4 bark; float4 leaf; };
StructuredBuffer<float3>     g_rt_tree_pos  : register(t34);
StructuredBuffer<uint>       g_rt_tree_idx  : register(t35);
StructuredBuffer<TreeRtInfo> g_rt_tree_info : register(t36);
StructuredBuffer<float4>     g_rt_tree_attr : register(t37);   // per vertex: u, v, kind, seed

static const uint  kRtSlotIndices = 36864;     // chunk_mesher.h kSlotIndices
static const uint  kRtTreeFlag = 0x800000u;    // rt_scene.h
static const uint  kRtMaskTerrain = 0x1u, kRtMaskTrees = 0x2u;
// Whether a ray meeting a leaf card at these barycentrics meets a leaf: the
// raster's own pattern (leaf.hlsli) at the card's interpolated uv, tested at
// 0.5 as a sharp card is. A first version passed 55% of the light through
// every card instead; a low sun crossed so many that a wood's floor lost
// every trunk shadow under a uniform dimness.
bool rt_leaf_solid(uint inst, uint prim, float2 b) {
    const TreeRtInfo ti = g_rt_tree_info[inst & 0x7FFFFFu];
    const uint ib = ti.ibase_leaf + 3u * prim;
    const float4 a0 = g_rt_tree_attr[ti.vbase + g_rt_tree_idx[ib + 0u]];
    const float4 a1 = g_rt_tree_attr[ti.vbase + g_rt_tree_idx[ib + 1u]];
    const float4 a2 = g_rt_tree_attr[ti.vbase + g_rt_tree_idx[ib + 2u]];
    const float2 uv = a0.xy * (1.0f - b.x - b.y) + a1.xy * b.x + a2.xy * b.y;
    return leaf_coverage(uv, a0.z, a0.w, 0.1f, float(ti.narrow)) >= 0.5f;
}

// The light that gets through along the ray: 1 open, 0 blocked. Leaf cards
// are alpha-tested; the first solid hit ends the search.
float gi_occluded_masked(GiRay r, uint mask) {
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    RayDesc rd;
    rd.Origin = r.origin; rd.TMin = r.tmin; rd.Direction = r.dir; rd.TMax = r.tmax;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, mask, rd);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            rt_leaf_solid(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics())) {
            q.CommitNonOpaqueTriangleHit();
            q.Abort();
        }
    }
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

float gi_occluded(GiRay r) { return gi_occluded_masked(r, 0xFFu); }

float4 rt_unpack_material(uint m) {
    const float4 q = float4(m & 0xFFu, (m >> 8) & 0xFFu, (m >> 16) & 0xFFu, m >> 24) / 255.0f;
    return float4(q.rgb * q.rgb, q.a);
}

// The nearest hit, with its normal and albedo. Leaf cards are alpha-tested
// as the shadow ray's are.
GiHit gi_trace_masked(GiRay r, uint mask) {
    RayQuery<RAY_FLAG_NONE> q;
    RayDesc rd;
    rd.Origin = r.origin; rd.TMin = r.tmin; rd.Direction = r.dir; rd.TMax = r.tmax;
    q.TraceRayInline(g_tlas, RAY_FLAG_NONE, mask, rd);
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            rt_leaf_solid(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics()))
            q.CommitNonOpaqueTriangleHit();
    }
    GiHit h;
    h.hit = false; h.t = r.tmax; h.normal = float3(0.0f, 1.0f, 0.0f);
    h.albedo = float3(0.0f, 0.0f, 0.0f); h.emission = float3(0.0f, 0.0f, 0.0f); h.material = 0u;
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return h;
    h.hit = true;
    h.t = q.CommittedRayT();
    const uint inst = q.CommittedInstanceID();
    const uint prim = q.CommittedPrimitiveIndex();
    const float2 b = q.CommittedTriangleBarycentrics();
    const float3 w = float3(1.0f - b.x - b.y, b.x, b.y);
    if ((inst & kRtTreeFlag) != 0u) {
        const TreeRtInfo ti = g_rt_tree_info[inst & 0x7FFFFFu];
        const uint geom = q.CommittedGeometryIndex();
        const uint ib = (geom == 0u ? ti.ibase_bark : ti.ibase_leaf) + 3u * prim;
        const float3 p0 = g_rt_tree_pos[ti.vbase + g_rt_tree_idx[ib + 0u]];
        const float3 p1 = g_rt_tree_pos[ti.vbase + g_rt_tree_idx[ib + 1u]];
        const float3 p2 = g_rt_tree_pos[ti.vbase + g_rt_tree_idx[ib + 2u]];
        const float3x4 m = q.CommittedObjectToWorld3x4();
        h.normal = normalize(mul(m, float4(cross(p1 - p0, p2 - p0), 0.0f)));
        h.albedo = geom == 0u ? ti.bark.rgb : ti.leaf.rgb;
        h.material = geom == 0u ? 1u : 2u;
    } else {
        const uint base = inst * kRtSlotIndices + 3u * prim;
        const uint i0 = g_rt_tidx[base], i1 = g_rt_tidx[base + 1u], i2 = g_rt_tidx[base + 2u];
        h.normal = normalize(normalize(g_rt_tvert[i0].nrm) * w.x + normalize(g_rt_tvert[i1].nrm) * w.y +
                             normalize(g_rt_tvert[i2].nrm) * w.z);
        // The same ground-or-rock choice mesh.hlsl makes, from the vertex
        // materials, without its surface texture.
        const uint2 m0 = g_rt_tmat[i0];
        const float4 ground = rt_unpack_material(m0.x), rock = rt_unpack_material(m0.y);
        h.albedo = lerp(rock.rgb, ground.rgb, smoothstep(0.62f, 0.86f, h.normal.y));
        h.material = 0u;
    }
    if (dot(h.normal, r.dir) > 0.0f) h.normal = -h.normal;
    return h;
}

GiHit gi_trace(GiRay r) { return gi_trace_masked(r, 0xFFu); }

// The terrain along r, and the point to start a shadow ray from there:
// Hanika's shadow-terminator fix ("Hacking the Shadow Terminator", Ray
// Tracing Gems II). Shading uses smooth vertex normals over a faceted
// surface-nets mesh -- facets up to 8 m wide at LOD 3 -- so a ray leaving
// the flat facet towards a grazing sun meets the neighbouring facet's bulge.
// Lifting the start onto the curved surface the vertex normals imply removes
// the self-shadowing streaks without a bias that detaches real shadows.
bool gi_terrain_point(GiRay r, out float t, out float3 lifted, out float3 n_shade) {
    t = r.tmax;
    lifted = r.origin;
    n_shade = float3(0.0f, 1.0f, 0.0f);
    RayQuery<RAY_FLAG_CULL_NON_OPAQUE> q;
    RayDesc rd;
    rd.Origin = r.origin; rd.TMin = r.tmin; rd.Direction = r.dir; rd.TMax = r.tmax;
    q.TraceRayInline(g_tlas, RAY_FLAG_CULL_NON_OPAQUE, kRtMaskTerrain, rd);
    q.Proceed();
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return false;
    t = q.CommittedRayT();
    const uint base = q.CommittedInstanceID() * kRtSlotIndices + 3u * q.CommittedPrimitiveIndex();
    const float2 b = q.CommittedTriangleBarycentrics();
    const float w[3] = {1.0f - b.x - b.y, b.x, b.y};
    const float3 P = r.origin + r.dir * t;
    float3 lift = P;
    float3 ns = float3(0.0f, 0.0f, 0.0f);
    [unroll] for (int k = 0; k < 3; ++k) {
        const TerrainVertex v = g_rt_tvert[g_rt_tidx[base + uint(k)]];
        const float3 n = normalize(v.nrm);
        lift -= w[k] * min(0.0f, dot(P - v.pos, n)) * n;
        ns += w[k] * n;
    }
    lifted = lift;
    n_shade = normalize(ns);
    return true;
}

#endif
