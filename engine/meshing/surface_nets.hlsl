// ---------------------------------------------------------------------------
// B2: GPU surface nets over one chunk.
//
// Hand-written per backend, and deliberately NOT in the ADR-001 dialect --
// that ADR excludes kernel code by name, because workgroup memory, atomics and
// barriers are exactly the constructs that would make a small shared dialect
// impossible. It still *includes* the dialect field header, which is the whole
// point of the split: the field is shared, the machinery around it is not.
//
// Naive surface nets rather than dual contouring. One vertex per cell that has
// a sign change, placed at the average of its edge crossings, and a quad for
// every sign-changing edge joining the four cells around it. Dual contouring's
// QEF solve gives sharper creases and needs the Hermite data and a 3x3 solve
// per cell; the design document's terrain is noise, which has no creases to
// preserve, so the solve would cost without buying anything. If hard-edged
// player edits ever need it, this is where it goes.
// ---------------------------------------------------------------------------

#include "engine/field/trivial_field.h"

// 32 voxels at 0.5 m is the design's 16 m chunk; 33 corners is its 35,937
// field samples per chunk. Fixed here rather than passed: changing them is a
// change to the streaming design, not a runtime parameter. LOD changes the
// voxel size, not the counts.
static const uint kCells   = 32;
static const uint kCorners = 33;

cbuffer ChunkConstants : register(b0) {
    float3 chunk_origin;   // world position of corner (0,0,0)
    float  voxel;          // metres per voxel
};

struct Vertex {
    float3 pos;
    float3 nrm;
};

RWStructuredBuffer<float>  g_density     : register(u0);   // kCorners^3
RWStructuredBuffer<uint>   g_cell_vertex : register(u1);   // kCells^3, 0xFFFFFFFF = none
RWStructuredBuffer<Vertex> g_vertices    : register(u2);
RWStructuredBuffer<uint>   g_indices     : register(u3);
RWStructuredBuffer<uint>   g_counters    : register(u4);   // [0] vertices, [1] indices
RWStructuredBuffer<uint>   g_draw_args   : register(u5);   // DrawIndexedInstanced

static const uint kMaxVertices = 40000;
static const uint kMaxIndices  = 600000;

uint corner_index(uint3 c) { return (c.z * kCorners + c.y) * kCorners + c.x; }
uint cell_index(uint3 c)   { return (c.z * kCells   + c.y) * kCells   + c.x; }

float3 corner_world(uint3 c) {
    return chunk_origin + float3(c) * voxel;
}

// ---------------------------------------------------------------------------
// Pass 0 -- zero the append counters.
//
// A kernel rather than ClearUnorderedAccessViewUint, which needs the UAV in
// both a shader-visible and a non-shader-visible descriptor heap. Two heaps
// and two descriptors to zero eight bytes is not a trade worth making.
// ---------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    g_counters[0] = 0;
    g_counters[1] = 0;
}

// ---------------------------------------------------------------------------
// Pass 1 -- evaluate the field at every corner.
//
// Stored rather than recomputed because every corner is shared by up to eight
// cells, and the design's field budget is 20 ns a sample. Eight-fold redundant
// evaluation would be the single most expensive mistake available here.
// ---------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void cs_density(uint3 id : SV_DispatchThreadID) {
    if (any(id >= kCorners)) return;
    const float3 p = corner_world(id);
    g_density[corner_index(id)] = trivial_density(p.x, p.y, p.z);
}

// The 12 edges of a cell, as pairs of corner indices. Corner bit 0 is x,
// bit 1 is y, bit 2 is z.
static const uint2 kEdges[12] = {
    uint2(0, 1), uint2(2, 3), uint2(4, 5), uint2(6, 7),   // along x
    uint2(0, 2), uint2(1, 3), uint2(4, 6), uint2(5, 7),   // along y
    uint2(0, 4), uint2(1, 5), uint2(2, 6), uint2(3, 7)    // along z
};

uint3 corner_offset(uint i) {
    return uint3(i & 1u, (i >> 1) & 1u, (i >> 2) & 1u);
}

uint3 unit_axis(uint a) {
    return uint3(a == 0u ? 1u : 0u, a == 1u ? 1u : 0u, a == 2u ? 1u : 0u);
}

// Outward normal is MINUS the gradient: density is positive inside rock, so
// the gradient points into the solid. B1 learned this the visible way, by
// lighting the overhang on its underside. Taken from the analytic field rather
// than from the sampled grid so the mesh's shading matches the raymarch it is
// being checked against.
float3 field_normal(float3 p) {
    const float e = 0.5f * voxel;
    const float dx = trivial_density(p.x + e, p.y, p.z) - trivial_density(p.x - e, p.y, p.z);
    const float dy = trivial_density(p.x, p.y + e, p.z) - trivial_density(p.x, p.y - e, p.z);
    const float dz = trivial_density(p.x, p.y, p.z + e) - trivial_density(p.x, p.y, p.z - e);
    return normalize(-float3(dx, dy, dz));
}

// ---------------------------------------------------------------------------
// Pass 2 -- one vertex per cell that the surface passes through.
// ---------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void cs_vertices(uint3 id : SV_DispatchThreadID) {
    if (any(id >= kCells)) return;
    const uint ci = cell_index(id);
    g_cell_vertex[ci] = 0xFFFFFFFFu;

    float d[8];
    uint  solid = 0;
    [unroll] for (uint i = 0; i < 8; ++i) {
        d[i] = g_density[corner_index(id + corner_offset(i))];
        if (d[i] > 0.0f) solid |= (1u << i);
    }
    // Entirely inside or entirely outside: no surface here.
    if (solid == 0u || solid == 255u) return;

    // Average of the edge crossings. Linear interpolation along each edge is
    // exact for a field that is locally linear and is what makes the surface
    // land between samples instead of on voxel boundaries -- without it this
    // is marching cubes on a grid and looks like Minecraft.
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    uint   n = 0;
    [unroll] for (uint e = 0; e < 12; ++e) {
        const uint a = kEdges[e].x, b = kEdges[e].y;
        const bool sa = (solid & (1u << a)) != 0u;
        const bool sb = (solid & (1u << b)) != 0u;
        if (sa == sb) continue;
        const float da = d[a], db = d[b];
        const float denom = da - db;
        const float t = abs(denom) > 1e-12f ? saturate(da / denom) : 0.5f;
        sum += lerp(float3(corner_offset(a)), float3(corner_offset(b)), t);
        ++n;
    }
    if (n == 0) return;

    const float3 local = sum / float(n);
    const float3 world = chunk_origin + (float3(id) + local) * voxel;

    uint slot;
    InterlockedAdd(g_counters[0], 1u, slot);
    if (slot >= kMaxVertices) return;   // buffer full; pass 3 drops the quads

    Vertex v;
    v.pos = world;
    v.nrm = field_normal(world);
    g_vertices[slot] = v;
    g_cell_vertex[ci] = slot;
}

// ---------------------------------------------------------------------------
// Pass 3 -- a quad per sign-changing edge, joining the four cells around it.
// ---------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void cs_quads(uint3 id : SV_DispatchThreadID) {
    if (any(id >= kCells)) return;
    // An edge's four cells are at -1 in the two axes it does not run along, so
    // the lower faces of the chunk have no quads. That is the chunk seam, and
    // it is B3's problem (skirts), not a bug here.
    if (any(id < 1u)) return;

    const float d0 = g_density[corner_index(id)];
    const bool  s0 = d0 > 0.0f;

    [unroll] for (uint axis = 0; axis < 3; ++axis) {
        const uint3 step = unit_axis(axis);
        const bool s1 = g_density[corner_index(id + step)] > 0.0f;
        if (s0 == s1) continue;

        // The two axes the edge does not run along, taken as the CYCLIC
        // successors so that ua x ub = step for every axis. Picking them in a
        // fixed order instead (x,z for the y edge) makes that one case
        // left-handed, which winds its quads backwards -- and because a
        // near-horizontal surface changes sign almost entirely along vertical
        // edges, back-face culling then removes most of the terrain. That is
        // exactly what the first run of B2 looked like.
        const uint3 ua = unit_axis((axis + 1u) % 3u);
        const uint3 ub = unit_axis((axis + 2u) % 3u);

        const uint c00 = g_cell_vertex[cell_index(id - ua - ub)];
        const uint c10 = g_cell_vertex[cell_index(id - ub)];
        const uint c11 = g_cell_vertex[cell_index(id)];
        const uint c01 = g_cell_vertex[cell_index(id - ua)];
        if (c00 == 0xFFFFFFFFu || c10 == 0xFFFFFFFFu ||
            c11 == 0xFFFFFFFFu || c01 == 0xFFFFFFFFu) continue;

        uint base;
        InterlockedAdd(g_counters[1], 6u, base);
        if (base + 6u > kMaxIndices) return;

        // Wind so the front face is the one on the air side. Which end of the
        // edge is solid decides that, so the two cases are mirrored.
        //
        // The order below was settled by measurement, not by reasoning about
        // handedness, because there are three sign conventions stacked here:
        // the ring's orientation relative to the edge, D3D's y-down viewport,
        // and FrontCounterClockwise. Getting an even number of them wrong
        // looks identical to getting none wrong. The test that decides it is
        // rendering the same chunk with CULL_NONE and with CULL_BACK and
        // comparing the covered pixel count -- equal means every triangle
        // faces the camera, and anything less means some are inverted.
        if (s0) {
            g_indices[base + 0] = c00; g_indices[base + 1] = c11; g_indices[base + 2] = c10;
            g_indices[base + 3] = c00; g_indices[base + 4] = c01; g_indices[base + 5] = c11;
        } else {
            g_indices[base + 0] = c00; g_indices[base + 1] = c10; g_indices[base + 2] = c11;
            g_indices[base + 3] = c00; g_indices[base + 4] = c11; g_indices[base + 5] = c01;
        }
    }
}

// ---------------------------------------------------------------------------
// Pass 4 -- publish the counts as indirect draw arguments.
//
// One thread. This exists so the CPU never has to read the counter back to
// know how much to draw: a readback here would stall the frame, and the
// design's whole streaming story is built on not doing that.
// ---------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void cs_args(uint3 id : SV_DispatchThreadID) {
    g_draw_args[0] = min(g_counters[1], kMaxIndices);   // IndexCountPerInstance
    g_draw_args[1] = 1;                                 // InstanceCount
    g_draw_args[2] = 0;                                 // StartIndexLocation
    g_draw_args[3] = 0;                                 // BaseVertexLocation
    g_draw_args[4] = 0;                                 // StartInstanceLocation
}
