// ---------------------------------------------------------------------------
// GPU surface nets. B2 meshed one chunk; B3 meshes many into slots of a shared
// pool, so the whole resident world is one ExecuteIndirect.
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
// preserve, so the solve would cost without buying anything.
// ---------------------------------------------------------------------------

// M1: the real generator. The field function arrives through the ADR-001
// dialect unmodified; real_field.hlsl is the caller's side of the line --
// the spatial indices, which are written per backend.
#include "engine/field/real_field.hlsl"

// A chunk OWNS 32 cells; neighbouring chunk origins are 32 voxels apart. It
// MESHES 33, one ghost layer on the high side, because the quad joining its
// last cell to the neighbour's first belongs to neither chunk and leaves a
// one-cell crack otherwise. The ghost vertices duplicate the neighbour's, at
// the same positions from the same field, so the surfaces meet exactly.
//
// LOD changes the voxel SIZE, never these counts -- a coarser level is the
// same 32 cells over more metres, which is what lets one kernel serve every
// level.
static const uint kOwnedCells = 32;
static const uint kCells      = 33;   // owned + one ghost layer
static const uint kCorners    = 34;

cbuffer ChunkConstants : register(b0) {
    float3 chunk_origin;   // world position of corner (0,0,0)
    float  voxel;          // metres per voxel at this LOD
    uint   slot;           // which slot of the pool this chunk owns
    uint   vbase;          // first vertex of the slot
    uint   ibase;          // first index of the slot
    uint   slot_caps;      // vertex cap in the low 16 bits, index cap in the high
    uint   brick_first;    // this chunk's run in the brick header array
    uint   brick_count;
    float  skirt;          // how many voxels the lip drops, from the C++ side
    uint   pad1;
};

// The edit overlay. Edits live on the LOD 0 lattice whatever LOD is meshing,
// so a coarse chunk point-samples them -- a dug hole aliases at distance
// rather than disappearing.
static const float kEditVoxel = 0.5f;
static const int   kBrickSide = 16;
static const uint  kBrickVoxels = 4096u;
static const uint  kBrickMaskWords = 128u;

struct Vertex {
    float3 pos;
    float3 nrm;
};

RWStructuredBuffer<float>  g_density     : register(u0);   // scratch, kCorners^3
RWStructuredBuffer<uint>   g_cell_vertex : register(u1);   // scratch, kCells^3
RWStructuredBuffer<Vertex> g_vertices    : register(u2);   // pool
RWStructuredBuffer<uint>   g_indices     : register(u3);   // pool
RWStructuredBuffer<uint>   g_counters    : register(u4);   // 2 per slot
RWStructuredBuffer<uint>   g_draw_args   : register(u5);   // 5 per slot
RWStructuredBuffer<float>  g_heights     : register(u6);   // kCorners^2 per slot
RWStructuredBuffer<uint>   g_chunk_flags : register(u7);   // 1 per slot
RWStructuredBuffer<int4>   g_brick_hdr   : register(u8);   // (bx, by, bz, slot)
RWStructuredBuffer<float>  g_brick_den   : register(u9);   // kBrickVoxels per slot
RWStructuredBuffer<uint>   g_brick_mask  : register(u10);  // kBrickMaskWords per slot

// A column with no surface in it. Jolt spells its equivalent
// HeightFieldShapeConstants::cNoCollisionValue; the conversion happens on the
// CPU so this kernel does not have to know about a physics engine.
static const float kNoSurface = 1.0e30f;

uint vertex_cap() { return slot_caps & 0xFFFFu; }
uint index_cap()  { return (slot_caps >> 16) & 0xFFFFu; }

uint corner_index(uint3 c) { return (c.z * kCorners + c.y) * kCorners + c.x; }
uint cell_index(uint3 c)   { return (c.z * kCells   + c.y) * kCells   + c.x; }

float3 corner_world(uint3 c) {
    return chunk_origin + float3(c) * voxel;
}

// Integer floor-divide. The world is centred on the origin (invariant 6) so
// voxel coordinates go negative, and HLSL's integer division truncates toward
// zero -- which would put voxel -1 and voxel 0 in the same brick.
int brick_of(int v) {
    return v >= 0 ? (v / kBrickSide) : -(((-v) + kBrickSide - 1) / kBrickSide);
}

// The player's value at this point, if there is one.
bool edited_density(float3 p, out float out_d) {
    out_d = 0.0f;
    if (brick_count == 0u) return false;

    const int vx = (int)floor(p.x / kEditVoxel);
    const int vy = (int)floor(p.y / kEditVoxel);
    const int vz = (int)floor(p.z / kEditVoxel);
    const int bx = brick_of(vx), by = brick_of(vy), bz = brick_of(vz);

    for (uint i = 0; i < brick_count; ++i) {
        const int4 hdr = g_brick_hdr[brick_first + i];
        if (hdr.x != bx || hdr.y != by || hdr.z != bz) continue;
        const uint lx = (uint)(vx - bx * kBrickSide);
        const uint ly = (uint)(vy - by * kBrickSide);
        const uint lz = (uint)(vz - bz * kBrickSide);
        const uint li = (lz * 16u + ly) * 16u + lx;
        const uint sl = (uint)hdr.w;
        if ((g_brick_mask[sl * kBrickMaskWords + (li >> 5u)] & (1u << (li & 31u))) == 0u) {
            return false;   // the brick exists but has no opinion here
        }
        out_d = g_brick_den[sl * kBrickVoxels + li];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Pass 0 -- zero this slot's append counters.
//
// A kernel rather than ClearUnorderedAccessViewUint, which needs the UAV in
// both a shader-visible and a non-shader-visible descriptor heap. Two heaps
// and two descriptors to zero eight bytes is not a trade worth making.
// ---------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    g_counters[slot * 2u + 0u] = 0;
    g_counters[slot * 2u + 1u] = 0;
    g_chunk_flags[slot] = 0;
}

// Retire a slot: an all-zero draw is skipped by the command processor, so an
// evicted chunk costs nothing but its place in the indirect argument array.
[numthreads(1, 1, 1)]
void cs_retire(uint3 id : SV_DispatchThreadID) {
    for (uint i = 0; i < 5u; ++i) g_draw_args[slot * 5u + i] = 0;
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
    // The overlay wins where it has a value, because what it stores is
    // absolute. Everywhere else -- which is almost everywhere -- the field
    // answers and nothing was stored at all.
    float e = 0.0f;
    g_density[corner_index(id)] =
        edited_density(p, e) ? e : real_terrain_density(p.x, p.y, p.z, voxel);
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
// checked against.
float3 field_normal(float3 p) {
    // The arm does NOT scale with the voxel. It used to, at half a voxel,
    // which is 0.25 m at LOD 0 and 32 m at LOD 7 -- and a central difference
    // with a 32 m arm is not a gradient, it is a difference between two
    // unrelated pieces of terrain. Measured by --normal-check against the CPU
    // gradient: 14.8 degrees of mean error at a 64 m voxel, worst 170, and
    // 6.0% of normals pointing more than 90 degrees away from the surface's
    // own. A face lit as though it pointed away from the sun is a black
    // polygon in daylight, and that is exactly what the dark patches on
    // distant terrain were -- not aliased overhangs, which is what they were
    // confidently explained as first.
    //
    // The density function already fades octaves by `voxel`, so the field
    // being sampled here is smooth at coarse levels. A short arm on a smooth
    // field is the correct gradient; a long arm on it is still wrong.
    const float e = min(0.5f * voxel, 0.25f);
    float3 g = float3(
        real_terrain_density(p.x + e, p.y, p.z, voxel) - real_terrain_density(p.x - e, p.y, p.z, voxel),
        real_terrain_density(p.x, p.y + e, p.z, voxel) - real_terrain_density(p.x, p.y - e, p.z, voxel),
        real_terrain_density(p.x, p.y, p.z + e, voxel) - real_terrain_density(p.x, p.y, p.z - e, voxel));

    // Inside a fully carved cave, layer 6 pins the density at exactly
    // -cave_carve, so over a short arm the field is CONSTANT and the gradient
    // is exactly zero. normalize(0) is NaN, and a NaN normal shades to pure
    // black -- the very thing the old long arm was being blamed for. The long
    // arm never hit this because it reached out of the cave.
    //
    // Widening the arm where the short one finds nothing was the obvious
    // answer and does not fit: field_normal already inlines six whole field
    // evaluations, FXC inlines the lot, and twelve is
    // "error X4505: Sum of temp registers ... exceeds limit of 4096".
    // So: a fixed direction. It is arbitrary, but the surface normal in a
    // saturated void is genuinely undefined, and "up" is deterministic, never
    // NaN, and shades as lit ground rather than as a black hole.
    if (dot(g, g) < 1e-12f) return float3(0.0f, 1.0f, 0.0f);
    return normalize(-g);
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
    float3 world = chunk_origin + (float3(id) + local) * voxel;

    // The skirt, such as it is. Adjacent chunks at DIFFERENT levels put their
    // vertices on different grids, so the two surfaces do not meet exactly and
    // a sliver of sky shows between them. streamer.h has claimed since B3 that
    // "skirts cover it -- see the skirt pass"; there was no skirt pass, and
    // the claim went unchallenged because at five levels the seams were close
    // to the camera and sub-pixel. Eight levels put seven seams in frame and
    // took a ground shot from 71 cracks to 569.
    //
    // Dropping the outermost ring of cells turns each chunk's edge into a
    // downward lip, and two neighbouring lips overlap across their depth
    // whichever way the surfaces disagree. Only X and Z: vertically stacked
    // chunks are always the same level, share a ghost layer and agree exactly,
    // so there is nothing to cover there.
    //
    // Two voxels deep because the coarser neighbour is exactly twice this
    // chunk's voxel and the harness bounds LOD disagreement at one voxel of
    // the coarser level. The lip sits on the ghost ring, which is the cell the
    // neighbour starts at, so the neighbour's own surface covers it.
    //
    // THAT WAS WRONG, and this replaced it. The lip dropped the cells at
    // id == 0 AND id == kCells - 1, and a chunk's ghost cell is its
    // neighbour's cell 0 -- so the one row of vertices two same-LOD
    // neighbours share was dropped by BOTH of them. Nothing covered it: this
    // chunk's quads end at that row and the neighbour's begin there. Every
    // chunk border was a V groove two voxels deep, 1 m at LOD 0 and 8 m at
    // LOD 3, and where it crossed a ridge the silhouette had a V notch cut in
    // it. A/B with the lip at 0 made them disappear (and let 1147 crack
    // pixels through, which is what the lip was for).
    //
    // Same-LOD neighbours need nothing: they compute that shared row from the
    // same densities and weld exactly. The crack is only ever between levels,
    // so the cover is now a real skirt -- EXTRA geometry, never a moved
    // surface vertex. A border cell gets a second vertex two voxels into the
    // solid along -normal (into rock under an overhang too, not just down),
    // and cs_quads hangs a curtain between the two from every mesh-border
    // edge. Every stored surface vertex is now true.
    //
    // The skirt vertex carries its normal at HALF length. Shading normalises,
    // so it looks identical; the CPU tests the length (is_skirt_vertex) to
    // keep the curtain out of physics and out of every check that measures a
    // vertex against the field. Do not normalise vertex normals in a kernel.
    const float3 nrm = field_normal(world);
    const bool border = id.x == 0u || id.x == kCells - 1u ||
                        id.z == 0u || id.z == kCells - 1u;
    const uint need = border ? 2u : 1u;

    uint local_slot;
    InterlockedAdd(g_counters[slot * 2u + 0u], need, local_slot);
    if (local_slot + need > vertex_cap()) return;   // slot full; pass 3 drops the quads

    Vertex v;
    v.pos = world;
    v.nrm = nrm;
    g_vertices[vbase + local_slot] = v;
    // Slot-relative, so the draw's BaseVertexLocation does the rebasing and an
    // index never has to be rewritten when a chunk moves slot. Packed: the
    // surface vertex in the low 16 bits, the skirt vertex (or 0xFFFF) in the
    // high 16. A slot holds 4608 vertices, well inside 16 bits.
    uint skirt_slot = 0xFFFFu;
    if (border) {
        Vertex s;
        s.pos = world - nrm * (voxel * skirt);
        s.nrm = nrm * 0.5f;
        skirt_slot = local_slot + 1u;
        g_vertices[vbase + skirt_slot] = s;
    }
    g_cell_vertex[ci] = (local_slot & 0xFFFFu) | (skirt_slot << 16);
}

// Is the edge from cell a to cell b on the chunk's outer X/Z boundary -- the
// edge of the mesh, with no quad beyond it in this chunk? c is a cell of the
// same quad off that edge. True when a and b lie on a border face and c is
// one cell inside it.
bool border_side(uint3 a, uint3 b, uint3 c) {
    const uint hi = kCells - 1u;
    if (a.x == b.x && ((a.x == hi && c.x == hi - 1u) || (a.x == 0u && c.x == 1u))) return true;
    if (a.z == b.z && ((a.z == hi && c.z == hi - 1u) || (a.z == 0u && c.z == 1u))) return true;
    return false;
}

// A curtain quad from a mesh-border edge down to its skirt vertices, both
// windings: which side of it faces the viewer depends on which of the two
// LODs at the seam is higher there, so either can be the one seen.
void emit_curtain(uint a, uint b) {
    const uint sa = a >> 16, sb = b >> 16;
    if (sa == 0xFFFFu || sb == 0xFFFFu) return;
    uint base;
    InterlockedAdd(g_counters[slot * 2u + 1u], 12u, base);
    if (base + 12u > index_cap()) return;
    const uint o = ibase + base;
    const uint va = vbase + (a & 0xFFFFu), vb = vbase + (b & 0xFFFFu);
    const uint wa = vbase + sa, wb = vbase + sb;
    g_indices[o + 0] = va; g_indices[o + 1] = vb; g_indices[o + 2]  = wb;
    g_indices[o + 3] = va; g_indices[o + 4] = wb; g_indices[o + 5]  = wa;
    g_indices[o + 6] = va; g_indices[o + 7] = wb; g_indices[o + 8]  = vb;
    g_indices[o + 9] = va; g_indices[o + 10] = wa; g_indices[o + 11] = wb;
}

// ---------------------------------------------------------------------------
// Pass 3 -- a quad per sign-changing edge, joining the four cells around it.
// ---------------------------------------------------------------------------
[numthreads(4, 4, 4)]
void cs_quads(uint3 id : SV_DispatchThreadID) {
    if (any(id >= kCells)) return;
    // An edge's four cells are at -1 in the two axes it does not run along, so
    // the chunk's low faces still have no quads -- those belong to the
    // neighbour, which reaches them with its own ghost layer.
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

        const uint3 p00 = id - ua - ub, p10 = id - ub, p11 = id, p01 = id - ua;
        const uint k00 = g_cell_vertex[cell_index(p00)];
        const uint k10 = g_cell_vertex[cell_index(p10)];
        const uint k11 = g_cell_vertex[cell_index(p11)];
        const uint k01 = g_cell_vertex[cell_index(p01)];
        // Low 16 bits: the surface vertex (0xFFFF: none). High: the skirt's.
        const uint c00 = k00 & 0xFFFFu, c10 = k10 & 0xFFFFu;
        const uint c11 = k11 & 0xFFFFu, c01 = k01 & 0xFFFFu;
        if (c00 == 0xFFFFu || c10 == 0xFFFFu || c11 == 0xFFFFu || c01 == 0xFFFFu) continue;

        // The skirt: a curtain from each side of this quad that is the edge of
        // the chunk's mesh. A side whose two cells sit on a border face, with
        // the quad's other cells one step inside, has nothing beyond it here.
        if (border_side(p00, p10, p01)) emit_curtain(k00, k10);
        if (border_side(p10, p11, p00)) emit_curtain(k10, k11);
        if (border_side(p11, p01, p10)) emit_curtain(k11, k01);
        if (border_side(p01, p00, p11)) emit_curtain(k01, k00);

        uint base;
        InterlockedAdd(g_counters[slot * 2u + 1u], 6u, base);
        if (base + 6u > index_cap()) return;

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
        // ABSOLUTE indices into the pool, not slot-relative.
        //
        // The tidy version of this stores slot-relative indices and lets the
        // draw's BaseVertexLocation rebase them. That relies on SV_VertexID
        // including BaseVertexLocation, which is a property of the input
        // assembler's vertex fetch -- and this shader has no input layout, it
        // reads a StructuredBuffer by SV_VertexID. Every chunk then read slot
        // 0's range, which is empty, so 662,000 triangles collapsed to the
        // origin and the screen was blank while every counter looked right.
        //
        // Adding vbase here costs nothing and depends on no convention. The
        // only consequence is that indices are tied to the slot they were
        // meshed into, which is already true: a chunk is remeshed whenever it
        // becomes resident.
        const uint o = ibase + base;
        const uint v00 = vbase + c00, v10 = vbase + c10;
        const uint v11 = vbase + c11, v01 = vbase + c01;
        if (s0) {
            g_indices[o + 0] = v00; g_indices[o + 1] = v11; g_indices[o + 2] = v10;
            g_indices[o + 3] = v00; g_indices[o + 4] = v01; g_indices[o + 5] = v11;
        } else {
            g_indices[o + 0] = v00; g_indices[o + 1] = v10; g_indices[o + 2] = v11;
            g_indices[o + 3] = v00; g_indices[o + 4] = v11; g_indices[o + 5] = v01;
        }
    }
}

// ---------------------------------------------------------------------------
// Pass 3b -- the surface height of every column, for physics.
//
// Reuses the density grid rather than re-evaluating the field: the same
// samples that produced the mesh produce the shape, so the two cannot
// disagree about where the ground is. A separate CPU evaluation would be a
// second source of truth for the one thing the player stands on.
//
// Highest surface wins. The trivial field has a floating sphere over the
// ground, and a heightfield can only represent one surface per column -- B5's
// MeshShape is where overhangs stop being approximated. Taking the highest
// means a character walks on top of the sphere rather than through it.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void cs_heights(uint3 id : SV_DispatchThreadID) {
    if (id.x >= kCorners || id.y >= kCorners) return;

    float h = kNoSurface;
    uint  floors = 0;     // air above, rock below: a surface you can stand on
    uint  ceilings = 0;   // rock above, air below: the underside of something
    // Downwards from the top, and the whole column is walked rather than
    // stopping at the first floor, because what decides whether a heightfield
    // can hold this chunk is the shape of the WHOLE column.
    for (uint j = kCorners - 1u; j >= 1u; --j) {
        const float above = g_density[corner_index(uint3(id.x, j, id.y))];
        const float below = g_density[corner_index(uint3(id.x, j - 1u, id.y))];
        if (above > 0.0f && below <= 0.0f) ++ceilings;
        if (above <= 0.0f && below > 0.0f) {
            ++floors;
            if (floors == 1u) {
                // Linear crossing between the two samples, the same
                // interpolation the mesher uses along an edge. t is the
                // fraction from the SOLID sample (j-1) up to the air sample
                // (j), so the height is measured from j-1, not down from j.
                const float denom = below - above;
                const float t = abs(denom) > 1e-12f ? saturate(below / denom) : 0.5f;
                h = chunk_origin.y + (float(j - 1u) + t) * voxel;
            }
        }
    }
    g_heights[slot * (kCorners * kCorners) + id.y * kCorners + id.x] = h;

    // A heightfield says "rock below one height, air above", so it represents
    // this column exactly only when the column is all air, all rock, or has a
    // single floor and no ceiling.
    //
    // A CEILING is the decisive one, and counting only floors misses it. The
    // underside of the trivial field's floating sphere is rock above air: no
    // floor at all within that chunk, so a floors-only test called the whole
    // world representable and classified 0 of 816 chunks as needing a mesh.
    // It is exactly the case a heightfield fills in solid.
    if (ceilings > 0u || floors > 1u) {
        uint ignored;
        InterlockedAdd(g_chunk_flags[slot], 1u, ignored);
    }
}

// ---------------------------------------------------------------------------
// Pass 4 -- publish this slot's counts as indirect draw arguments.
//
// One thread. This exists so the CPU never has to read a counter back to know
// how much to draw: a readback here would stall the frame, and the design's
// whole streaming story is built on not doing that. With one argument entry
// per slot, the entire resident world is a single ExecuteIndirect.
// ---------------------------------------------------------------------------
[numthreads(1, 1, 1)]
void cs_args(uint3 id : SV_DispatchThreadID) {
    const uint n = min(g_counters[slot * 2u + 1u], index_cap());
    g_draw_args[slot * 5u + 0u] = n;       // IndexCountPerInstance
    g_draw_args[slot * 5u + 1u] = 1;       // InstanceCount
    g_draw_args[slot * 5u + 2u] = ibase;   // StartIndexLocation
    g_draw_args[slot * 5u + 3u] = 0;       // BaseVertexLocation: indices are absolute
    g_draw_args[slot * 5u + 4u] = 0;       // StartInstanceLocation
}
