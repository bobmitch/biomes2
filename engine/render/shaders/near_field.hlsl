// ---------------------------------------------------------------------------
// The near field's update (near_field.h): light under the top surface.
//
//   cs_nf_fill   an 8 m block of the occupancy volume's density, from the
//                field at 1 m (the GPU's real_terrain_density, as the mesher
//                evaluates it), for each block that scrolled in or was dug.
//   cs_nf_edits  the player's edits over those blocks, scattered from the
//                CPU (absolute values at the 1 m points, invariant 5).
//   cs_nf_occ    occupancy from density, a 1 m ramp across the surface.
//   cs_nf_mip    occupancy mips to 16 m (the rays skip empty space by them).
//   cs_nf_coarse the COARSE level: per 8 m block, a probe placed in its
//                covered air, 256 rays into 64 equal-area octahedral bins,
//                counted from kNfSplit (6 m) to 48 m.
//   cs_nf_probe  the fine level: per 2 m probe, 64 rays, one a bin, marched
//                through the occupancy at 1 m until they rise above the
//                heightfield GI's top surface -- out in the open, where its
//                maps say what light is there -- or meet rock, or get
//                kNfSplit clear, where they take the coarse level's bin
//                (nf_merge). L1 SH, the heightfield's own convention,
//                blended over updates as DDGI's probes are.
//
// The two levels are Split Radiance Cascades' idea (docs/2607.20384v2.pdf)
// on this volume: a coarser level with more directions covering the longer
// interval, merged into the finer. Not its hash map, its screen-space ray
// split or its hardware rays.
//
// A ray that meets rock carries what that wall sends back (nf_hit): the
// sun on it through the cascades and the clouds, for a fine hit every local
// light on it through its own shadow, and the probes round it -- every
// further bounce, a frame at a time -- times the wall's albedo over pi.
// Deep and unlit is still dark: nothing there to send anything. Glowing
// things are meant to be local lights (an algae pool is a few dim
// unshadowed ones), so they reach the walls and the bounce the same way a
// torch does.
// ---------------------------------------------------------------------------
#include "engine/field/real_field.hlsl"
#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/lighting.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/near_field_common.hlsli"
Texture2D<float> g_cloud_shadow : register(t60);   // clouds.hlsl cs_shadow, last frame's
#include "engine/render/shaders/cloud_shadow.hlsli"

cbuffer NfConstants : register(b0) {
    int4 nf_origin;    // the volume's min world voxel (a multiple of 8); w unused
    uint nf_edits;     // entries in g_edits
    uint nf_probes;    // entries in g_probe_list
    float nf_keep_box;   // a frame's blend at the fine memory (near_field.h Schedule): a still's base, a probe traced from the CPU's list
    float nf_keep_rr;    // the same (unused)
    float4 nf_rot0, nf_rot1, nf_rot2;   // unused since the rays are octahedral bins (the layout keeps them)
    float4 nf_cloud;                    // the clouds' shadow map: centre x, z, half extent (0: none), base
    uint   nf_lights;                   // local lights to light a near hit with (the frame's first, most important)
    uint   nf_coarse;                   // entries in g_coarse_list
    float  nf_keep_coarse;              // the coarse level's blend over history
    float  nf_keep_light;               // probes a moving or new light reaches (the list's bit 29)
    uint   nf_seed;                     // this update's jitter of the coarse rays
    uint   nf_dbg;                      // BIOMES_NF_DBG (near_field.cpp record)
    uint   nf_gate;                     // 1: probes in sealed air with no light near do not trace (BIOMES_NF_NOGATE: 0)
    uint   nf_settle;                   // a still: its updates before this frame's (iteration nf_level adds); ~0 in play
    // The live pass (play): every probe under cover traced every frame.
    // Blends for one frame (fine, a fast light's, coarse); then the fast
    // lights (x, y, z, reach^2).
    float4 nf_live;                     // keep fine, keep by a fast light, keep coarse, the coarse rays' reach (m; 0: 48)
    uint4  nf_live2;                    // fast lights, plans so far, the fine and coarse strides (frames)
    float4 nf_fast[8];
};
// A dug wall's albedo: soil and rock, a little warm. The terrain's own
// material per biome would be better (gaps.md); this is its mean.
static const float3 kNfWallAlbedo = float3(0.26f, 0.23f, 0.20f);
// 64 rays an update, and history kept in SECONDS by how often a probe is
// traced (the blends come from the CPU, near_field.h Schedule). 32 rays and
// 0.85 everywhere made the probes round the camera DANCE wherever the sky
// came in through a small opening (a stairwell 10 m down): a few of 32 rays
// find it, a different few each update.
static const int   kNfRays = 64;
// The two levels (near_field.h): the fine probes trace 0-kNfSplit, the
// coarse ones kNfSplit-48 m into 8 x 8 octahedral bins, four rays a bin.
static const float kNfSplit = 6.0f;
static const int3  kNfCoarseDims = int3(16, 8, 16);
static const uint  kNfCoarse = 2048u;
static const int   kNfCoarseRays = 256;
// A fine ray that gets past kNfSplit with no coarse probe to take (none
// placed near, or none it can see) marches on itself, and only there is the
// sun at its hit faded out over this ray length: the pulse a far sunlit
// sliver made before the coarse level (nf_hit).
static const float kNfSunNear = 4.0f, kNfSunFar = 12.0f;
static const float kNfCoarseSunFar = 24.0f;
// A local light at a near hit is never nearer than this: the inverse square
// at a flame a few centimetres from the rock lit a probe tenfold.
static const float kNfLightNear = 0.5f;
cbuffer NfLevel : register(b3) { uint nf_level; };

StructuredBuffer<int4>   g_blocks     : register(t2);   // world block (8 m units) to fill
StructuredBuffer<float4> g_edits      : register(t3);   // x: storage index (bits), y: density
StructuredBuffer<uint>   g_probe_list : register(t4);   // probe storage indices to update
StructuredBuffer<uint>   g_coarse_list : register(t5);  // coarse probe (block) storage indices to update
// Sky access (sky_access.h): per entry, a world block (8 m units, 3 ints and
// a pad) and 16 words, a bit per 1 m voxel (x + 8 y + 64 z) of air the
// CPU's flood found no path from to the open sky.
StructuredBuffer<uint>   g_marks      : register(t6);
// The probe visibility volume (hfgi.hlsli nf_sample): per 1 m voxel v, a bit
// per probe P = floor(v / 2) + d, d in {-1, 0, 1}^3, bit (dx+1) + 3 (dy+1) +
// 9 (dz+1), set when a reader there sees that probe. Every 2 m cell a reader
// within half a metre of v can fall in has its eight probes among those 27.
// And the blocks to compute it for (8 m units).
RWTexture3D<uint>        u_vis        : register(u21);
StructuredBuffer<int4>   g_vis_blocks : register(t7);
RWTexture3D<float>       u_density    : register(u0);
RWTexture3D<float>       u_occ[5]     : register(u1);   // mips 0..4 (u1..u5)
RWStructuredBuffer<float4> u_probes   : register(u6);
RWTexture2D<float>       u_top        : register(u7);   // per 1 m column, the topmost rock's surface
RWStructuredBuffer<float4> u_coarse   : register(u8);   // the coarse level (near_field.h kCoarseFloat4s)
// The live lists (near_field.h kAux*): per probe its mode (0 unknown, 1 a
// copy of the heightfield, 2 off, 3 traced); the fine and coarse lists; their
// counts. And the live passes' dispatch arguments.
RWStructuredBuffer<uint> u_aux        : register(u9);
RWStructuredBuffer<uint> u_args       : register(u10);
static const uint kNfProbes = 131072u;
static const uint kAuxFine = kNfProbes, kAuxCoarse = 2u * kNfProbes, kAuxCounts = 2u * kNfProbes + 2048u;
// The lists' two counts, in one of two slots by the plan's parity: the args
// kernel zeroes the other for the next frame, which saves a dispatch.
uint nf_count_slot() { return kAuxCounts + 2u * (nf_live2.y & 1u); }
// The mode word: bit 2, traced fresh (an edit or a scroll), set by the
// classify pass, cleared by the trace.
static const uint kNfModeFresh = 4u;
// Bit 3: a probe that would trace, GATED -- in sealed air with no light near
// (nf_gated) -- and held dark instead. cs_nf_live checks it every frame.
static const uint kNfModeGated = 8u;
// At most this many fine probes a frame in the live pass; past it the list
// is walked in turns and each probe's blend covers its interval.
static const uint kNfLiveMax = 8192u;

// The live lists are traced in turns, stride frames for the whole of each
// (nf_live2.z fine, .w coarse): this frame's share of a list of n, and the
// frames each probe's blend covers. Past kNfLiveMax a frame, longer turns.
void nf_live_share(uint n, uint stride, uint cap, out uint start, out uint count, out uint interval) {
    const uint turns = max(stride, (n + cap - 1u) / cap);
    const uint chunk = (n + turns - 1u) / turns;
    start = min((nf_live2.y % turns) * chunk, n);
    count = min(chunk, n - start);
    interval = turns;
}
SamplerState             g_nf_wrap    : register(s6);   // toroidal: wraps

uint nf_coarse_index(int3 b) {
    const int3 s = nf_wrap(b, kNfCoarseDims);
    return uint((s.z * kNfCoarseDims.y + s.y) * kNfCoarseDims.x + s.x);
}

[numthreads(8, 8, 8)]
void cs_nf_fill(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const int3 w = g_blocks[g.x].xyz * 8 + int3(t);
    u_density[nf_wrap(w, kNfDims)] = real_terrain_density(float(w.x), float(w.y), float(w.z), 1.0f);
    // The block's probes are off until traced again (the plan queues them).
    // The volume is toroidal: a slot that scrolls in still held a probe from
    // the far side -- daylight from the surface, say -- and a neighbour's hit
    // read it through nf_hit_irradiance and passed it round the bounce loop.
    // Walking into a tunnel, its darkest tenth drained from 0.2 to 0.02 nits
    // over half a minute: a slow "eye opening" that was the probes.
    if (all((t & 1u) == 0u)) {
        const uint o4 = nf_probe_index(w / 2) * uint(kNfProbeFloat4s);
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        u_aux[nf_probe_index(w / 2)] = 0u;   // unknown: off the live list until traced again
    }
    // And the block's coarse probe is unplaced: traced again, from nothing.
    if (all(t == 0u)) u_coarse[nf_coarse_index(g_blocks[g.x].xyz)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}

[numthreads(64, 1, 1)]
void cs_nf_edits(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nf_edits) return;
    const float4 e = g_edits[id.x];
    const uint i = asuint(e.x);
    const int3 s = int3(i % uint(kNfDims.x), (i / uint(kNfDims.x)) % uint(kNfDims.y),
                        i / uint(kNfDims.x * kNfDims.y));
    u_density[s] = e.y;
}

// SEALED air -- no path through air to the open sky, by the CPU's flood
// (sky_access.h) -- is stored in the density volume less kNfSealBias. Its
// sign stays air and its occupancy stays 0 (saturate(0.5 + d)), so nothing
// that reads occupancy changes; a ray that leaves the volume through it is
// dark, not the heightfield's sky (nf_march). Only air is ever marked: the
// CPU's field agrees with this one to a tolerance, and a voxel it calls air
// that this one calls rock stays rock.
static const float kNfSealBias = 1000.0f;
float nf_unseal(float d) { return d <= -0.5f * kNfSealBias ? d + kNfSealBias : d; }
bool nf_sealed(float d) { return d <= -0.5f * kNfSealBias; }

// After the fill and the edits, before the occupancy: each listed block's
// marks, the old ones taken off first (a block re-marked as the flood learns
// more, a cave dug open).
[numthreads(8, 8, 8)]
void cs_nf_mark(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const uint e = g.x * 20u;
    const int3 w = int3(int(g_marks[e]), int(g_marks[e + 1u]), int(g_marks[e + 2u])) * 8 + int3(t);
    const uint idx = t.x + 8u * t.y + 64u * t.z;
    const bool bit = ((g_marks[e + 4u + (idx >> 5)] >> (idx & 31u)) & 1u) != 0u;
    const int3 s = nf_wrap(w, kNfDims);
    float d = nf_unseal(u_density[s]);
    if (bit && d < 0.0f) d -= kNfSealBias;
    u_density[s] = d;
}

// The visibility volume. The reader marched from its point to each of its
// eight probes every half metre (nf_clear) -- up to ~50 occupancy loads a
// pixel, which was the whole of its cost: 0.66 ms of a 3.2 ms terrain pass
// at 1440p on a screen of cave wall. The same march, once per voxel and
// probe, here, for the blocks filled this frame and the blocks round them
// (a march reaches ~3.5 m, a probe's placement 0.9 m).
//
// From the voxel's point, not the pixel's: a reader 0.4 m off the surface
// rounds to a voxel in rock as often as not, so a voxel in rock may climb
// up to a metre out of it before the line must stay clear. The probe stands
// where nf_probe_mode puts it: its point, or 0.9 m out of rock along an
// axis; nowhere (no bit) if it is switched off.
bool nf_vis_place(int3 P, out float3 pc) {
    const float3 c = float3(P * 2 + 1);
    pc = c;
    if (u_occ[0][nf_wrap(P * 2 + 1, kNfDims)] <= 0.5f) return true;
    static const float3 kTry[6] = {float3(0, 0.9f, 0), float3(0.9f, 0, 0), float3(-0.9f, 0, 0),
                                   float3(0, 0, 0.9f), float3(0, 0, -0.9f), float3(0, -0.9f, 0)};
    [loop] for (int q = 0; q < 6; ++q) {
        if (u_occ[0][nf_wrap(int3(floor(c + kTry[q] + 0.5f)), kNfDims)] < 0.5f) { pc = c + kTry[q]; return true; }
    }
    return false;
}

bool nf_vis_clear(float3 a, float3 b, bool a_rock) {
    const float3 v = b - a;
    const float len = length(v);
    const int n = max(int(ceil(len * 2.0f)), 1);
    bool climbing = a_rock;
    [loop] for (int i = 1; i <= n; ++i) {
        const float s = float(i) / float(n + 1);
        const bool rock = u_occ[0][nf_wrap(int3(floor(a + v * s + 0.5f)), kNfDims)] > 0.5f;
        if (!rock) { climbing = false; continue; }
        if (climbing && len * s <= 1.0f) continue;
        return false;
    }
    return !climbing || len <= 1.0f;
}

[numthreads(8, 8, 8)]
void cs_nf_vis(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const int3 w = g_vis_blocks[g.x].xyz * 8 + int3(t);
    const float3 a = float3(w);
    const bool a_rock = u_occ[0][nf_wrap(w, kNfDims)] > 0.5f;
    // Voxels no reader rounds to: rock with rock all round it (a reader is
    // in air, 0.4 m off a surface), and air 3 m over its column's top
    // surface (readers there take the heightfield). Most of a block that
    // scrolls in is one or the other; walking in the open paid ~0.05 ms a
    // frame for marching them.
    if (a_rock) {
        bool buried = true;
        [loop] for (uint n = 0u; n < 27u && buried; ++n) {
            const int3 d = int3(int(n % 3u), int((n / 3u) % 3u), int(n / 9u)) - 1;
            if (u_occ[0][nf_wrap(w + d, kNfDims)] <= 0.5f) buried = false;
        }
        if (buried) { u_vis[nf_wrap(w, kNfDims)] = 0u; return; }
    } else if (float(w.y) > u_top[nf_wrap(int3(w.x, 0, w.z), kNfDims).xz] + 3.0f) {
        u_vis[nf_wrap(w, kNfDims)] = 0x7FFFFFFu;
        return;
    }
    const int3 pc0 = w >> 1;   // arithmetic: floor(w / 2)
    uint mask = 0u;
    [loop] for (uint b = 0u; b < 27u; ++b) {
        const int3 d = int3(int(b % 3u), int((b / 3u) % 3u), int(b / 9u)) - 1;
        float3 pc;
        if (!nf_vis_place(pc0 + d, pc)) continue;
        if (nf_vis_clear(a, pc, a_rock)) mask |= 1u << b;
    }
    u_vis[nf_wrap(w, kNfDims)] = mask;
}

[numthreads(8, 8, 8)]
void cs_nf_occ(uint3 id : SV_DispatchThreadID) {
    // Density is about metres to the surface, positive in rock: occupancy
    // ramps across the metre round it, so a cone sees a surface inside a
    // voxel rather than a step at its edge.
    u_occ[0][id] = saturate(0.5f + u_density[id]);
}

[numthreads(4, 4, 4)]
void cs_nf_mip(uint3 id : SV_DispatchThreadID) {
    const int3 n = kNfDims >> int(nf_level);
    if (any(int3(id) >= n)) return;
    float s = 0.0f;
    [unroll] for (uint c = 0; c < 8u; ++c) s += u_occ[nf_level - 1u][id * 2u + uint3(c & 1u, (c >> 1) & 1u, c >> 2)];
    u_occ[nf_level][id] = s * 0.125f;
}

// The real top surface, per 1 m column: the first rock from the volume's top
// down, its surface found by the density's crossing. The readers decide
// "under cover" by it: the heightfield's 2 m map keeps the HIGHEST point of
// each texel, so ordinary bumpy ground sits up to a metre and a half under
// it, and testing that ground for a roof cost the terrain 0.3 ms at 1440p.
[numthreads(8, 8, 1)]
void cs_nf_top(uint3 id : SV_DispatchThreadID) {
    const int3 o = nf_origin.xyz;
    float top = -1.0e9f;
    float above = -1.0f;
    [loop] for (int y = kNfDims.y - 1; y >= 0; --y) {
        const int3 sw = nf_wrap(int3(int(id.x) - o.x, 0, int(id.y) - o.z), kNfDims);
        const int3 w = int3(o.x + sw.x, o.y + y, o.z + sw.z);
        const float d = u_density[nf_wrap(w, kNfDims)];
        if (d > 0.0f) {
            // Rock at the volume's top: buried deeper than it reaches.
            top = y == kNfDims.y - 1 ? float(w.y) + 1.0f : float(w.y) + d / max(d - above, 1e-4f);
            break;
        }
        above = nf_unseal(d);
    }
    u_top[id.xy] = top;
}

// --- probes -----------------------------------------------------------------

bool nf_inside(float3 wp) {
    const float3 rel = wp - float3(nf_origin.xyz);
    return all(rel >= 0.5f) && all(rel <= float3(kNfDims) - 1.5f);
}

// Whether the air round p (inside the volume) is sealed: the trilinear
// weights of the eight voxels round it, sealed air against open air (rock
// counts for neither). A ray's last point in air sits beside rock as often
// as not, and a 1 m wall between a sealed cave and an open one puts both
// kinds in one cell: the weights decide.
bool nf_sealed_at(float3 p) {
    const float3 f = floor(p);
    const float3 t = p - f;
    float ws = 0.0f, wo = 0.0f;
    [unroll] for (int c = 0; c < 8; ++c) {
        const int3 off = int3(c & 1, (c >> 1) & 1, c >> 2);
        const float3 tw = lerp(1.0f - t, t, float3(off));
        const float w = tw.x * tw.y * tw.z;
        const float d = u_density[nf_wrap(int3(f) + off, kNfDims)];
        if (d >= 0.0f) continue;
        if (nf_sealed(d)) ws += w;
        else wo += w;
    }
    return ws > wo;
}

float nf_occ_point(float3 wp) {
    return g_nf_occ.Load(int4(nf_wrap(int3(floor(wp + 0.5f)), kNfDims), 0));
}

// The heightfield's map at (x, z): its uv, and whether it holds ground there.
bool nf_hf_uv(float2 xz, out float2 uv, out float top) {
    uv = float2((xz.x - l_hf.x) / kHfExtent, 1.0f - (xz.y - l_hf.y) / kHfExtent);
    top = kHfNone;
    if (l_hf.w < 0.5f || any(uv <= 0.0f) || any(uv >= 1.0f)) return false;
    top = g_hf_height.SampleLevel(g_gi_linear, uv, 0.0f).x;
    return top > kHfNone * 0.5f;
}

// The key light's visibility at a hit: one tap of the third cascade, and
// the clouds' shadow over it. NOT the cascade by distance, as the mist takes
// it: the near two are fitted to slices of the VIEW, and a hit behind or
// beside the camera falls outside their maps, where the border reads "lit"
// -- full sun on the walls of a sealed tunnel 12 m down. The third is fitted
// round the camera (380 m), so it holds the whole volume; its 0.19 m texels
// are far finer than the volume's metre.
float nf_key_visibility(float3 p) {
    float v = 1.0f;
    if (shadow_res >= 0.0f) {
        const float d = length(p - shadow_cam);
        if (d < shadow_split.w) {
            const uint c = max(2u, uint(l_shadow_frame.z + 0.5f));
            const float4 lp = mul(float4(p, 1.0f), light_vp[c]);
            const float2 suv = float2(lp.x * 0.5f + 0.5f, 0.5f - lp.y * 0.5f) * shadow_scale[c];
            v = g_shadow.SampleCmpLevelZero(g_shadow_cmp, float3(suv, float(c)), lp.z - 0.12f / shadow_depth_range[c]);
        }
    }
    return v * cloud_shadow_at(g_cloud_shadow, g_gi_linear, nf_cloud, kSunDir, p);
}

// What reaches a hit from the probes round it -- the sky through the
// openings and every bounce so far -- read from the buffer being written
// (a probe updated earlier this pass is a frame fresher; DDGI reads last
// frame's). Rock between the hit and a probe drops it, as in the reader.
float3 nf_hit_irradiance(float3 p, float3 n) {
    const float3 q = p + n * 0.5f;
    const float3 f = (q - 1.0f) * 0.5f;
    const int3 p0 = int3(floor(f));
    const float3 t = f - float3(p0);
    const int3 op = nf_origin.xyz / 2;
    float4 sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    float wsum = 0.0f;
    [loop] for (int c = 0; c < 8; ++c) {
        const int3 o = int3(c & 1, (c >> 1) & 1, c >> 2);
        const int3 pq = p0 + o;
        if (any(pq < op) || any(pq >= op + kNfProbeDims)) continue;
        const uint i4 = nf_probe_index(pq) * uint(kNfProbeFloat4s);
        const float4 meta = u_probes[i4 + 4u];
        if (meta.x < 0.5f) continue;
        const float3 pc = float3(pq * 2 + 1) + meta.yzw;
        const float3 v = pc - q;
        if (!nf_clear(q, pc)) continue;
        const float3 tw = lerp(1.0f - t, t, float3(o));
        const float bf = 0.5f + 0.5f * dot(v * rsqrt(max(dot(v, v), 1e-6f)), n);
        const float w = tw.x * tw.y * tw.z * (bf * bf + 0.05f);
        sv += w * u_probes[i4 + 0u];
        sr += w * u_probes[i4 + 1u];
        sg += w * u_probes[i4 + 2u];
        sb += w * u_probes[i4 + 3u];
        wsum += w;
    }
    if (wsum <= 1e-6f) return float3(0.0f, 0.0f, 0.0f);
    const float inv = 1.0f / wsum;
    return kSkyMean * hf_irradiance(sv * inv, n) +
           float3(hf_irradiance(sr * inv, n), hf_irradiance(sg * inv, n), hf_irradiance(sb * inv, n));
}

float nf_occ_lin(float3 wp) {
    return g_nf_occ.SampleLevel(g_nf_wrap, (wp + 0.5f) / float3(kNfDims), 0.0f);
}

// The radiance a wall sends back along -d from hit point p (in air, just
// short of the rock), its normal from the occupancy's gradient. `sun_w`
// scales the sun (1 but on the fine level's fallback past kNfSplit);
// `lights`: the local lights too (the fine level's hits, within kNfSplit).
float3 nf_hit(float3 p, float3 d, float sun_w, bool lights) {
    const float e = 0.5f;
    const float3 g = float3(nf_occ_lin(p + float3(e, 0, 0)) - nf_occ_lin(p - float3(e, 0, 0)),
                            nf_occ_lin(p + float3(0, e, 0)) - nf_occ_lin(p - float3(0, e, 0)),
                            nf_occ_lin(p + float3(0, 0, e)) - nf_occ_lin(p - float3(0, 0, e)));
    float3 n = dot(g, g) > 1e-8f ? -normalize(g) : -d;
    if (dot(n, d) > 0.0f) n = -n;   // it faces the ray that found it
    float3 E = float3(0.0f, 0.0f, 0.0f);
    // The sun. The volume's wall is a metre-resolution guess at the real
    // mesh, so the shadow is taken half a metre out along the normal.
    const float nl = dot(n, kSunDir);
    if (nl > 0.0f && sun_w > 0.0f) E += kSunLux * (nl * sun_w * nf_key_visibility(p + n * 0.5f));
    // Local lights, at hits within kNfSplit only, and only those with a
    // shadow (one past the 24th lights through rock). Before the coarse
    // level and the light timer they came OUT: their bounce faded in
    // through the probes' history, and a ray that struck rock beside a
    // flame -- the inverse square at a few tens of centimetres -- lit its
    // probe tenfold for a frame, and a torch-lit tunnel pulsed 2 stops.
    // Now the distance is held to kNfLightNear, and the probes a moving
    // light reaches keep a short memory (near_field.h light_memory).
    if (lights && l_cluster.w > 0.5f) {
        const uint nlights = min(uint(l_cluster.w), nf_lights);
        [loop] for (uint i = 0u; i < nlights; ++i) {
            const LocalLightGpu Lg = g_lights[i];
            if (Lg.shadow < 0) continue;
            const float3 v = Lg.pos - p;
            const float d2 = dot(v, v);
            const float r2 = Lg.range * Lg.range;
            if (d2 >= r2) continue;
            const float3 l = v * rsqrt(max(d2, 1e-6f));
            const float ndl = dot(n, l);
            if (ndl <= 0.0f) continue;
            const float x = d2 / r2;
            const float w = saturate(1.0f - x * x);
            float3 El = Lg.intensity * (w * w / max(d2, kNfLightNear * kNfLightNear));
            if (Lg.type > 0.5f) El *= smoothstep(Lg.cos_outer, Lg.cos_inner, dot(-l, Lg.spot_dir));
            E += El * (ndl * local_shadow_taps(Lg, p + n * 0.5f, n, l, true));
        }
    }
    E += nf_hit_irradiance(p, n);
    const float3 L = kNfWallAlbedo * (E * (1.0f / kPi));
    return (any(isnan(L)) || any(isinf(L))) ? float3(0.0f, 0.0f, 0.0f) : L;
}

// The heightfield's answer for a ray that got out (its map's uv, if on
// it): its sky visibility and bounce in direction d, irradiance / pi of the
// SH.
void nf_escape(bool on_map, float2 uv, float3 d, out float vis, out float3 rad) {
    if (on_map) {
        vis = hf_irradiance(g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f), d) * (1.0f / 3.14159265f);
        rad = float3(hf_irradiance(g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f), d),
                     hf_irradiance(g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f), d),
                     hf_irradiance(g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f), d)) * (1.0f / 3.14159265f);
    } else {
        vis = saturate(0.5f + d.y);   // off the map: open air, sky above
        rad = float3(0.0f, 0.0f, 0.0f);
    }
}

// Directions from the square, y the pole: Clarberg's EQUAL-AREA octahedral
// map ("Fast Equal-Area Mapping of the (Hemi)Sphere using SIMD", 2008), so
// every bin and every stratum of one stands for the same solid angle. The
// plain octahedral map's strata run from 0.32 to 1.65 of the mean, and a
// sunlit patch landing in a heavy one made a probe jump (the stairwell's
// mouth: 202 pixels a frame against 19). uv in [0, 1]^2; only decoded --
// rays know their bin.
float3 nf_oct_dec(float2 uv) {
    const float2 p = uv * 2.0f - 1.0f;
    const float2 a = abs(p);
    const float dd = 1.0f - (a.x + a.y);
    const float r = 1.0f - abs(dd);
    const float phi = (r <= 0.0f ? 1.0f : (a.y - a.x) / r + 1.0f) * (3.14159265f / 4.0f);
    const float sxy = r * sqrt(max(2.0f - r * r, 0.0f));
    const float x = (p.x >= 0.0f ? 1.0f : -1.0f) * cos(phi) * sxy;
    const float z = (p.y >= 0.0f ? 1.0f : -1.0f) * sin(phi) * sxy;
    const float y = (dd >= 0.0f ? 1.0f : -1.0f) * (1.0f - r * r);
    return normalize(float3(x, y, z));
}

// The straight line from a to b crosses no rock, b included (nf_clear,
// hfgi.hlsli, leaves both ends out).
bool nf_clear_to(float3 a, float3 b) {
    const float3 v = b - a;
    const int n = max(int(ceil(length(v) * 2.0f)), 1);
    [loop] for (int i = 1; i <= n; ++i) {
        if (nf_occ_point(a + v * (float(i) / float(n))) > 0.5f) return false;
    }
    return true;
}

// Which of the eight coarse probes round a fine probe at o share its space:
// the line between them crosses no rock. Once a probe, by eight threads
// (cs_nf_probe), into gs_nf_cvis; the merge tested it from each ray's 6 m
// point, two 0.5 m marches of up to 14 m a coarse probe a ray, and was the
// fine pass's longest chain.
groupshared uint gs_nf_cvis;
bool nf_coarse_seen(float3 o, uint c) {
    const float3 f = (o - 4.0f) * 0.125f;
    const int3 B = int3(floor(f)) + int3(c & 1u, (c >> 1) & 1u, c >> 2);
    const int3 ob = nf_origin.xyz / 8;
    if (any(B < ob) || any(B >= ob + kNfCoarseDims)) return false;
    const float4 meta = u_coarse[nf_coarse_index(B)];
    if (meta.x < 0.5f || meta.x > 2.5f) return false;
    return nf_clear_to(o, float3(B * 8 + 4) + meta.yzw);
}

// The coarse level's radiance along d past kNfSplit, for a fine ray from o
// that got that far clear: its own bin (a fine ray is pinned to one, as a
// cascade's ray is to its four children) of the eight coarse
// probes round o, trilinearly, each by the share of its bin's rays that got
// kNfSplit clear from it (a), and only those whose own interval starts
// where this ray's does -- seen from the fine probe (nf_coarse_seen), and
// where its own 6 m point is air, that seen from this ray's. That is the cascades' parallax fix done
// as a visibility test: interpolating round o alone, a probe up on the
// surface over a tunnel's mouth answered "sunlit hillside" for a ray down
// the tunnel. False: none to take; the caller marches on itself.
bool nf_merge(float3 o, float3 d, uint bin, out float vis, out float3 rad, out float3 dbg) {
    dbg = float3(0.0f, 0.0f, 0.0f);
    vis = 0.0f;
    rad = float3(0.0f, 0.0f, 0.0f);
    const float3 f = (o - 4.0f) * 0.125f;   // coarse probe B stands at 8B + 4 (+ its offset)
    const int3 c0 = int3(floor(f));
    const float3 t = f - float3(c0);
    const int3 ob = nf_origin.xyz / 8;
    const float3 e = o + d * kNfSplit;
    float4 acc = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float wsum = 0.0f;
    [loop] for (int c = 0; c < 8; ++c) {
        const int3 off = int3(c & 1, (c >> 1) & 1, c >> 2);
        const int3 B = c0 + off;
        if (any(B < ob) || any(B >= ob + kNfCoarseDims)) continue;
        const uint ci = nf_coarse_index(B);
        const float4 meta = u_coarse[ci];
        if (meta.x < 0.5f || meta.x > 2.5f) continue;   // unplaced, or all rock
        dbg.x += 1.0f;
        const float3 tw = lerp(1.0f - t, t, float3(off));
        const float w = tw.x * tw.y * tw.z;
        const uint b4 = kNfCoarse + ci * 128u + bin * 2u;
        const float a = u_coarse[b4 + 1u].x;
        if (w * a <= 1e-5f) continue;
        dbg.y += 1.0f;
        const float3 pc = float3(B * 8 + 4) + meta.yzw;
        if ((gs_nf_cvis & (1u << uint(c))) == 0u) continue;   // not seen from the probe (nf_coarse_seen)
        // Where the probe's own interval starts in air, the ray's must see
        // it too: a probe up over a tunnel's mouth sees the hillside along
        // a ray that runs down the tunnel. Where it starts in rock (a probe
        // by the wall, a ray at an angle) the bin's other rays speak for it.
        // Only an OPEN coarse probe (meta.x 1) is tested: its bins are the
        // heightfield's daylight in every direction, the sunlit hillside
        // that failure took. A covered one traced its own rays underground.
        // Nested ifs: HLSL's && does not short-circuit.
        const float3 s = pc + d * kNfSplit;
        if (meta.x < 1.5f) {
            if (nf_inside(s)) {
                if (nf_occ_point(s) < 0.5f) {
                    if (!nf_clear_to(e, s)) continue;
                }
            }
        }
        dbg.z += 1.0f;
        acc += w * u_coarse[b4];
        wsum += w * a;
    }
    if (wsum <= 1e-4f) return false;
    vis = acc.w / wsum;
    rad = acc.rgb / wsum;
    return true;
}

// One ray from o along d, marched through the occupancy at its finest
// level -- a cone's coarse levels average a 6 m roof into a half-open one,
// and the sealed chamber of --leak-test lit to 18 nits through it. The
// coarse mip only SKIPS: where a 4 m cell holds no rock at all the ray
// strides to its far side. A ray that rises above the heightfield's top
// surface, or leaves the volume, carries back the heightfield's sky
// visibility and bounce in its direction (nf_escape); one that meets rock
// carries what the wall sends back (nf_hit).
//
// `fine`: the fine level -- past kNfSplit clear it takes the coarse level
// (nf_merge) and stops, or marches on to 48 m where there is none, its sun
// faded with distance. Otherwise the coarse level's ray: `a` is 0 where it
// meets rock before kNfSplit -- its interval starts in rock, and it has
// nothing to say -- and its hits are lit by the sun in full, no lights.
void nf_march(float3 o, float3 d, uint bin, bool fine, bool merge, out float a, out float vis, out float3 rad) {
    a = 1.0f;
    vis = 0.0f;
    rad = float3(0.0f, 0.0f, 0.0f);
    float t = 0.3f, t_air = 0.0f;
    bool tried = !fine || !merge;
    uint outcome = 0u;   // 0 ran out (dark), 1 got out, 2 met rock
    float2 uv_out = 0.0f;
    bool on_out = false;
    [loop] for (int i = 0; i < 128; ++i) {
        const float3 q = o + d * t;
        float2 uv;
        float top;
        const bool on_map = nf_hf_uv(q.xz, uv, top);
        const bool above = on_map && q.y > top + 1.0f;
        if (!nf_inside(q) || above) {
            // Out of the volume under the top surface: what lies beyond is
            // not here to march, and taking the heightfield's sky for it lit
            // a sealed cave longer than the volume with daylight. The air it
            // left by says whether there is any way out (sky_access.h).
            if (!above && nf_sealed_at(o + d * t_air)) break;   // outcome 0: dark
            outcome = 1u;
            uv_out = uv;
            on_out = on_map;
            break;
        }
        const float3 uvw = (q + 0.5f) / float3(kNfDims);
        if (g_nf_occ.SampleLevel(g_nf_wrap, uvw, 0.0f) > 0.5f) {
            // Rock: close in on the surface from the last point in air.
            float ta = t_air, tb = t;
            [unroll] for (int k = 0; k < 3; ++k) {
                const float tm = 0.5f * (ta + tb);
                if (nf_occ_lin(o + d * tm) > 0.5f) tb = tm;
                else ta = tm;
            }
            t_air = ta;
            outcome = 2u;
            break;
        }
        t_air = t;
        if (!tried && t >= kNfSplit) {
            tried = true;
            if (nf_dbg == 3u) continue;
            float3 dbg;
            const bool merged = nf_merge(o, d, bin, vis, rad, dbg);
            if (nf_dbg == 2u) { rad = dbg * 12.0f; return; }
            if (merged) {
                if (nf_dbg == 1u) rad = float3(50.0f, 0.0f, 0.0f);
                return;
            }
            if (nf_dbg == 1u) { rad = float3(0.0f, 50.0f, 0.0f); return; }
        }
        // Empty 4 m cell (mip 2 is its mean; zero is no rock at all): stride.
        // (Stepping to the exit of empty 2 m and 4 m cells instead, as a DDA,
        // was tried: down a 4 m tunnel hardly a 2 m cell is all air, and the
        // extra mip reads made the coarse pass slower, 0.038 -> 0.046 ms.)
        const float coarse = g_nf_occ.SampleLevel(g_nf_wrap, uvw, 2.0f);
        t += coarse <= 0.0f ? 2.0f : 0.5f;
        if (t > (fine || nf_live.w <= 0.0f ? 48.0f : nf_live.w)) break;
    }
    if (outcome == 1u) {
        nf_escape(on_out, uv_out, d, vis, rad);
    } else if (outcome == 2u) {
        if (!fine && t_air < kNfSplit) { a = 0.0f; return; }
        const bool near_hit = t_air < kNfSplit;
        const float sun_w = (fine && near_hit) ? 1.0f
                          : fine ? 1.0f - smoothstep(kNfSunNear, kNfSunFar, t_air)
                                 : 1.0f - smoothstep(kNfSplit, kNfCoarseSunFar, t_air);
        rad = nf_hit(o + d * t_air, d, sun_w, fine && near_hit);
    }
}

// One GROUP a probe and one THREAD a ray. A probe a thread walked its 64
// rays one after another -- up to 96 steps each, and a lit hit gathering 8
// neighbouring probes -- and the pass lasted as long as the slowest probe's
// serial chain: 0.8 ms at 1440p whether it traced 4,608 probes (play) or
// all 131k (a still), and twice that of 32 rays. A thread a ray makes the
// chain one ray long; the group sums the rays' SH in group memory.
// Dispatched (kNfProbeRowGroups, rows): the list is longer than one
// dimension's 65,535 groups in a still.
static const uint kNfProbeRowGroups = 256u;
// A hash for the rays' jitter.
uint nf_pcg(uint v) {
    const uint s = v * 747796405u + 2891336453u;
    const uint w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
groupshared float4 gs_nf_v[kNfRays];
groupshared float4 gs_nf_r[kNfRays];
groupshared float4 gs_nf_g[kNfRays];
groupshared float4 gs_nf_b[kNfRays];

// Within a fast light's reach (the carried torch, a campfire just placed):
// the probe's blend is the light's, short, so the bounce follows it.
bool nf_near_fast(float3 c) {
    bool near = false;
    [loop] for (uint i = 0u; i < nf_live2.x; ++i) {
        const float3 v = c - nf_fast[i].xyz;
        if (dot(v, v) < nf_fast[i].w) near = true;
    }
    return near;
}

// Sealed air (sky_access.h) with no light near: nothing there can send a
// probe anything. The sun and the sky cannot reach sealed air by
// definition, so only the local lights the hits are lit by (the frame's
// first nf_lights, shadowed) can -- a fine probe through its hits within
// kNfSplit and the probes round them, so a light's range + `reach`. In a
// cave world most covered air is sealed and unlit: 21,440 probes traced
// in a sealed 42 m chamber for nothing but black. A gated probe is held
// VALID and black, not switched off: off, the reader would fall back to
// the heightfield's covered branch, which can borrow light.
bool nf_gated(float3 c, float reach) {
    if (nf_gate == 0u || !nf_sealed_at(c)) return false;
    if (l_cluster.w > 0.5f) {
        const uint nlights = min(uint(l_cluster.w), nf_lights);
        [loop] for (uint i = 0u; i < nlights; ++i) {
            const LocalLightGpu Lg = g_lights[i];
            if (Lg.shadow < 0) continue;
            const float3 v = Lg.pos - c;
            const float r = Lg.range + reach;
            if (dot(v, v) < r * r) return false;
        }
    }
    return true;
}
static const float kNfGateFine = kNfSplit + 2.0f;   // a hit's reach, and the probes round it

void nf_write_dark(uint o4) {
    [unroll] for (int z = 0; z < 4; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    u_probes[o4 + 4] = float4(1.0f, 0.0f, 0.0f, 0.0f);
}

// What a probe at c needs: 1 a copy of the heightfield, 2 off, 3 traced
// (from c + off); uv its place on the heightfield's map.
//
// At or over the top surface: open air, which the heightfield already
// answers -- and a probe there only matters to a reader under the surface
// as a neighbour, which its occupancy test mostly rejects. Tracing these
// (their sideways rays crawl along the ground at 0.5 m steps) was most of
// +0.8 ms on an open-ground walk at 1440p. Traced only under real cover:
// under the 1 m top surface (a tunnel, a roof), or more than 1.5 m under
// the heightfield's 2 m map (a narrow pit, which the map's texel maximum
// spans). Open air over a slope also sits under that maximum; tracing it --
// and since the hits are lit, lighting every hit -- doubled the walking cost.
uint nf_probe_mode(float3 c, out float2 uv, out float3 off) {
    off = float3(0.0f, 0.0f, 0.0f);
    float top;
    const float ttop = g_nf_top.Load(int3(nf_wrap(int3(int(floor(c.x + 0.5f)), 0, int(floor(c.z + 0.5f))), kNfDims).xz, 0));
    if (nf_hf_uv(c.xz, uv, top) && c.y > top - 1.5f && c.y > ttop - 0.5f) return 1u;
    // In rock: move it out into the nearest air along an axis, or switch it
    // off. A probe in rock would see nothing and darken the air beside it.
    if (nf_occ_point(c) <= 0.5f) return 3u;
    static const float3 kTry[6] = {float3(0, 0.9f, 0), float3(0.9f, 0, 0), float3(-0.9f, 0, 0),
                                   float3(0, 0, 0.9f), float3(0, 0, -0.9f), float3(0, -0.9f, 0)};
    uint mode = 2u;
    [loop] for (int q = 0; q < 6 && mode == 2u; ++q) {
        if (nf_occ_point(c + kTry[q]) < 0.5f) { off = kTry[q]; mode = 3u; }
    }
    return mode;
}

// One probe. `from_live`: the live pass, its entry from the GPU's own list
// (every probe under cover, in turns); else the CPU's list -- every probe,
// in a still (play's list goes through cs_nf_classify).
void nf_probe_body(uint li, uint k, bool from_live) {
    bool live;
    uint entry;
    uint interval = 1u;
    if (from_live) {
        uint start, count;
        nf_live_share(u_aux[nf_count_slot()], max(nf_live2.z, 1u), kNfLiveMax, start, count, interval);
        live = li < count;
        entry = live ? u_aux[kAuxFine + start + li] : 0u;
    } else {
        live = li < nf_probes;
        entry = live ? g_probe_list[li] : 0u;
    }
    const uint pi = entry & 0x1FFFFFFFu;
    const int3 s = int3(pi % uint(kNfProbeDims.x), (pi / uint(kNfProbeDims.x)) % uint(kNfProbeDims.y),
                        pi / uint(kNfProbeDims.x * kNfProbeDims.y));
    const int3 op = nf_origin.xyz / 2;
    const int3 p = op + nf_wrap(s - op, kNfProbeDims);
    const float3 c = float3(p * 2 + 1);
    const uint o4 = pi * uint(kNfProbeFloat4s);

    // What the probe needs, the same in every thread of its group (so the
    // barriers below stay in uniform flow): 0 nothing (past the list), 1 a
    // copy of the heightfield, 2 switched off, 3 traced (nf_probe_mode).
    uint mode = 0u;
    float2 uv = 0.0f;
    float3 off = float3(0.0f, 0.0f, 0.0f);
    if (live) mode = nf_probe_mode(c, uv, off);
    if (mode == 3u && nf_gated(c + off, kNfGateFine)) mode = 4u;   // held dark, not traced
    // A probe in sealed air traced for a light near it takes no coarse bins:
    // the coarse probes there are gated (cs_nf_live), and finding which it
    // sees cost more than its rays marching on themselves -- the torch in the
    // 4 m tunnel 0.29 ms against 0.16 with the coarse level off, the same
    // still. Uniform over the group, as mode is.
    const bool merge = !(mode == 3u && nf_gate != 0u && nf_sealed_at(c + off));
    const bool settle = nf_settle != 0xFFFFFFFFu;
    const bool fresh = from_live ? (u_aux[pi] & kNfModeFresh) != 0u : (entry & 0x80000000u) != 0u;

    // This thread's ray: octahedral bin k of 8 x 8, jittered inside it by a
    // hash of the probe and the update, weighted by the solid angle it stands
    // for, projected into L1 SH as radiance: c0 += L Y00 dw, c1 += L Y1 d dw.
    // Pinned to its bin, a ray that gets kNfSplit clear takes exactly the
    // coarse level's mean over that bin. It was a Fibonacci set turned per
    // probe and per update, which made WHICH coarse bin a ray landed in a
    // lottery: a bright bin (a sunlit mouth 25 m off) caught by one ray or
    // three, and a tunnel's meter wandered 4x over seconds.
    if (k == 0u) gs_nf_cvis = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (mode == 3u && merge && k < 8u && nf_coarse_seen(c + off, k)) InterlockedOr(gs_nf_cvis, 1u << k);
    GroupMemoryBarrierWithGroupSync();
    float4 sv = 0.0f, sr = 0.0f, sg = 0.0f, sb = 0.0f;
    if (mode == 3u) {
        // Fresh random points each update, not a low-discrepancy sequence as
        // the coarse rays have: the round robin comes back to a probe every
        // ~32 frames, and that stride walked the sequence slowly across a
        // bin -- updates in a row all missed a sunlit patch, then all caught
        // it, and the stairwell's mouth jumped on 8,467 pixels.
        const uint h0 = nf_pcg(nf_seed ^ (pi * 7919u + k * 104729u)), h1 = nf_pcg(h0 + 0x68E31DA4u);
        const float2 jit = float2(float(h0 & 0xFFFFu), float(h1 & 0xFFFFu)) / 65536.0f;
        const float3 d = nf_oct_dec((float2(float(k & 7u), float(k >> 3)) + jit) / 8.0f);
        const float dw = 4.0f * 3.14159265f / float(kNfRays);
        float a, vis;
        float3 rad;
        nf_march(c + off, d, k, true, merge, a, vis, rad);
        const float4 basis = float4(kHfY00, kHfY1 * d) * dw;
        sv = vis * basis;
        sr = rad.r * basis;
        sg = rad.g * basis;
        sb = rad.b * basis;
    }
    gs_nf_v[k] = sv;
    gs_nf_r[k] = sr;
    gs_nf_g[k] = sg;
    gs_nf_b[k] = sb;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint h = kNfRays / 2u; h > 0u; h >>= 1u) {
        if (k < h) {
            gs_nf_v[k] += gs_nf_v[k + h];
            gs_nf_r[k] += gs_nf_r[k + h];
            gs_nf_g[k] += gs_nf_g[k + h];
            gs_nf_b[k] += gs_nf_b[k + h];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (k != 0u || mode == 0u) return;
    if (mode == 4u) {
        u_aux[pi] = 3u | kNfModeGated;
        nf_write_dark(o4);
        return;
    }
    u_aux[pi] = mode;
    if (mode == 1u) {
        u_probes[o4 + 0] = g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 1] = g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 2] = g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 3] = g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 4] = float4(1.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    if (mode == 2u) {
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    // Blended into what the probe held, DDGI's way, by how often it is traced
    // (nf_keep_*, nf_live); a probe an edit dirtied starts over (`fresh`). A
    // still (or the frame a jump of the light starts over): the plain mean
    // of its updates so far, until the memory's blend is smaller.
    float keep;
    if (fresh || u_probes[o4 + 4].x < 0.5f) {
        keep = 0.0f;
    } else if (settle) {
        const float n = float(nf_settle + nf_level);
        keep = min(nf_keep_box, n / (n + 1.0f));
    } else if (from_live) {
        keep = pow(nf_near_fast(c) ? nf_live.y : nf_live.x, float(interval));
    } else {
        keep = nf_keep_box;
    }
    u_probes[o4 + 0] = lerp(gs_nf_v[0], u_probes[o4 + 0], keep);
    u_probes[o4 + 1] = lerp(gs_nf_r[0], u_probes[o4 + 1], keep);
    u_probes[o4 + 2] = lerp(gs_nf_g[0], u_probes[o4 + 2], keep);
    u_probes[o4 + 3] = lerp(gs_nf_b[0], u_probes[o4 + 3], keep);
    u_probes[o4 + 4] = float4(1.0f, off);
}

[numthreads(kNfRays, 1, 1)]
void cs_nf_probe(uint3 gid : SV_GroupID, uint k : SV_GroupIndex) {
    nf_probe_body(gid.y * kNfProbeRowGroups + gid.x, k, false);
}

// Play's CPU list (dirty probes and the round robin): a THREAD a probe, not
// a group -- placing and copying is a handful of loads, and a group of 64
// for each was most of the pass's fixed cost. A probe to trace is marked for
// the live pass (fresh, if dirty), which traces it in its turn.
[numthreads(64, 1, 1)]
void cs_nf_classify(uint3 id : SV_DispatchThreadID) {
    if (id.x >= nf_probes) return;
    const uint entry = g_probe_list[id.x];
    const uint pi = entry & 0x1FFFFFFFu;
    const int3 s = int3(pi % uint(kNfProbeDims.x), (pi / uint(kNfProbeDims.x)) % uint(kNfProbeDims.y),
                        pi / uint(kNfProbeDims.x * kNfProbeDims.y));
    const int3 op = nf_origin.xyz / 2;
    const float3 c = float3((op + nf_wrap(s - op, kNfProbeDims)) * 2 + 1);
    const uint o4 = pi * uint(kNfProbeFloat4s);
    float2 uv;
    float3 off;
    const uint mode = nf_probe_mode(c, uv, off);
    if (mode == 3u && nf_gated(c + off, kNfGateFine)) {
        u_aux[pi] = 3u | kNfModeGated;
        nf_write_dark(o4);
        return;
    }
    if (mode == 3u) {
        // A fresh one keeps its old light until its turn (a frame or two),
        // then starts over.
        u_aux[pi] = (entry & 0x80000000u) != 0u ? (3u | kNfModeFresh) : (3u | (u_aux[pi] & kNfModeFresh));
        return;
    }
    u_aux[pi] = mode;
    if (mode == 1u) {
        u_probes[o4 + 0] = g_hf_vis.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 1] = g_hf_r.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 2] = g_hf_g.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 3] = g_hf_b.SampleLevel(g_gi_linear, uv, 0.0f);
        u_probes[o4 + 4] = float4(1.0f, 0.0f, 0.0f, 0.0f);
    } else {
        [unroll] for (int z = 0; z < 5; ++z) u_probes[o4 + uint(z)] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
}

[numthreads(kNfRays, 1, 1)]
void cs_nf_probe_live(uint3 gid : SV_GroupID, uint k : SV_GroupIndex) {
    nf_probe_body(gid.y * kNfProbeRowGroups + gid.x, k, true);
}

// --- the coarse level ---------------------------------------------------------
//
// One GROUP a coarse probe, one thread a ray: 256 rays into 64 octahedral
// bins, four a bin, each jittered in its quarter of the bin, marched from
// the probe and counted from kNfSplit (nf_march). A bin keeps (a R, a G,
// a B, a vis) and a, a the share of its rays that got kNfSplit clear, both
// blended over updates: the fine rays (nf_merge) take a bin's light as the
// mean over its clear rays, so a direction the probe sees rock in does not
// darken one a fine probe sees open.
//
// Placement: the block's 512 lattice points, of those in air and under
// cover by the fine probes' own rule the one with least rock beside it,
// then nearest the block's centre; failing that, the nearest in open air, which takes the
// heightfield's answer in every bin; failing that, unplaced (all rock).
groupshared uint   gs_nc_best;
groupshared float4 gs_nc_meta;
groupshared uint   gs_nc_entry;
groupshared float  gs_nc_keep;
groupshared float4 gs_nc_c[kNfCoarseRays];
groupshared float2 gs_nc_a[kNfCoarseRays];

bool nf_covered(float3 c, out float2 uv) {
    float top;
    const float ttop = g_nf_top.Load(int3(nf_wrap(int3(int(floor(c.x + 0.5f)), 0, int(floor(c.z + 0.5f))), kNfDims).xz, 0));
    return !(nf_hf_uv(c.xz, uv, top) && c.y > top - 1.5f && c.y > ttop - 0.5f);
}

void nf_coarse_body(uint gi_in, uint k, bool from_live) {
    uint gi = gi_in, interval = 1u;
    if (from_live) {
        uint start, count;
        nf_live_share(u_aux[nf_count_slot() + 1u], max(nf_live2.w, 1u), 2048u, start, count, interval);
        gi += start;
    }
    // The live pass dispatches exactly the list's length (cs_nf_live_args).
    // The entry goes through group memory: read from a UAV, FXC takes it for
    // varying, and every barrier after a branch on it for an error (X3663).
    if (k == 0u) {
        const uint e = from_live ? u_aux[kAuxCoarse + gi] : g_coarse_list[gi];
        gs_nc_entry = e;
        gs_nc_best = 0xFFFFFFFFu;
        gs_nc_meta = u_coarse[e & 0xFFFFu];
    }
    GroupMemoryBarrierWithGroupSync();
    const uint entry = gs_nc_entry;
    const uint ci = entry & 0xFFFFu;
    const int3 s = int3(ci % uint(kNfCoarseDims.x), (ci / uint(kNfCoarseDims.x)) % uint(kNfCoarseDims.y),
                        ci / uint(kNfCoarseDims.x * kNfCoarseDims.y));
    const int3 ob = nf_origin.xyz / 8;
    const int3 base = (ob + nf_wrap(s - ob, kNfCoarseDims)) * 8;
    // Placed already (meta.x: 1 open, 2 covered, 3 all rock), until the
    // block is filled again (cs_nf_fill zeroes it): most blocks are solid
    // rock, and searching 512 points each lap was half the pass.
    const float4 m0 = gs_nc_meta;
    const bool known = m0.x > 0.5f && (entry & 0x80000000u) == 0u;
    // In play a covered probe is the live pass's; the round robin places
    // the unplaced and refreshes the open ones.
    const bool settle = nf_settle != 0xFFFFFFFFu;
    if (!from_live && !settle && known && m0.x > 1.5f && m0.x < 2.5f) return;   // the whole group
    if (!known) {
        [unroll] for (uint j = 0u; j < 2u; ++j) {
            const uint idx = k * 2u + j;
            const int3 o = int3(idx & 7u, (idx >> 3) & 7u, idx >> 6);
            const float3 wp = float3(base + o);
            if (nf_occ_point(wp) >= 0.5f) continue;
            // Rock beside it: a 4 m tunnel has few points with air all round.
            uint rock = 0u;
            rock += nf_occ_point(wp + float3(1, 0, 0)) >= 0.5f ? 1u : 0u;
            rock += nf_occ_point(wp - float3(1, 0, 0)) >= 0.5f ? 1u : 0u;
            rock += nf_occ_point(wp + float3(0, 1, 0)) >= 0.5f ? 1u : 0u;
            rock += nf_occ_point(wp - float3(0, 1, 0)) >= 0.5f ? 1u : 0u;
            rock += nf_occ_point(wp + float3(0, 0, 1)) >= 0.5f ? 1u : 0u;
            rock += nf_occ_point(wp - float3(0, 0, 1)) >= 0.5f ? 1u : 0u;
            float2 uv;
            const bool cov = nf_covered(wp, uv);
            const int3 dc = o - 4;
            const uint score = ((cov ? 0u : 1u) << 24) | (rock << 20) | (uint(dot(dc, dc)) << 9) | idx;
            InterlockedMin(gs_nc_best, score);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint mode;   // 1 open, 2 covered, 3 none (all rock)
    float3 off;
    if (known) {
        mode = uint(m0.x + 0.5f);
        off = m0.yzw;
    } else {
        const uint best = gs_nc_best;
        mode = best == 0xFFFFFFFFu ? 3u : ((best >> 24) == 0u ? 2u : 1u);
        const uint bi = best & 511u;
        off = float3(int3(bi & 7u, (bi >> 3) & 7u, bi >> 6) - 4);
    }
    const float3 pc = float3(base + 4) + off;
    if (k == 0u) {
        // Placed just now: start over.
        // A still: the plain mean of its updates so far, until the memory's
        // blend is the smaller (a still is too short for it to converge).
        float keep = from_live ? pow(nf_live.z, float(interval)) : nf_keep_coarse;
        if (settle) {
            const float n = float(nf_settle + nf_level);
            keep = min(keep, n / (n + 1.0f));
        }
        gs_nc_keep = known ? keep : 0.0f;
        if (!known) u_coarse[ci] = float4(float(mode), off);
    }
    if (mode == 3u) return;   // uniform: every thread read the same meta and best
    GroupMemoryBarrierWithGroupSync();

    // This thread's ray: bin k / 4, its quarter k % 4, jittered in it.
    const uint bin = k >> 2, q = k & 3u;
    // Jittered by a low-discrepancy sequence over the probe's updates (R2,
    // from a hashed start per ray): the bin's history covers its quarter
    // evenly, where fresh random points clumped and left a bright patch's
    // share to chance. The sequence steps once an UPDATE -- each frame in
    // play (the live pass traces every covered probe every frame), each
    // iteration in a still. It stepped once a lap of the round robin, and
    // once the live pass traced every frame, a probe re-traced the same 256
    // directions 16 frames running and blended them as new: the unlit
    // tunnel's meter came in bursts, 3x the noise.
    const uint seq = settle ? nf_settle + nf_level : nf_live2.y;
    const uint h0 = nf_pcg(ci * 9781u + k * 6271u + 1u), h1 = nf_pcg(h0 + 0x9E3779B9u);
    const float2 jit = frac(float2(float(h0 & 0xFFFFu), float(h1 & 0xFFFFu)) / 65536.0f +
                            float(seq) * float2(0.75487766f, 0.56984029f));
    const float2 cell = float2(float((bin & 7u) * 2u + (q & 1u)), float((bin >> 3) * 2u + (q >> 1)));
    const float3 d = nf_oct_dec((cell + jit) / 16.0f);
    const float dw = 1.0f;   // equal-area strata
    float a = 1.0f, vis = 0.0f;
    float3 rad = float3(0.0f, 0.0f, 0.0f);
    if (mode == 2u) {
        nf_march(pc, d, bin, false, false, a, vis, rad);
    } else {
        float2 uv;
        float top;
        const bool on_map = nf_hf_uv(pc.xz, uv, top);
        nf_escape(on_map, uv, d, vis, rad);
    }
    gs_nc_c[k] = float4(rad, vis) * (a * dw);
    gs_nc_a[k] = float2(a * dw, dw);
    GroupMemoryBarrierWithGroupSync();
    if (q != 0u) return;
    // The bin's means, each ray by the solid angle it stands for.
    const float2 aw = gs_nc_a[k] + gs_nc_a[k + 1u] + gs_nc_a[k + 2u] + gs_nc_a[k + 3u];
    const float4 c4 = (gs_nc_c[k] + gs_nc_c[k + 1u] + gs_nc_c[k + 2u] + gs_nc_c[k + 3u]) / aw.y;
    const float a4 = aw.x / aw.y;
    const uint b4 = kNfCoarse + ci * 128u + bin * 2u;
    const float keep = gs_nc_keep;
    u_coarse[b4] = lerp(c4, u_coarse[b4], keep);
    u_coarse[b4 + 1u] = float4(lerp(a4, u_coarse[b4 + 1u].x, keep), 0.0f, 0.0f, 0.0f);
}

[numthreads(kNfCoarseRays, 1, 1)]
void cs_nf_coarse(uint3 gid : SV_GroupID, uint k : SV_GroupIndex) {
    nf_coarse_body(gid.x, k, false);
}

[numthreads(kNfCoarseRays, 1, 1)]
void cs_nf_coarse_live(uint3 gid : SV_GroupID, uint k : SV_GroupIndex) {
    nf_coarse_body(gid.x, k, true);
}

// --- the live lists -----------------------------------------------------------
//
// Of the 131k probes only those under cover trace (mode 3): a few hundred in
// a dug tunnel, the rest copy the heightfield or sit in rock. A round robin
// over all of them came back to each every ~32 frames, and a jump of the
// clock took ~10 s to leave the tunnels (1.5 s of memory a lap, the camera
// box's 0.35 s, the coarse level's 1 s, each bouncing off the others).
// Every probe every frame was 0.45 ms. So: the round robin only places and
// copies, and the probes it found tracing are listed here, on the GPU, and
// traced every frame by an indirect dispatch.
[numthreads(256, 1, 1)]
void cs_nf_live(uint3 id : SV_DispatchThreadID) {
    const uint i = id.x;
    uint slot;
    if (i < kNfProbes) {
        const uint a = u_aux[i];
        if ((a & 3u) == 3u) {
            // The gate, every frame both ways: a light carried in wakes the
            // probes it reaches this frame (fresh: they start over from
            // black), and one carried away leaves them dark.
            const int3 s = int3(i % uint(kNfProbeDims.x), (i / uint(kNfProbeDims.x)) % uint(kNfProbeDims.y),
                                i / uint(kNfProbeDims.x * kNfProbeDims.y));
            const int3 op = nf_origin.xyz / 2;
            const float3 c = float3((op + nf_wrap(s - op, kNfProbeDims)) * 2 + 1);
            const uint o4 = i * uint(kNfProbeFloat4s);
            const float3 off = (a & kNfModeGated) != 0u ? float3(0.0f, 0.0f, 0.0f) : u_probes[o4 + 4].yzw;
            if (nf_gated(c + off, kNfGateFine)) {
                if ((a & kNfModeGated) == 0u) {
                    u_aux[i] = 3u | kNfModeGated;
                    nf_write_dark(o4);
                }
                return;
            }
            if ((a & kNfModeGated) != 0u) u_aux[i] = 3u | kNfModeFresh;
            InterlockedAdd(u_aux[nf_count_slot()], 1u, slot);
            u_aux[kAuxFine + slot] = i;
        }
    } else if (i < kNfProbes + kNfCoarse) {
        const uint ci = i - kNfProbes;
        const float4 meta = u_coarse[ci];
        const float m = meta.x;
        bool gated = false;
        if (m > 1.5f && m < 2.5f) {
            const int3 s = int3(ci % uint(kNfCoarseDims.x), (ci / uint(kNfCoarseDims.x)) % uint(kNfCoarseDims.y),
                                ci / uint(kNfCoarseDims.x * kNfCoarseDims.y));
            const int3 ob = nf_origin.xyz / 8;
            // A coarse probe in sealed air is gated with lights or without:
            // its hits see no local light, and the fine probes round a light
            // carry its bounce. A/B with the coarse level off: a torch in the
            // sealed 42 m chamber and in the 4 m tunnel under spawn drew the
            // same still to 1 level in 255, for 0.36 and 0.16 ms of coarse.
            gated = nf_gate != 0u && nf_sealed_at(float3((ob + nf_wrap(s - ob, kNfCoarseDims)) * 8 + 4) + meta.yzw);
        }
        if (m > 1.5f && m < 2.5f && !gated) {   // placed under cover, and not gated
            InterlockedAdd(u_aux[nf_count_slot() + 1u], 1u, slot);
            u_aux[kAuxCoarse + slot] = ci;
        }
    }
}

[numthreads(1, 1, 1)]
void cs_nf_live_args() {
    uint start, n, interval;
    // Next frame's counts start from nothing.
    const uint next = kAuxCounts + 2u * ((nf_live2.y + 1u) & 1u);
    u_aux[next] = 0u;
    u_aux[next + 1u] = 0u;
    nf_live_share(u_aux[nf_count_slot()], max(nf_live2.z, 1u), kNfLiveMax, start, n, interval);
    u_args[0] = n == 0u ? 0u : min(n, kNfProbeRowGroups);   // a group a probe, in rows (cs_nf_probe)
    u_args[1] = (n + kNfProbeRowGroups - 1u) / kNfProbeRowGroups;
    u_args[2] = 1u;
    nf_live_share(u_aux[nf_count_slot() + 1u], max(nf_live2.w, 1u), 2048u, start, n, interval);
    u_args[3] = n;
    u_args[4] = 1u;
    u_args[5] = 1u;
}

// A jump of the light: every probe's history goes, valid flags included, so
// no probe reads a neighbour's daylight while the frame settles.
[numthreads(256, 1, 1)]
void cs_nf_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < kNfProbes * uint(kNfProbeFloat4s)) u_probes[id.x] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
