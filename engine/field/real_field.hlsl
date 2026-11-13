// ---------------------------------------------------------------------------
// M1: the caller's side of the field function, for the GPU.
//
// ADR-001 draws the line here. The field function itself is shared source and
// arrives through the include below unmodified; WHICH cells, river segments
// and entrance shafts are near a point is a spatial-index question, and an
// index is a vector of vectors on the CPU and a prefix-sum offset array plus a
// flat item array here. The same job worldgen/world.cpp does, against the same
// buffers, written twice because the two machines index differently.
//
// Everything in this file is HLSL only and is NOT part of the dialect.
// ---------------------------------------------------------------------------
#ifndef ENGINE_REAL_FIELD_HLSL
#define ENGINE_REAL_FIELD_HLSL

#include "worldgen/shaders/field.h"

cbuffer WorldConstants : register(b1) {
    float w_radius;
    float w_continent_freq;
    int   w_continent_octaves;
    float w_centre_floor;

    float w_edge_drop;
    float w_sea_level;
    float w_land_offset;
    float w_danger_w;

    float w_danger_freq;
    int   w_danger_octaves;
    float w_warp_amp;
    float w_warp_freq;

    float w_continent_scale;
    float w_biome_freq;
    float w_blend_width;
    float w_overhang_freq;

    float w_overhang_scale;
    float w_cave_freq;
    float w_cave_carve;
    float w_entrance_falloff;

    uint  w_s_cont, w_s_dang, w_s_wx, w_s_wz;
    uint  w_s_height, w_s_overhang, w_s_cave, w_s_entrance;

    int   w_cell_buckets;
    float w_cell_bucket_size;
    int   w_river_grid;
    float w_river_bin_size;

    int   w_entrance_grid;
    float w_entrance_bin_size;
    int   w_cell_count;
    int   w_biome_count;
};

RWStructuredBuffer<float4> g_cells        : register(u11);   // xy position, z biome
RWStructuredBuffer<uint>   g_cell_off     : register(u12);
RWStructuredBuffer<uint>   g_cell_item    : register(u13);
RWStructuredBuffer<float4> g_arch         : register(u14);   // 3 per biome
RWStructuredBuffer<float4> g_rivers       : register(u15);   // 2 per segment
RWStructuredBuffer<uint>   g_river_off    : register(u16);
RWStructuredBuffer<uint>   g_river_item   : register(u17);
RWStructuredBuffer<float4> g_ent          : register(u18);   // 2 per segment
RWStructuredBuffer<uint>   g_ent_off      : register(u19);
RWStructuredBuffer<uint>   g_ent_item     : register(u20);

BiomeProfile profile_of(int b) {
    BiomeProfile p;
    const float4 a0 = g_arch[b * 3 + 0];
    const float4 a1 = g_arch[b * 3 + 1];
    const float4 a2 = g_arch[b * 3 + 2];
    p.base_elevation = a0.x;
    p.amplitude = a0.y;
    p.octaves = (int)a0.z;
    p.lacunarity = a0.w;
    p.gain = a1.x;
    p.ridge_blend = a1.y;
    p.flatness = a1.z;
    p.overhang_strength = a1.w;
    p.cave_density = a2.x;
    p.cave_depth_min = a2.y;
    p.cave_depth_max = a2.z;
    return p;
}

BiomeProfile empty_profile() {
    BiomeProfile p;
    p.base_elevation = 0.0f;
    p.amplitude = 0.0f;
    p.octaves = 0;
    p.lacunarity = 2.0f;
    p.gain = 0.5f;
    p.ridge_blend = 0.0f;
    p.flatness = 1.0f;
    p.overhang_strength = 0.0f;
    p.cave_density = 0.0f;
    p.cave_depth_min = 0.0f;
    p.cave_depth_max = 0.0f;
    return p;
}

// The four nearest cells, from the bucket grid. A 5x5 neighbourhood spans
// several cell spacings at about two cells per bucket, which is why the CPU
// version only falls back to a ring search when this finds nothing.
int cell_nearest4(float x, float z, out int4 out_idx, out float4 out_dist) {
    float bd0 = 3.0e38f, bd1 = 3.0e38f, bd2 = 3.0e38f, bd3 = 3.0e38f;
    int   bi0 = -1, bi1 = -1, bi2 = -1, bi3 = -1;

    const int B = w_cell_buckets;
    const int bx = clamp((int)((x + w_radius) / w_cell_bucket_size), 0, B - 1);
    const int bz = clamp((int)((z + w_radius) / w_cell_bucket_size), 0, B - 1);

    for (int j = bz - 2; j <= bz + 2; ++j) {
        if (j < 0 || j >= B) continue;
        for (int i = bx - 2; i <= bx + 2; ++i) {
            if (i < 0 || i >= B) continue;
            const uint bucket = (uint)(j * B + i);
            const uint o0 = g_cell_off[bucket];
            const uint o1 = g_cell_off[bucket + 1u];
            for (uint m = o0; m < o1; ++m) {
                const int ci = (int)g_cell_item[m];
                const float2 p = g_cells[ci].xy;
                const float dx = p.x - x, dz = p.y - z;
                const float d2 = dx * dx + dz * dz;
                // Insertion into a sorted four, written out rather than
                // looped: HLSL cannot index a local array dynamically without
                // spilling it to scratch, which costs more than four compares.
                if (d2 < bd0) {
                    bd3 = bd2; bi3 = bi2; bd2 = bd1; bi2 = bi1;
                    bd1 = bd0; bi1 = bi0; bd0 = d2; bi0 = ci;
                } else if (d2 < bd1) {
                    bd3 = bd2; bi3 = bi2; bd2 = bd1; bi2 = bi1;
                    bd1 = d2; bi1 = ci;
                } else if (d2 < bd2) {
                    bd3 = bd2; bi3 = bi2; bd2 = d2; bi2 = ci;
                } else if (d2 < bd3) {
                    bd3 = d2; bi3 = ci;
                }
            }
        }
    }

    out_idx = int4(bi0, bi1, bi2, bi3);
    out_dist = float4(sqrt(bd0), sqrt(bd1), sqrt(bd2), sqrt(bd3));
    int found = 0;
    if (bi0 >= 0) ++found;
    if (bi1 >= 0) ++found;
    if (bi2 >= 0) ++found;
    if (bi3 >= 0) ++found;
    return found;
}

// Ground height before the drainage is carved.
float real_terrain_base(float x, float z, float r, float voxel) {
    const float continent = layer0_height(x, z, r, w_continent_freq, w_continent_octaves,
                                          w_centre_floor, w_edge_drop, w_land_offset,
                                          w_s_cont, voxel);

    // The boundary warp is what makes cell edges read as coastlines.
    const float wx = x + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wx);
    const float wz = z + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wz);

    int4 idx; float4 dist;
    const int found = cell_nearest4(wx, wz, idx, dist);
    if (found == 0) return layer4_height(continent, w_continent_scale, 0.0f, w_sea_level);

    float h[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float wt[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int ids[4] = {idx.x, idx.y, idx.z, idx.w};
    const float ds[4] = {dist.x, dist.y, dist.z, dist.w};

    [unroll] for (int k = 0; k < 4; ++k) {
        if (k >= found || ids[k] < 0) continue;
        const int b = (int)g_cells[ids[k]].z;
        if (b < 0) continue;
        const float t = 1.0f - (ds[k] - ds[0]) / w_blend_width;
        if (t <= 0.0f) continue;
        wt[k] = t * t;
        // Heights are blended across the boundary, never noise parameters:
        // interpolating octave counts produces mush exactly where the player
        // is looking.
        h[k] = layer4_profile(x, z, w_biome_freq, profile_of(b), w_s_height, voxel);
    }

    const float blended = layer4_blend(h[0], wt[0], h[1], wt[1], h[2], wt[2], h[3], wt[3]);
    return layer4_height(continent, w_continent_scale, blended, w_sea_level);
}

// Distance to the nearest river, with that channel's shape.
float real_nearest_river(float x, float z, out float out_w, out float out_d) {
    out_w = 0.0f;
    out_d = 0.0f;
    const int N = w_river_grid;
    if (N <= 0) return 3.0e38f;
    const int gx = clamp((int)((x + w_radius) / w_river_bin_size), 0, N - 1);
    const int gz = clamp((int)((z + w_radius) / w_river_bin_size), 0, N - 1);

    float best = 3.0e38f;
    for (int j = gz - 1; j <= gz + 1; ++j) {
        if (j < 0 || j >= N) continue;
        for (int i = gx - 1; i <= gx + 1; ++i) {
            if (i < 0 || i >= N) continue;
            const uint bin = (uint)(j * N + i);
            const uint o0 = g_river_off[bin], o1 = g_river_off[bin + 1u];
            for (uint m = o0; m < o1; ++m) {
                const uint s = g_river_item[m];
                const float4 a = g_rivers[s * 2u + 0u];
                const float4 b = g_rivers[s * 2u + 1u];
                const float d = river_segment_distance(x, z, a.x, a.y, a.z, a.w);
                if (d < best) { best = d; out_w = b.x; out_d = b.y; }
            }
        }
    }
    return best;
}

float real_terrain_height(float x, float z, float r, float voxel) {
    const float h = real_terrain_base(x, z, r, voxel);
    float rw, rd;
    const float dist = real_nearest_river(x, z, rw, rd);
    if (dist >= rw) return h;
    return river_carve(h, dist, rw, rd);
}

// Signed distance to the nearest entrance shaft wall.
float real_nearest_entrance(float x, float y, float z) {
    const int N = w_entrance_grid;
    if (N <= 0) return 3.0e38f;
    const int gx = clamp((int)((x + w_radius) / w_entrance_bin_size), 0, N - 1);
    const int gz = clamp((int)((z + w_radius) / w_entrance_bin_size), 0, N - 1);

    float best = 3.0e38f;
    for (int j = gz - 1; j <= gz + 1; ++j) {
        if (j < 0 || j >= N) continue;
        for (int i = gx - 1; i <= gx + 1; ++i) {
            if (i < 0 || i >= N) continue;
            const uint bin = (uint)(j * N + i);
            const uint o0 = g_ent_off[bin], o1 = g_ent_off[bin + 1u];
            for (uint m = o0; m < o1; ++m) {
                const uint s = g_ent_item[m];
                const float4 a = g_ent[s * 2u + 0u];
                const float4 b = g_ent[s * 2u + 1u];
                const float sd = entrance_tube_sd(x, y, z, a.x, a.y, a.z, a.w,
                                                  b.x, b.y, b.z, b.w);
                if (sd < best) best = sd;
            }
        }
    }
    return best;
}

// Layer 6 and 7 over the blended biome parameters. Blending these is safe
// where blending octave counts would not be: they are strength multipliers on
// a shared noise field, not parameters that change what the noise looks like.
float real_terrain_density(float x, float y, float z, float voxel) {
    const float r = sqrt(x * x + z * z) / w_radius;
    const float h = real_terrain_height(x, z, r, voxel);

    const float wx = x + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wx);
    const float wz = z + w_warp_amp * fbm(x * w_warp_freq, z * w_warp_freq, 2, 2.0f, 0.5f, w_s_wz);
    int4 idx; float4 dist;
    const int found = cell_nearest4(wx, wz, idx, dist);

    BiomeProfile p = empty_profile();
    const int ids[4] = {idx.x, idx.y, idx.z, idx.w};
    const float ds[4] = {dist.x, dist.y, dist.z, dist.w};
    float wsum = 0.0f;
    [unroll] for (int k = 0; k < 4; ++k) {
        if (k >= found || ids[k] < 0) continue;
        const int b = (int)g_cells[ids[k]].z;
        if (b < 0) continue;
        const float t = 1.0f - (ds[k] - ds[0]) / w_blend_width;
        if (t <= 0.0f) continue;
        const float wt = t * t;
        const BiomeProfile q = profile_of(b);
        p.overhang_strength += wt * q.overhang_strength;
        p.cave_density      += wt * q.cave_density;
        p.cave_depth_min    += wt * q.cave_depth_min;
        p.cave_depth_max    += wt * q.cave_depth_max;
        wsum += wt;
    }
    if (wsum > 0.0f) {
        const float inv = 1.0f / wsum;
        p.overhang_strength *= inv;
        p.cave_density *= inv;
        p.cave_depth_min *= inv;
        p.cave_depth_max *= inv;
    }

    float d = layer6_density(x, y, z, h, p, w_overhang_freq, w_overhang_scale,
                             w_cave_freq, w_cave_carve, w_s_overhang, w_s_cave, voxel);

    // Layer 7 last and outside the depth mask: the shaft is the one thing
    // allowed to reach daylight.
    const float sd = real_nearest_entrance(x, y, z);
    if (sd < 0.0f) d = entrance_carve(d, sd, w_entrance_falloff, w_cave_carve);
    return d;
}

#endif  // ENGINE_REAL_FIELD_HLSL
