// ---------------------------------------------------------------------------
// Ground cover: grass blades, clump cards, bushes and flowers, generated on
// the GPU from the seed every frame and never stored.
//
//   cs_bake  -- once per 16 m tile as it comes into range: for each 1 m cell,
//               the real surface height and slope, the blended per-biome
//               ground cover (archetypes.json "ground"), the grass patch, and
//               how much tree canopy shades it. Cached in a pool of tiles.
//   cs_emit  -- every frame, over the cached cells near the camera: blades
//               (thinning with distance, wider as they thin so cover holds),
//               clump cards further out, bushes, flowers on some blade tips.
//   cs_args  -- the counts into four indirect draws.
//
// Presentation only, on the far side of the golden hash: nothing here is
// placement anyone can collide with, fell or save. Layer 8's tree patch noise
// is evaluated here (it is dual-compile) only to thin grass under canopy.
// ---------------------------------------------------------------------------
#include "engine/field/real_field.hlsl"
#include "engine/render/shaders/shadow.hlsli"
#include "engine/render/shaders/leaf.hlsli"
#include "engine/render/shaders/scene_out.hlsli"
#include "engine/render/shaders/local_lights.hlsli"
#include "engine/render/shaders/flash.hlsli"
#include "engine/render/shaders/wind.hlsli"

cbuffer FoliageFrame : register(b0) {
    row_major float4x4 view_proj;
    float3 cam_pos;     float time;
    float4 frustum[6];
    float3 cam_right;   float blade_range;   // blades out to here
    float3 cam_up;      float card_near;     // cards from here (cross-fade band to blade_range)
    float  card_far;    float max_blades;    // blades per m^2 at grass 1, full patch
    uint   slot_count;  uint bake_count;
    uint   s_tree_patch; float tree_patch_freq;
    float  tree_patch_edge; float sea_level;
    float  wind;        float far_plane;
    uint   s_grass;     float grass_freq;
    float  near_radius; float debug;
    float  blade_width; float tuft;      // width multiplier; blades a blade instance draws, fanned from one root (1-3)
    float4 wind_v;                       // weather.h: direction x, z; mean m/s; gustiness
    float2 wind_drift;  float2 wind_pad; // metres the air has moved
};

cbuffer DrawConst : register(b3) { uint g_segments; };

struct Cell {
    float y;       // surface height
    uint  grad;    // dh/dx, dh/dz as two snorm16 (x4 range)
    uint  dens;    // grass, flowers, bushes, canopy: 8 bits each
    uint  look;    // height lo, height hi (x0.01 m), dry, narrow
    uint  color;   // grass rgb8
    uint  dryc;    // dry rgb8
};

struct Blade {
    float3 pos;
    uint   a;      // angle 8 | height 8 (cm) | width 4 | tint 4 | flower 1 | narrow 1 | scale 6
    uint   color;  // rgb8 + dry 8
    uint   dryc;   // rgb8
    uint   seed;
    uint   pad;
};

RWStructuredBuffer<Cell>  g_pool     : register(u0);
StructuredBuffer<int4>    g_slots    : register(t0);   // tile x, z, valid
StructuredBuffer<uint>    g_bake     : register(t1);   // slots to bake this frame
StructuredBuffer<float4>  g_ground   : register(t2);   // 4 per biome
RWStructuredBuffer<Blade> g_hi       : register(u1);
RWStructuredBuffer<Blade> g_lo       : register(u2);
RWStructuredBuffer<Blade> g_cards    : register(u3);
RWStructuredBuffer<Blade> g_bushes   : register(u4);
RWStructuredBuffer<uint>  g_count    : register(u5);   // 4 lists + overflow
RWStructuredBuffer<uint>  g_args     : register(u6);   // 4 x DRAW_INDEXED

static const uint kTile = 16;
static const uint kCaps[4] = {400000u, 400000u, 160000u, 40000u};
// Index ranges in the shared index buffer, set by FoliageRenderer. A blade
// instance's range holds kTuftMax blades; vs_blade collapses those past `tuft`.
static const uint kTuftMax = 3u;
static const uint kIdxFirst[4] = {0u, 81u, 108u, 120u};
static const uint kIdxCount[4] = {81u, 27u, 12u, 84u};

uint pack8(float v) { return uint(saturate(v) * 255.0f + 0.5f); }
float unpack8(uint v, uint shift) { return float((v >> shift) & 0xFFu) / 255.0f; }
uint pack_rgb(float3 c) { return pack8(c.r) | (pack8(c.g) << 8) | (pack8(c.b) << 16); }
float3 unpack_rgb(uint v) { return float3(unpack8(v, 0), unpack8(v, 8), unpack8(v, 16)); }

uint fhash(uint a, uint b) {
    uint h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}
float funit(uint h) { return float(h >> 8) * (1.0f / 16777216.0f); }

// --- bake -------------------------------------------------------------------

[numthreads(16, 16, 1)]
void cs_bake(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    if (g.x >= bake_count) return;
    const uint slot = g_bake[g.x];
    const int4 tile = g_slots[slot];
    const uint ci = t.y * kTile + t.x;
    Cell c = (Cell)0;
    c.y = -1.0e9f;

    const float x = (float(tile.x * int(kTile) + int(t.x)) + 0.5f);
    const float z = (float(tile.y * int(kTile) + int(t.y)) + 0.5f);
    const float r = sqrt(x * x + z * z) / w_radius;
    if (r > 0.98f) { g_pool[slot * 256u + ci] = c; return; }

    // The biome blend, as layer 4 and layer 8 compute it.
    const float wx = x + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wx);
    const float wz = z + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wz);
    int4 idx; float4 dist; float d5;
    const int found = cell_nearest4(wx, wz, idx, dist, d5);
    const int ids[4] = {idx.x, idx.y, idx.z, idx.w};
    const float ds[4] = {dist.x, dist.y, dist.z, dist.w};
    float wb[4] = {0, 0, 0, 0};
    int   bb[4] = {-1, -1, -1, -1};
    float wsum = 0.0f, wall = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (k >= found || ids[k] < 0) continue;
        const float wk = blend_weight(ds[k], ds[0], d5, w_blend_width);
        wall += wk;
        const int b = (int)g_cells[ids[k]].z;
        if (b < 0 || wk <= 0.0f) continue;
        bb[k] = b; wb[k] = wk; wsum += wk;
    }
    const float div = blend_divisor(wsum, wall);
    if (div <= 0.0f) { g_pool[slot * 256u + ci] = c; return; }

    float grass = 0.0f, flowers = 0.0f, bushes = 0.0f, canopy = 0.0f;
    float hlo = 0.0f, hhi = 0.0f, dry = 0.0f, narrow = 0.0f, ov = 0.0f, wl = 0.0f;
    float3 col = 0.0f, dcol = 0.0f;
    [unroll] for (int k2 = 0; k2 < 4; ++k2) {
        if (bb[k2] < 0) continue;
        const float w = wb[k2] / div;
        const float4 g0 = g_ground[bb[k2] * 4 + 0];   // grass, cover, hlo, hhi
        const float4 g1 = g_ground[bb[k2] * 4 + 1];   // colour, dry
        const float4 g2 = g_ground[bb[k2] * 4 + 2];   // dry colour, narrow
        const float4 g3 = g_ground[bb[k2] * 4 + 3];   // flowers, bushes, tree density, tree cover
        const float patch = tree_patch(x, z, grass_freq, g0.y, 0.12f, s_grass);
        grass += w * g0.x * patch;
        flowers += w * g3.x * patch;
        bushes += w * g3.y;
        canopy += w * g3.z * tree_patch(x, z, tree_patch_freq, g3.w, tree_patch_edge, s_tree_patch);
        hlo += w * g0.z; hhi += w * g0.w;
        col += w * g1.rgb; dry += w * g1.a;
        dcol += w * g2.rgb; narrow += w * g2.a;
        ov += w * profile_of(bb[k2]).overhang_strength;
        wl += w;
    }
    if (wl <= 0.0f) { g_pool[slot * 256u + ci] = c; return; }
    // Canopy from layer 8's density: 0.55 is the densest biome's, a closed wood.
    canopy = saturate(canopy / 0.45f);

    // The ground, as the trees find it: the 2.5D height, then the real
    // surface bisected inside the overhang displacement if there is one.
    // ONE call site per field function, each in a [loop]. FXC inlines the
    // whole field at every call: this kernel written out longhand -- five
    // heights and three density probes -- overflowed the 4096 temp registers
    // (X4505), the same lesson as field_normal's six calls (CLAUDE.md).
    float h = 0.0f, hx = 0.0f, hz = 0.0f;
    [loop] for (int k5 = 0; k5 < 5; ++k5) {
        const float ox = k5 == 1 ? 0.5f : (k5 == 2 ? -0.5f : 0.0f);
        const float oz = k5 == 3 ? 0.5f : (k5 == 4 ? -0.5f : 0.0f);
        const float hk = real_terrain_height(x + ox, z + oz, r, 0.0f);
        if (k5 == 0) h = hk;
        else if (k5 == 1) hx += hk; else if (k5 == 2) hx -= hk;
        else if (k5 == 3) hz += hk; else hz -= hk;
    }
    float rw, rd;
    if (h < sea_level + 0.25f || real_nearest_river(x, z, rw, rd) < rw + 0.5f) {
        g_pool[slot * 256u + ci] = c; return;
    }
    float y = h;
    const float span = ov * w_overhang_scale;
    if (span > 0.3f) {
        // Step down 1 m at a time to the first rock, bisect six times, then
        // probe 2 m over the surface for a roof -- one density call site.
        float ya = h + span + 1.0f, yr = ya;
        int phase = 0, it = 0;
        bool hit = false, roof = false;
        [loop] for (int s = 0; s < 80; ++s) {
            const float yq = phase == 0 ? yr - 1.0f : (phase == 1 ? 0.5f * (ya + yr) : yr + 2.0f);
            const bool rock = real_terrain_density(x, yq, z, 0.0f) > 0.0f;
            if (phase == 0) {
                if (rock) { yr = yq; hit = true; phase = 1; }
                else { ya = yq; yr = yq; if (yq < h - span - 1.0f) break; }
            } else if (phase == 1) {
                if (rock) yr = yq; else ya = yq;
                if (++it >= 6) phase = 2;
            } else {
                roof = rock;
                break;
            }
        }
        if (!hit) { g_pool[slot * 256u + ci] = c; return; }
        y = yr;
        // A roof overhead: nothing grows in the dark under a ledge.
        if (roof) grass = flowers = bushes = 0.0f;
    }
    const float slope = sqrt(hx * hx + hz * hz);
    const float steep = saturate((0.95f - slope) / 0.35f);
    grass *= steep; flowers *= steep; bushes *= steep;

    c.y = y;
    const int gx = clamp(int(hx * 8192.0f), -32767, 32767), gz = clamp(int(hz * 8192.0f), -32767, 32767);
    c.grad = (uint(gx) & 0xFFFFu) | ((uint(gz) & 0xFFFFu) << 16);
    c.dens = pack8(grass) | (pack8(flowers * 4.0f) << 8) | (pack8(bushes * 4.0f) << 16) | (pack8(canopy) << 24);
    c.look = pack8(hlo / 2.0f) | (pack8(hhi / 2.0f) << 8) | (pack8(dry) << 16) | (pack8(narrow) << 24);
    c.color = pack_rgb(col);
    c.dryc = pack_rgb(dcol);
    g_pool[slot * 256u + ci] = c;
}

// --- emit -------------------------------------------------------------------

[numthreads(64, 1, 1)]
void cs_clear(uint3 id : SV_DispatchThreadID) {
    if (id.x < 5u) g_count[id.x] = 0u;
}

bool sphere_visible(float3 p, float r) {
    [unroll] for (int k = 0; k < 6; ++k) if (dot(frustum[k].xyz, p) + frustum[k].w < -r) return false;
    return true;
}

void put(uint list, Blade b) {
    uint at;
    InterlockedAdd(g_count[list], 1u, at);
    if (at >= kCaps[list]) { InterlockedAdd(g_count[4], 1u, at); return; }
    if (list == 0u) g_hi[at] = b;
    else if (list == 1u) g_lo[at] = b;
    else if (list == 2u) g_cards[at] = b;
    else g_bushes[at] = b;
}

[numthreads(16, 16, 1)]
void cs_emit(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const uint slot = g.x;
    if (slot >= slot_count) return;
    const int4 tile = g_slots[slot];
    if (tile.z == 0) return;
    const uint ci = t.y * kTile + t.x;
    const Cell c = g_pool[slot * 256u + ci];
    if (c.y < -1.0e8f || c.dens == 0u) return;

    const int cx = tile.x * int(kTile) + int(t.x), cz = tile.y * int(kTile) + int(t.y);
    const float3 centre = float3(float(cx) + 0.5f, c.y, float(cz) + 0.5f);
    const float d = length(centre - cam_pos);
    if (d > card_far + 2.0f) return;
    const float hhi = unpack8(c.look, 8) * 2.0f;
    if (!sphere_visible(centre + float3(0, 0.5f * hhi, 0), 1.0f + hhi)) return;

    const float grass = unpack8(c.dens, 0);
    const float flowers = unpack8(c.dens, 8) * 0.25f;
    const float bushes = unpack8(c.dens, 16) * 0.25f;
    const float canopy = unpack8(c.dens, 24);
    const float hlo = unpack8(c.look, 0) * 2.0f;
    const float narrow = unpack8(c.look, 24);
    const float gx = float(int(c.grad << 16) >> 16) / 8192.0f;
    const float gz = float(int(c.grad) >> 16) / 8192.0f;
    const uint seed = fhash(uint(cx), uint(cz));

    // Blades. Full density inside near_radius; beyond it the count falls as
    // (near/d)^2 and each blade widens by the square root, so the ground stays
    // covered. The last blade of a cell scales down continuously rather than
    // popping as the count changes.
    // Within a patch, tufts: a 6-8 m noise moves density and height, so a
    // meadow has taller clumps and thinner ground rather than a lawn.
    const float tuft = vnoise(centre.xz * 0.14f + float2(float(seed & 7u) * 0.0f, 0.0f));
    const float g_eff = grass * (1.0f - 0.7f * canopy) * (0.55f + 0.9f * tuft);
    if (g_eff > 0.002f && d < blade_range + 1.0f) {
        const float thin = d < near_radius ? 1.0f : (near_radius / d) * (near_radius / d);
        const float full = g_eff * max_blades;
        const float want = full * thin;
        const uint n = min(uint(ceil(want)), 128u);
        const float widen = clamp(sqrt(1.0f / max(thin, 0.02f)), 1.0f, 4.0f);
        const uint list = d < 22.0f ? 0u : 1u;
        const uint flower_n = funit(fhash(seed, 77u)) < flowers * 6.0f ? 3u + (seed & 3u) : 0u;
        for (uint k = 0; k < n; ++k) {
            const uint h = fhash(seed, k + 1u);
            const float ox = funit(h), oz = funit(fhash(h, 3u));
            Blade b;
            b.pos = float3(float(cx) + ox, c.y + gx * (ox - 0.5f) + gz * (oz - 0.5f) - 0.06f, float(cz) + oz);
            const float scale = saturate(want - float(k));
            const float ht = lerp(hlo, hhi, funit(fhash(h, 5u))) * (0.75f + 0.25f * scale) * (0.6f + 0.8f * tuft);
            b.a = (fhash(h, 7u) & 0xFFu) | (min(uint(ht * 100.0f), 255u) << 8) |
                  (min(uint((widen - 1.0f) * 5.0f), 15u) << 16) | ((fhash(h, 9u) & 0xFu) << 20) |
                  ((k < flower_n ? 1u : 0u) << 24) | ((narrow > 0.5f ? 1u : 0u) << 25) |
                  (uint(scale * 63.0f) << 26);
            b.color = c.color | (c.look & 0x00FF0000u) << 8;
            b.dryc = c.dryc;
            b.seed = h;
            b.pad = 0u;
            put(list, b);
        }
    }

    // Clump cards, from card_near out: one per cell at the cell's grass
    // density, sized to the grass.
    if (d > card_near - 1.0f && d < card_far + 1.0f && funit(fhash(seed, 101u)) < g_eff * 1.15f) {
        Blade b;
        const uint h = fhash(seed, 103u);
        b.pos = float3(float(cx) + 0.5f, c.y - 0.08f, float(cz) + 0.5f);
        b.a = (h & 0xFFu) | (min(uint(hhi * 1.1f * 100.0f), 255u) << 8) | ((narrow > 0.5f ? 1u : 0u) << 25);
        b.color = c.color | (c.look & 0x00FF0000u) << 8;
        b.dryc = c.dryc;
        b.seed = h;
        b.pad = 0u;
        put(2u, b);
    }

    // Bushes: more of them under a canopy, which is where undergrowth is.
    if (d < 130.0f && funit(fhash(seed, 211u)) < bushes * (1.0f + canopy)) {
        Blade b;
        const uint h = fhash(seed, 213u);
        b.pos = float3(float(cx) + 0.3f + 0.4f * funit(h), c.y - 0.1f, float(cz) + 0.3f + 0.4f * funit(fhash(h, 1u)));
        b.a = (h & 0xFFu) | (uint(40.0f + 100.0f * funit(fhash(h, 2u))) << 8);
        b.color = c.color | (c.look & 0x00FF0000u) << 8;
        b.dryc = c.dryc;
        b.seed = h;
        b.pad = 0u;
        put(3u, b);
    }
}

[numthreads(4, 1, 1)]
void cs_args(uint3 id : SV_DispatchThreadID) {
    const uint l = id.x;
    if (l >= 4u) return;
    const uint n = min(g_count[l], kCaps[l]);
    g_args[l * 5u + 0u] = kIdxCount[l];
    g_args[l * 5u + 1u] = n;
    g_args[l * 5u + 2u] = kIdxFirst[l];
    g_args[l * 5u + 3u] = 0u;
    g_args[l * 5u + 4u] = 0u;
}

// --- drawing ----------------------------------------------------------------

StructuredBuffer<Blade> g_list : register(t4);

struct FolOut {
    float4 clip  : SV_Position;
    float3 world : TEXCOORD0;
    float3 nrm   : TEXCOORD1;
    float2 uv    : TEXCOORD2;   // x across, y root (0) to tip (1)
    nointerpolation float3 col  : TEXCOORD3;
    nointerpolation float3 dcol : TEXCOORD4;
    nointerpolation float  dry  : TEXCOORD5;
    nointerpolation uint   flags : TEXCOORD6;
    nointerpolation uint   seed : TEXCOORD7;
    float4 gi : TEXCOORD8;   // gi_diffuse per vertex (lighting.hlsli): blades overdraw too much to read the probes a pixel
};

FolOut with_gi(FolOut o) {
    o.gi = float4(gi_diffuse(o.world, normalize(o.nrm), normalize(cam_pos - o.world)), 0.0f);
    o.gi.rgb *= gi_fill(1.3f);   // the stand-in for bounce between blades, where the probes did not answer
    o.gi.a = g_gi_all;
    return o;
}

// Wind (weather.h, wind.hlsli): the tip's push as a fraction of the blade's
// height. A blade bends with the drag, the speed squared, until it is lying
// over: 0.3 in a light breeze (3 m/s), 0.7 at 6, 1.0 at 10, 1.3 in a gale.
// The gust cells drifting over the ground are what make a meadow ripple;
// the quick sway on top grows with the speed. `wind` is the settings'
// multiplier (1).
float2 wind_at(float2 p) {
    const float2 v = wind_velocity(p, wind_v, wind_drift, time);
    const float s2 = dot(v, v);
    const float bend = 1.4f * s2 / (s2 + 40.0f);
    const float2 dir = wind_v.xy;
    const float sway = sin(time * 2.3f + dot(p, float2(0.7f, 0.4f))) * min(0.02f + 0.012f * wind_v.z, 0.2f);
    return (dir * (bend + sway) + float2(-dir.y, dir.x) * (0.5f * sway)) * wind;
}

float blade_fade(float d) {
    // 1 where blades own the ground, 0 where cards do; the two draw on
    // complementary sides of one dither, as tree levels do.
    return saturate((blade_range - d) / max(blade_range - card_near, 1.0f));
}

FolOut vs_blade(uint vid_all : SV_VertexID, uint iid : SV_InstanceID) {
    Blade b = g_list[iid];
    const uint S = g_segments;
    // A TUFT: up to kTuftMax blades from one instance, each 2S + 1 vertices.
    // The first is the placed blade; the others stand a few centimetres off
    // its root at their own angle and height, leaning out, so one placement
    // reads as a clump rather than a lone stem.
    const uint per = 2u * S + 1u;
    const uint bi = vid_all / per, vid = vid_all - bi * per;
    float hmul = 1.0f, lean_out = 0.0f;
    float3 lean_dir = 0.0f;
    if (bi > 0u) {
        if (float(bi) >= tuft) {
            FolOut z = (FolOut)0;
            z.clip = float4(0.0f, 0.0f, 0.0f, 1.0f);   // collapsed: a zero-area triangle
            return z;
        }
        const uint hs = fhash(b.seed, 31u + bi);
        const float oa = funit(hs) * 6.2831853f;
        lean_dir = float3(cos(oa), 0.0f, sin(oa));
        b.pos += lean_dir * (0.025f + 0.045f * funit(fhash(hs, 1u)));
        b.a = (b.a & ~0xFFu) | (fhash(hs, 2u) & 0xFFu);
        b.seed = fhash(hs, 3u);
        hmul = 0.65f + 0.4f * funit(fhash(hs, 4u));
        lean_out = 0.12f + 0.18f * funit(fhash(hs, 5u));
    }
    const uint row = min(vid >> 1, S);
    const float side = (vid & 1u) ? 0.5f : -0.5f;
    const float tt = float(row) / float(S);
    const float ang = float(b.a & 0xFFu) / 255.0f * 6.2831853f;
    const float hgt = float((b.a >> 8) & 0xFFu) * 0.01f * hmul;
    const float widen = 1.0f + float((b.a >> 16) & 0xFu) / 5.0f;
    const bool narrow = ((b.a >> 25) & 1u) != 0u;
    const float scale = float((b.a >> 26) & 63u) / 63.0f;
    const float3 f = float3(cos(ang), 0.0f, sin(ang));
    const float3 rt = float3(-f.z, 0.0f, f.x);
    const float curl = 0.15f + 0.35f * funit(b.seed);
    const float2 w = wind_at(b.pos.xz);
    // Tip displacement as a fraction of the height; the stem shortens so the
    // blade bends rather than stretches.
    const float3 D = (f * curl + lean_dir * lean_out + float3(w.x, 0.0f, w.y)) * hgt;
    const float lean2 = dot(D, D) / max(hgt * hgt, 1e-4f);
    const float width = (narrow ? 0.009f : 0.016f) * blade_width * widen * (row == S ? 0.0f : pow(1.0f - tt, 0.6f));
    const float h = hgt * max(scale, 0.05f);
    const float3 p = b.pos + float3(0.0f, h * tt * (1.0f - 0.35f * lean2 * tt), 0.0f) + D * (tt * tt) * max(scale, 0.05f)
                   + rt * (side * width);
    const float3 tangent = normalize(float3(0.0f, h, 0.0f) + 2.0f * D * tt);
    float3 n = normalize(cross(rt, tangent));
    // Rounded across the blade, so a flat strip reads as a stem, and tilted
    // up: a sward is lit from above as a surface, and with the blade's own
    // near-horizontal normal every blade sat in its own shade -- grey hatching.
    n = normalize(n + rt * side * 0.8f + float3(0.0f, 0.9f, 0.0f));
    FolOut o;
    o.clip = mul(float4(p, 1.0f), view_proj);
    o.world = p;
    o.nrm = n;
    o.uv = float2(side + 0.5f, tt);
    const float3 col = unpack_rgb(b.color);
    // Per-blade variation: brightness, and a lean towards yellow-green.
    const float tv = float((b.a >> 20) & 0xFu) / 15.0f;
    o.col = lerp(col, col * float3(1.25f, 1.1f, 0.6f), 0.5f * funit(fhash(b.seed, 11u))) * (0.8f + 0.4f * tv);
    o.dcol = unpack_rgb(b.dryc);
    o.dry = float(b.color >> 24) / 255.0f;
    o.flags = b.a >> 24;
    o.seed = b.seed;
    return with_gi(o);
}

// `albedo` as authored (sRGB). Physical units since ADR-004 L1: the wrapped
// and transmitted sun of lighting.hlsli, and the GI interface's diffuse,
// per vertex, with the fallback's fill for light scattered between blades
// where the probes did not answer (with_gi; trees.hlsl's shade the same).

// A film of water on a wet leaf or blade (ADR-005 C4): a sheen of the sun
// and the sky off its surface, on top of the leaf's own light. Leaves have
// no specular dry -- a crown seen whole is too rough for one -- but a wet one
// glints. Cheap on purpose: leaves and blades overdraw heavily, and the
// full GGX lobe and sky lookup cost 0.55 ms walking at 1440p. A normalised
// Blinn-Phong lobe and the sky's mean radiance carry what a film shows.
void wet_leaf_sheen(inout float3 dir, inout float3 amb, float3 n, float3 v, float3 world, float sh, float wet) {
    if (wet <= 0.0f) return;
    float3 nn = n;
    if (dot(nn, v) < 0.0f) nn = -nn;   // a card's face towards the eye
    const float nv = saturate(dot(nn, v));
    const float k = 1.0f - nv;
    const float F = 0.04f + 0.96f * (k * k) * (k * k) * k;
    const float nl = dot(nn, kSunDir);
    if (nl > 0.0f && sh > 0.0f) {
        const float nh = saturate(dot(nn, normalize(kSunDir + v)));
        const float nh2 = nh * nh, nh4 = nh2 * nh2, nh8 = nh4 * nh4, nh16 = nh8 * nh8;
        dir += kSunLux * (1.35f * nh16 * nh16 * F * nl * sh * 0.35f * wet);
    }
    amb += kSkyMean * (F * 0.2f * wet * gi_sky_visibility(world, nn));
}

SceneOut shade_foliage(float4 clip, float3 albedo_srgb, float3 n, float ao, float3 world, float3 toward_eye,
                       float contact = 0.0f, float ssao = 0.4f) {
    // Rain (ADR-005 C4): wet blades a little darker, and glinting.
    const float wet = l_wet.x;
    const float3 albedo = srgb_to_linear(albedo_srgb) * lerp(1.0f, 0.82f, wet);
    const float sh = shadow_factor(world, float3(0.0f, 1.0f, 0.0f));
    float3 dir = leaf_sun_light(albedo, n, toward_eye, 0.5f, 0.7f, sh) * (1.09f * ao);
    float3 amb = albedo / kPi * gi_diffuse(world, n, toward_eye) * ao +
                 local_lights_leaf(albedo, n, toward_eye, 0.5f, 0.7f, world, clip) * ao +
                 flash_leaf(albedo, n, toward_eye, world, 0.5f, 0.7f) * ao;
    wet_leaf_sheen(dir, amb, n, toward_eye, world, sh, wet * ao);
    return scene_out(dir, amb, world, cam_pos, far_plane, contact, ssao);
}

SceneOut ps_blade(FolOut i, bool front : SV_IsFrontFace) {
    const float d = length(i.world - cam_pos);
    if (dither(i.clip.xy) >= blade_fade(d)) discard;
    float3 n = normalize(i.nrm);
    if (!front) n = -n;
    // Dry blades go straw from the tip down.
    const float dryness = saturate(i.dry + 0.35f * (funit(i.seed) - 0.5f)) * smoothstep(0.1f, 0.9f, i.uv.y + 0.3f * i.dry);
    float3 albedo = lerp(i.col, i.dcol, dryness);
    // Root to tip: the base sits in the shade of its neighbours.
    const float ao = lerp(0.5f, 1.0f, pow(i.uv.y, 0.6f));
    // Sunlit tips catch the light: a little warmer and brighter.
    albedo *= 1.0f + 0.25f * i.uv.y * i.uv.y;
    if ((i.flags & 1u) != 0u && i.uv.y > 0.82f) {
        const uint hc = i.seed % 4u;
        albedo = hc == 0u ? float3(0.85f, 0.82f, 0.75f) : hc == 1u ? float3(0.85f, 0.70f, 0.15f)
               : hc == 2u ? float3(0.55f, 0.35f, 0.70f) : float3(0.80f, 0.30f, 0.30f);
    }
    if (debug > 0.5f) return scene_flat(float3(0.9f, 0.9f, 0.1f));
    // Blades take a little: the base of a sward darkening where it meets a
    // trunk or a stone reads; blade-on-blade does not.
    g_gi_vertex = i.gi;
    return shade_foliage(i.clip, albedo, n, ao, i.world, normalize(cam_pos - i.world), 0.3f * (1.0f - i.uv.y));
}

// Two crossed quads, verts 0..7.
FolOut vs_card(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const Blade b = g_list[iid];
    const uint q = vid >> 2, k = vid & 3u;
    const float ang = float(b.a & 0xFFu) / 255.0f * 6.2831853f + float(q) * 1.5707963f;
    const float hgt = max(float((b.a >> 8) & 0xFFu) * 0.01f, 0.1f);
    const float3 ax = float3(cos(ang), 0.0f, sin(ang));
    const float wdt = 0.75f;
    const float u = (k == 1u || k == 2u) ? 1.0f : 0.0f;
    const float v = k >= 2u ? 1.0f : 0.0f;
    const float2 w = wind_at(b.pos.xz) * 0.5f;
    float3 p = b.pos + ax * ((u - 0.5f) * 2.0f * wdt) + float3(0.0f, v * hgt, 0.0f) + float3(w.x, 0.0f, w.y) * hgt * v;
    FolOut o;
    o.clip = mul(float4(p, 1.0f), view_proj);
    o.world = p;
    o.nrm = normalize(float3(-ax.z, 0.0f, ax.x) * 0.35f + float3(0.0f, 1.0f, 0.0f));
    o.uv = float2(u, v);
    o.col = unpack_rgb(b.color);
    o.dcol = unpack_rgb(b.dryc);
    o.dry = float(b.color >> 24) / 255.0f;
    o.flags = b.a >> 24;
    o.seed = b.seed;
    return with_gi(o);
}

SceneOut ps_card(FolOut i) {
    const float d = length(i.world - cam_pos);
    const float dz = dither(i.clip.xy);
    if (dz < blade_fade(d)) discard;
    if (dz >= saturate((card_far - d) / (0.15f * card_far))) discard;
    // Strands: columns of tapering blades of uneven height. Their edges soften
    // as the card shrinks, towards the strands' average coverage.
    const float cols = 14.0f;
    const float cx = i.uv.x * cols;
    const float col = floor(cx);
    const float fx = cx - col - 0.5f;
    const float hh = 0.45f + 0.55f * hash2(float2(col, float(i.seed & 1023u)));
    const float y = i.uv.y / hh;
    const float wdt = 0.42f * (1.0f - y);
    const float blur = length(fwidth(i.uv)) * cols;
    float a = y < 1.0f ? saturate((wdt - abs(fx)) / max(blur * 0.5f, 0.05f) + 0.5f) : 0.0f;
    a = lerp(a, 0.45f * saturate(1.2f - i.uv.y), saturate(blur - 0.5f));
    if (a < dither(i.clip.xy + 17.0f)) discard;
    const float dryness = saturate(i.dry) * smoothstep(0.1f, 0.9f, i.uv.y + 0.3f * i.dry);
    const float3 albedo = lerp(i.col, i.dcol, dryness);
    const float ao = lerp(0.4f, 1.0f, i.uv.y);
    if (debug > 0.5f) return scene_flat(float3(0.1f, 0.6f, 0.9f));
    g_gi_vertex = i.gi;
    return shade_foliage(i.clip, albedo, normalize(i.nrm), ao, i.world, normalize(cam_pos - i.world));
}

// A bush: 14 leaf cards round the inside of a squat ellipsoid, verts 0..55.
FolOut vs_bush(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
    const Blade b = g_list[iid];
    const uint card = vid >> 2, k = vid & 3u;
    const float rad = float((b.a >> 8) & 0xFFu) * 0.01f;
    const uint h = fhash(b.seed, card);
    // A direction over the upper hemisphere, from the card's hash.
    const float az = funit(h) * 6.2831853f;
    const float el = 0.15f + 0.85f * funit(fhash(h, 1u));
    const float3 dir = normalize(float3(cos(az) * (1.0f - el * 0.6f), el, sin(az) * (1.0f - el * 0.6f)));
    const float3 ctr = b.pos + float3(0.0f, rad * 0.55f, 0.0f) + dir * float3(rad * 0.75f, rad * 0.5f, rad * 0.75f);
    const float3 nn = normalize(dir + float3(funit(fhash(h, 2u)) - 0.5f, 0.3f, funit(fhash(h, 3u)) - 0.5f));
    float3 u = normalize(cross(nn, abs(nn.y) < 0.9f ? float3(0, 1, 0) : float3(1, 0, 0)));
    const float rot = funit(fhash(h, 4u)) * 6.2831853f;
    u = u * cos(rot) + cross(nn, u) * sin(rot);
    const float3 v = cross(nn, u);
    const float s = rad * (0.55f + 0.3f * funit(fhash(h, 5u)));
    const float cu = (k == 1u || k == 2u) ? 1.0f : 0.0f;
    const float cv = k >= 2u ? 1.0f : 0.0f;
    const float2 w = wind_at(b.pos.xz) * 0.08f * el;
    const float3 p = ctr + u * ((cu - 0.5f) * 2.0f * s) + v * ((cv - 0.5f) * 2.0f * s) + float3(w.x, 0.0f, w.y);
    FolOut o;
    o.clip = mul(float4(p, 1.0f), view_proj);
    o.world = p;
    o.nrm = normalize(dir + float3(0, 0.3f, 0));
    o.uv = float2(cu, 1.0f - cv);
    // Bushes a deeper green than the grass round them.
    o.col = unpack_rgb(b.color) * float3(0.70f, 0.82f, 0.72f);
    o.dcol = unpack_rgb(b.dryc) * 0.8f;
    o.dry = float(b.color >> 24) / 255.0f * 0.5f;
    o.flags = 0u;
    o.seed = h;
    return with_gi(o);
}

SceneOut ps_bush(FolOut i) {
    const float blur = length(fwidth(i.uv));
    const float a = leaf_coverage(i.uv, 1.0f, funit(i.seed), blur, 0.0f);
    const float thr = lerp(0.5f, dither(i.clip.xy), saturate(blur * 6.0f));
    if (a < thr) discard;
    const float3 albedo = lerp(i.col, i.dcol, i.dry) * (0.85f + 0.3f * funit(fhash(i.seed, 9u)));
    const float ao = lerp(0.45f, 1.0f, saturate(i.nrm.y));
    if (debug > 0.5f) return scene_flat(float3(0.9f, 0.2f, 0.9f));
    g_gi_vertex = i.gi;
    return shade_foliage(i.clip, albedo, normalize(i.nrm), ao, i.world, normalize(cam_pos - i.world));
}
