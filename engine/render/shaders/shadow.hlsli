// ---------------------------------------------------------------------------
// Sun shadows: three cascades, sampled by the terrain (mesh.hlsl) and the
// trees (trees.hlsl) through this one function, so ground and trees agree on
// where the shadow falls.
//
// Cascades are chosen by distance from the camera; each is an orthographic
// view down the sun fitted to a sphere round its slice of the view frustum
// and snapped to whole texels (Renderer::compute_cascades), so its edges stay
// put as the camera moves instead of crawling. Past the last cascade the
// shadow fades out: terrain beyond ~500 m is lit, and fog covers the change.
// ---------------------------------------------------------------------------
#ifndef ENGINE_SHADOW_HLSLI
#define ENGINE_SHADOW_HLSLI

cbuffer ShadowConstants : register(b2) {
    row_major float4x4 light_vp[4];
    float4 shadow_split;       // far edge of each cascade, metres from the camera
    float4 shadow_texel;       // world metres per shadow texel, per cascade
    float4 shadow_depth_range; // metres of light-space depth per cascade
    float3 shadow_cam;         float shadow_res;   // negative when shadows are off
    // The frame's light (ADR-004 L2), read through lighting.hlsli. Here
    // because every lit shader already binds this buffer for its shadows.
    float4 l_sun_dir;          // the key light -- sun, or the moon at night -- towards it
    float4 l_sun_lux;          // its illuminance at the camera's altitude
    float4 l_sky_mean;         // the sky's mean radiance over the upper hemisphere
    float4 l_ground;           // the lit ground's radiance, for the lower hemisphere
    float4 l_sky_zenith;
    float4 l_sky_horizon_sun;  // the horizon under the key light, and opposite it
    float4 l_sky_horizon_away;
    // Local lights' froxel clusters (ADR-004 L3, local_lights.hlsli).
    float4 l_cluster;          // tiles x, tiles y, slices, lights this frame
    float4 l_cluster2;         // near, slices / ln(far / near), screen width, screen height
    float4 l_cam_fwd;          // the camera's forward, for a pixel's view depth; w unused (was "RT shadows on")
    // The canopy map (ADR-004 L6, canopy.hlsli): its min corner x, z, the
    // height its crown heights are relative to; w 0 none, 1 built, 2 applied.
    float4 l_canopy;
    // The screen-space bounce (ssgi.hlsli): last frame's jittered view-
    // projection and camera; w on.
    row_major float4x4 l_prev_vp;
    float4 l_prev_cam;
    // The heightfield GI (hfgi.hlsli): the map's min corner x, z; w on.
    float4 l_hf;
    // The shadow filter's frame (x), for its rotation to walk under TAA;
    // y parallax on; z the first cascade drawn -- 2 under RT shadows, which
    // skip the near two (their readers start at the first drawn); w the
    // filter's rotation mode (BIOMES_PCSS, an A/B -- see cascade_pcss).
    float4 l_shadow_frame;
    // Each cascade's corner of its slice: its resolution over the slice's.
    float4 shadow_scale;
    // The far terrain shadow (far_shadow.h): map corner x, z, extent; w on.
    float4 l_far;
    // The canopy shell (forest_map.h, forest.hlsli): the map's corner x, z,
    // extent, w on; then the band it fades in over, metres from the camera.
    float4 l_forest;
    float4 l_forest2;
    // The swash (water_wave.h), for the wet sand under it (mesh.hlsl): the
    // eight longest trains' amplitudes, then sea level, run-up, the frame
    // clock, and w on.
    float4 l_swash_a;
    float4 l_swash_b;
    float4 l_swash;
    // The wet world (ADR-005 C4, wet.hlsli): wetness 0..1, puddles 0..1, the
    // rain now 0..1, the ripples' clock (s, wrapped at 1000).
    float4 l_wet;
    // A lightning flash (ADR-005 C5, flash.hlsli): where its light comes
    // from and its luminous intensity now (cd, 0 none); its colour.
    float4 l_flash;
    float4 l_flash_col;
    // Bare ground (mesh.hlsl, GroundLook): macro variation's strength, the
    // scan's fade to its mean (start, end, metres), and (RockLook) the most
    // a rock hex turns, as a tangent; the ground scan hex-tiled (x > 0.5),
    // far detail's strength and its scale over the tile, rock hex-tiled
    // (w > 0.5).
    float4 l_look;
    float4 l_nf;        // the near field (near_field.h): the volume's min world voxel xyz; w ready
    float4 l_look3;
    // Rock (RockLook): the far layer's strength at full distance, its scale
    // over the tile, the distances it fades in over (metres).
    float4 l_look4;
};

Texture2DArray<float>  g_shadow     : register(t8);
SamplerComparisonState g_shadow_cmp : register(s1);
SamplerState           g_shadow_point : register(s4);   // the blocker search reads raw depths
Texture2D<float>       g_far_shadow   : register(t43);  // the far terrain shadow, 1024^2 over 8 km

// Past the cascades: the terrain's own shadow from a coarse map marched
// towards the key light (far_shadow.hlsl). Bilinear by four loads -- every
// pipeline that filters shadows binds the point sampler, not all a linear
// one -- and faded to lit over the map's last tenth.
float far_shadow(float3 world) {
    if (l_far.w < 0.5f) return 1.0f;
    const float2 uv = float2((world.x - l_far.x) / l_far.z, 1.0f - (world.z - l_far.y) / l_far.z);
    const float2 e = min(uv, 1.0f - uv);
    if (min(e.x, e.y) <= 0.0f) return 1.0f;
    const float2 g = uv * 1024.0f - 0.5f;
    const float2 fl = floor(g);
    const float2 f = g - fl;
    const int2 i0 = clamp(int2(fl), 0, 1023), i1 = clamp(int2(fl) + 1, 0, 1023);
    const float a = g_far_shadow.Load(int3(i0.x, i0.y, 0)), b = g_far_shadow.Load(int3(i1.x, i0.y, 0));
    const float c = g_far_shadow.Load(int3(i0.x, i1.y, 0)), d = g_far_shadow.Load(int3(i1.x, i1.y, 0));
    const float v = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
    return lerp(1.0f, v, saturate(min(e.x, e.y) * 10.0f));
}

static const float2 kShadowDisk[16] = {
    float2(-0.94201624f, -0.39906216f), float2(0.94558609f, -0.76890725f), float2(-0.09418410f, -0.92938870f),
    float2(0.34495938f, 0.29387760f),   float2(-0.91588581f, 0.45771432f), float2(-0.81544232f, -0.87912464f),
    float2(-0.38277543f, 0.27676845f),  float2(0.97484398f, 0.75648379f),  float2(0.44323325f, -0.97511554f),
    float2(0.53742981f, -0.47373420f),  float2(-0.26496911f, -0.41893023f), float2(0.79197514f, 0.19090188f),
    float2(-0.24188840f, 0.99706507f),  float2(-0.81409955f, 0.91437590f), float2(0.19984126f, 0.78641367f),
    float2(0.14383161f, -0.14100790f)};

// The key light's angular diameter in radians: the sun's 0.53 degrees (the
// moon's is within 3% of it). A penumbra is this times the distance from
// the caster to the receiver.
static const float kKeyLightDiameter = 0.0093f;

// One cascade, filtered by contact hardening (PCSS): a blocker search for
// how far above the receiver the caster is, then a Poisson PCF as wide as
// the sun makes that penumbra -- never under 1.5 texels, which is the
// filtering a cascade needs whatever the sun says. A pixel the search
// finds wholly lit or wholly shadowed stops there, which is most of them.
float cascade_pcss(float3 world, float3 n, uint c) {
    const float texel = shadow_texel[c];
    // Offset along the normal by a texel and a half: the cheapest cure for
    // acne that does not detach shadows from their casters.
    const float3 p = world + n * (texel * 1.5f);
    const float4 lp = mul(float4(p, 1.0f), light_vp[c]);
    const float2 uv = float2(lp.x * 0.5f + 0.5f, 0.5f - lp.y * 0.5f) * shadow_scale[c];
    const float range = shadow_depth_range[c];
    // 0.12 m of constant bias, in this cascade's depth units.
    const float zr = lp.z - 0.12f / range;
    const float res = abs(shadow_res);
    const float uv_per_m = 1.0f / (texel * res);
    // The rotation the 16 taps are turned by. Under a canopy the map is
    // dappled at the texel, the penumbra is metres of it, and 16 taps are a
    // coarse Monte Carlo estimate of that coverage -- so WHERE its error
    // lands decides whether TAA can average it away or not. Per shadow
    // TEXEL (mode 0) every screen pixel over one texel shares one estimate,
    // so the error comes in blocks wider than TAA's 3x3 neighbourhood: the
    // clamp then locks on to the block and the history is thrown away.
    // Per PIXEL (mode 2, the default) each pixel draws its own, the
    // neighbourhood spans many draws, and the history survives to be
    // averaged. Mode 1 is the old fixed-per-texel rotation, kept for the
    // A/B; it is stable and speckled. BIOMES_PCSS picks one.
    const float mode = l_shadow_frame.w;
    const float grid = mode > 1.5f ? 32.0f : 1.0f;
    const float2 st = floor(uv * (res * grid));
    const float walk = (mode > 0.5f && mode < 1.5f) ? 0.0f : 0.618034f * l_shadow_frame.x;
    const float ang = 6.2831853f * frac(52.9829189f * frac(dot(st, float2(0.06711056f, 0.00583715f))) + walk);
    const float ca = cos(ang), sa = sin(ang);
    // Blocker search: casters up to ~30 m above, whose penumbra is ~0.28 m.
    const float search = clamp(30.0f * kKeyLightDiameter, 2.0f * texel, 48.0f * texel) * uv_per_m;
    // The NEAREST blocker sets the penumbra, not the average: at a trunk's
    // foot the search also finds the crown 15 m up, and averaging the two
    // blurred the trunk's shadow where it should be sharpest.
    float znear = 0.0f, nb = 0.0f;
    [unroll] for (int k = 0; k < 12; ++k) {
        const float2 o = kShadowDisk[k];
        const float2 q = uv + float2(o.x * ca - o.y * sa, o.x * sa + o.y * ca) * search;
        const float zb = g_shadow.SampleLevel(g_shadow_point, float3(q, float(c)), 0.0f);
        if (zb < zr) { znear = max(znear, zb); nb += 1.0f; }
    }
    if (nb < 0.5f) return 1.0f;
    const float zb = znear;
    const float penumbra = max((zr - zb) * range * kKeyLightDiameter, 1.5f * texel);
    const float radius = penumbra * uv_per_m;
    // Wholly inside an umbra wider than the filter: no PCF to do.
    if (nb > 11.5f && radius < search) return 0.0f;
    float s = 0.0f;
    [unroll] for (int k2 = 0; k2 < 16; ++k2) {
        const float2 o = kShadowDisk[k2];
        const float2 q = uv + float2(o.x * ca - o.y * sa, o.x * sa + o.y * ca) * radius;
        s += g_shadow.SampleCmpLevelZero(g_shadow_cmp, float3(q, float(c)), zr);
    }
    return s * (1.0f / 16.0f);
}

// 1 lit, 0 in shadow, by the cascades. `n` is the surface normal used for
// the offset; pass the geometric one where there is one, a crown's for
// foliage. Each cascade blends into the next over its last 12%, so their
// seams do not show; the last blends into the far terrain shadow over its
// last 20% -- 1.36 to 1.6 km. (It used to end at 520 m with a 52 m fade, and
// shadows switched on in front of the player as they walked.)
float cascade_visibility(float3 world, float3 n) {
    if (shadow_res < 0.0f) return 1.0f;
    const float d = length(world - shadow_cam);
    if (d >= shadow_split.w || l_far.w > 1.5f) return far_shadow(world);   // w 2: the far map everywhere (BIOMES_FAR_DBG)
    const uint c = max(d < shadow_split.x ? 0u : (d < shadow_split.y ? 1u : (d < shadow_split.z ? 2u : 3u)),
                       uint(l_shadow_frame.z + 0.5f));
    const float lo = c == 0u ? 0.0f : shadow_split[c - 1u];
    const float hi = shadow_split[c];
    const float band = (c == 3u ? 0.2f : 0.12f) * (hi - lo);
    const float t = saturate((d - (hi - band)) / band);
    float s = cascade_pcss(world, n, c);
    if (t > 0.0f) {
        if (c == 3u) s = lerp(s, far_shadow(world), t);   // into the far terrain shadow
        else s = lerp(s, cascade_pcss(world, n, c + 1u), t);
    }
    return s;
}

// What a surface shader multiplies its key light by: the cascades (and the
// far terrain shadow past them). Ray-traced shadows replaced this until they
// were removed (git tag rt-last).
float shadow_factor(float3 world, float3 n) {
    return cascade_visibility(world, n);
}

#include "engine/render/shaders/canopy.hlsli"
// A static linear clamp sampler at s3 in every lit pipeline's root signature,
// once DDGI's (ddgi.hlsli, removed with hardware RT); the forest map, the
// heightfield GI, the screen-space bounce and the wet world sample through it.
SamplerState g_gi_linear : register(s3);
#include "engine/render/shaders/ssgi.hlsli"
#include "engine/render/shaders/hfgi.hlsli"

#endif
