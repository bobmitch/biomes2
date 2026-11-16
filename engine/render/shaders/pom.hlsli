#ifndef POM_HLSLI
#define POM_HLSLI
// ---------------------------------------------------------------------------
// Parallax occlusion mapping over the CC0 scans' height (scan_library.h:
// layer A's alpha, 1 high). Shared by the terrain (mesh.hlsl) and the bark
// (trees.hlsl), which both declare g_scan_a and g_scan_samp before including
// this.
//
// Depth runs 0 at the top of the height map to 1 at its floor, in units of
// the set's `depth_m` (assets/textures/scans.json). A caller turns the view
// and the key light into uv shifts per unit of that depth in its own frame
// -- the dominant triplanar plane for terrain, the trunk's derivative frame
// for bark -- and these do the rest. No silhouettes and no depth write: the
// surface's outline stays the mesh's.
// ---------------------------------------------------------------------------

float pom_depth(float3 uvl, float2 gx, float2 gy) {
    return 1.0f - g_scan_a.SampleGrad(g_scan_samp, uvl, gx, gy).a;
}

// The depth fraction at which the view ray meets the height map: march in
// equal depth layers until the ray is below the map, then refine linearly
// between the last two. `view_step` is the uv shift over the full depth;
// the gradients are the unshifted uv's, so the loop's samples keep its mip.
float pom_march(float2 uv0, float layer, float2 view_step, float2 gx, float2 gy, int steps) {
    const float dt = 1.0f / float(steps);
    float t = 0.0f, prev_t = 0.0f;
    float d = pom_depth(float3(uv0, layer), gx, gy), prev_d = d;
    [loop] for (int s = 0; s < 32; ++s) {
        if (s >= steps || t >= d) break;
        prev_t = t;
        prev_d = d;
        t += dt;
        d = pom_depth(float3(uv0 + view_step * t, layer), gx, gy);
    }
    const float a = prev_d - prev_t, b = d - t;   // a >= 0 above the map, b <= 0 below
    return lerp(prev_t, t, saturate(a / max(a - b, 1e-5f)));
}

// Soft self-shadow from the hit towards the key light: how far the map
// rises above a ray climbing from depth `th` to the top. `light_step` is the
// uv shift per unit of depth climbed. 1 lit, 0 shadowed.
float pom_shadow(float2 uvh, float th, float layer, float2 light_step, float2 gx, float2 gy) {
    if (th <= 0.002f) return 1.0f;
    float occ = 0.0f;
    [loop] for (int s = 1; s <= 6; ++s) {
        const float f = float(s) / 6.0f;
        const float tt = th * (1.0f - f);   // the ray's depth here
        const float d = pom_depth(float3(uvh + light_step * (th - tt), layer), gx, gy);
        occ = max(occ, (tt - d) * (1.0f - 0.8f * f));
    }
    return 1.0f - saturate(occ * 5.0f);
}

// The same, faded out as the light grazes the surface (`lz` its cosine to
// the normal): there the march strides far across the map in six steps and
// finds occluders that are not there, and the mesh's own N.L already says
// the surface is barely lit.
float pom_shadow_faded(float2 uvh, float th, float layer, float2 light_step, float lz, float2 gx, float2 gy) {
    const float k = saturate((lz - 0.05f) / 0.25f);
    if (k <= 0.0f) return 1.0f;
    return lerp(1.0f, pom_shadow(uvh, th, layer, light_step, gx, gy), k);
}

// How much parallax at a distance. Parallax shrinks on screen with distance
// in proportion to its depth, so the reach scales with it: 300 x depth_m,
// 10 to 45 m -- 3 cm of ground or bark stops at 10 m, 15 cm of rock at 45 --
// with the fade over the far half. A flat 45 m cost +0.33 ms walking, most
// of it on meadow ground under grass where 3 cm could not be seen.
float pom_fade(float dist, float depth_m) {
    const float far_m = clamp(depth_m * 300.0f, 10.0f, 45.0f);
    return saturate((far_m - dist) / (0.5f * far_m));
}

#endif
