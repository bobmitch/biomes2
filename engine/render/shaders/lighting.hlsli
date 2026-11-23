// ---------------------------------------------------------------------------
// How every scene shader lights a surface (ADR-004 L1).
//
//   units          the sun in lux, the sky in nits (lighting_units.h).
//   the light      the FRAME's (L2): the key light -- sun, or moon at night --
//                  and the sky's radiance, computed by the atmosphere on the
//                  CPU each frame (sky.cpp) and declared by the includer as
//                  l_sun_dir, l_sun_lux, l_sky_mean, l_ground, l_sky_zenith,
//                  l_sky_horizon_sun, l_sky_horizon_away (float4 each; the
//                  shadow constants for terrain, trees and grass, the sea's
//                  own constants for the sea).
//   albedo         authored as sRGB-encoded colours, as every colour in this
//                  engine always was; srgb_to_linear() before any lighting.
//   BRDF           Lambert diffuse + GGX specular (Smith height-correlated
//                  visibility, Schlick Fresnel), dielectric F0 0.04, metal for
//                  the rare case.
//   indirect       gi_diffuse / gi_specular / gi_sky_visibility, the
//                  shading-facing GI interface the ADR fixes before any GI
//                  exists. What is here is its no-RT fallback: the sky's
//                  irradiance from the upper hemisphere and the ground's
//                  bounce from the lower one, which is the shape of the old
//                  `albedo * 0.22 + (0.10, 0.13, 0.18) * sky` made honest.
//                  A better GI replaces the bodies, never the calls (the
//                  heightfield GI does now; DDGI did until rt-last).
//
// Ambient occlusion is NOT applied here. The post chain multiplies the
// ambient target by GTAO (scene_out.hlsli), which is the Low tier's "sky
// irradiance x GTAO".
// ---------------------------------------------------------------------------
#ifndef ENGINE_LIGHTING_HLSLI
#define ENGINE_LIGHTING_HLSLI

#include "engine/render/lighting_units.h"

static const float  kPi = 3.14159265f;
#define kSunDir         (l_sun_dir.xyz)
#define kSunLux         (l_sun_lux.rgb)
#define kSkyMean        (l_sky_mean.rgb)
#define kGroundRadiance (l_ground.rgb)

float3 srgb_to_linear(float3 c) {
    c = max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

// The sky's radiance along a direction, from three samples of the real one:
// the horizon under the key light, the horizon opposite, the zenith. The
// sunward glow is narrow, so the blend round the horizon is cubed towards
// it. Good enough for what reads it -- rough specular and the sea's
// reflection; the sky itself is drawn from the full sky-view table.
float3 sky_radiance(float3 dir) {
    const float2 hd = dir.xz, hs = l_sun_dir.xz;
    const float lh = length(hd), ls = length(hs);
    const float a = (lh > 1e-4f && ls > 1e-4f) ? dot(hd / lh, hs / ls) : 0.0f;
    const float s = 0.5f + 0.5f * a;
    const float3 horizon = lerp(l_sky_horizon_away.rgb, l_sky_horizon_sun.rgb, s * s * s);
    return lerp(horizon, l_sky_zenith.rgb, saturate(dir.y * 1.4f));
}

// --- the shading-facing GI interface (ADR-004), no-RT fallback --------------
//
// `view` is the unit direction from the surface TO the eye.

// What gi_diffuse found, for gi_specular and gi_sky_visibility in the same
// shading call: how much of the sky term the canopy leaves (the fallback's
// answer) and how much of an open-sky ambient the probes found (DDGI's,
// which knows about caves, so it scales the ground's share too). Unset < 0.
static float g_gi_sky = -1.0f;
static float g_gi_all = -1.0f;
// Irradiance a vertex shader already computed (xyz), and its g_gi_all (w);
// w < 0 none. Trees and grass read the probes per vertex: their cards and
// blades overdraw too much to read eight probes a pixel.
static float4 g_gi_vertex = float4(0.0f, 0.0f, 0.0f, -1.0f);
// How much of gi_diffuse's answer came from the probes: 0 the fallback.
static float g_gi_cover = 0.0f;

float gi_canopy(float3 world) {
#ifdef ENGINE_CANOPY_HLSLI
    return canopy_visibility(world);
#else
    return 1.0f;
#endif
}

float gi_luma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// Trees and grass carried a 1.3x fill on their ambient: a stand-in for the
// light that bounces between leaves and blades and off the floor under a
// crown, which the fallback cannot see. The probes compute that bounce, so
// wherever they answered the stand-in goes; the fallback keeps it. Call
// after gi_diffuse.
float gi_fill(float stand_in) { return lerp(stand_in, 1.0f, g_gi_cover); }

// Irradiance (lux) arriving at a surface with this normal, from everything
// but the sun's direct beam: two uniform hemispheres -- sky above (what the
// canopy leaves) and ground below, integrated against the cosine lobe, which
// is exact rather than a fit -- replaced by the heightfield GI wherever its
// map covers the point. (DDGI's ray-traced probes answered first until
// hardware RT was removed: git tag rt-last.)
float3 gi_diffuse(float3 world, float3 n, float3 view) {
    if (g_gi_vertex.w >= 0.0f) {
        g_gi_sky = 1.0f;
        g_gi_all = g_gi_vertex.w;
        return g_gi_vertex.xyz;
    }
    const float up = 0.5f + 0.5f * n.y;
    g_gi_cover = 0.0f;
    const float sv = gi_canopy(world);
    float3 fb = kPi * (kSkyMean * (up * sv) + kGroundRadiance * (1.0f - up));
    const float3 fb_hemis = fb;   // the two hemispheres, before the heightfield
#ifdef ENGINE_HFGI_HLSLI
    // The heightfield GI: the sky the terrain round it leaves (through the
    // canopy) and the light that terrain really bounces, in place of the
    // two uniform hemispheres -- wherever the map covers the point.
    {
        float vis_e, hcov;
        float3 hb;
        hfgi_sample(world, n, vis_e, hb, hcov);
        if (hcov > 0.0f) fb = lerp(fb, kSkyMean * (vis_e * sv) + hb, hcov);
    }
#endif
#ifdef ENGINE_SSGI_HLSLI
    // The bounce off the surfaces on screen round it (last frame's cs_ssgi).
    if (l_prev_cam.w > 1.5f) return ssgi_irradiance(world) + 1e-3f;   // BIOMES_SSGI_DBG: the bounce alone
    fb += ssgi_irradiance(world);
#endif
    g_gi_sky = sv;
    // The specular follows the diffuse: what the heightfield took from the
    // two hemispheres (a valley floor, a cave called covered) it takes from
    // the reflection too.
    g_gi_all = saturate(gi_luma(fb) / max(gi_luma(fb_hemis), 1e-4f));
    return fb;
}

// Radiance (nits) arriving along the mirror direction, prefiltered by
// roughness: a smooth surface sees the sky or the ground in that direction,
// a rough one sees the hemisphere's average. Scaled by what gi_diffuse
// found: a probe-lit cave reflects no sky.
float3 gi_specular(float3 world, float3 n, float3 view, float roughness) {
    const float3 r = reflect(-view, n);
    const float sky = g_gi_sky >= 0.0f ? g_gi_sky : gi_canopy(world);
    const float all = g_gi_all >= 0.0f ? g_gi_all : 1.0f;
    const float3 sharp = lerp(kGroundRadiance, sky_radiance(r) * sky, smoothstep(-0.15f, 0.15f, r.y));
    const float3 broad = lerp(kGroundRadiance, kSkyMean * sky, 0.5f + 0.5f * n.y);
    return lerp(sharp, broad, roughness * roughness) * all;
}

// How much of the sky a point sees: 0 in a sealed cave. What gi_diffuse
// found relative to the two hemispheres, else what the canopy leaves.
float gi_sky_visibility(float3 world, float3 n) {
    if (g_gi_all >= 0.0f) return g_gi_sky * g_gi_all;
    return gi_canopy(world);
}

// --- BRDF -----------------------------------------------------------------

float3 f_schlick(float3 f0, float vh) {
    const float k = 1.0f - vh;
    const float k2 = k * k;
    return f0 + (1.0f - f0) * (k2 * k2 * k);
}

float d_ggx(float nh, float a) {
    const float a2 = a * a;
    const float d = nh * nh * (a2 - 1.0f) + 1.0f;
    return a2 / (kPi * d * d);
}

float v_smith_ggx(float nv, float nl, float a) {
    const float a2 = a * a;
    const float gv = nl * sqrt(nv * nv * (1.0f - a2) + a2);
    const float gl = nv * sqrt(nl * nl * (1.0f - a2) + a2);
    return 0.5f / max(gv + gl, 1e-5f);
}

// The split-sum environment BRDF, Karis's analytic fit (no LUT).
float3 env_brdf(float3 f0, float roughness, float nv) {
    const float4 c0 = float4(-1.0f, -0.0275f, -0.572f, 0.022f);
    const float4 c1 = float4(1.0f, 0.0425f, 1.04f, -0.04f);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28f * nv)) * r.x + r.y;
    const float2 ab = float2(-1.04f, 1.04f) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

struct Surface {
    float3 albedo;      // LINEAR
    float  roughness;   // perceptual, 0..1
    float  metal;
    float3 n;           // unit
    float3 v;           // unit, towards the eye
};

Surface make_surface(float3 albedo_linear, float roughness, float3 n, float3 v) {
    Surface s;
    s.albedo = albedo_linear;
    s.roughness = roughness;
    s.metal = 0.0f;
    s.n = n;
    s.v = v;
    return s;
}

// Radiance from the sun's direct beam, times `shadow` (the sun's visibility).
// Radiance from a light arriving along `l` with illuminance `E` (lux, on a
// surface facing it), times `shadow`: the sun's, and since L3 each local
// light's (local_lights.hlsli).
float3 direct_light(Surface s, float3 l, float3 E, float shadow) {
    const float nl = dot(s.n, l);
    if (nl <= 0.0f || shadow <= 0.0f) return 0.0f;
    const float nv = max(dot(s.n, s.v), 1e-4f);
    const float3 h = normalize(l + s.v);
    const float nh = saturate(dot(s.n, h));
    const float vh = saturate(dot(s.v, h));
    const float a = max(s.roughness * s.roughness, 0.002f);
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), s.albedo, s.metal);
    const float3 F = f_schlick(f0, vh);
    const float3 spec = d_ggx(nh, a) * v_smith_ggx(nv, nl, a) * F;
    const float3 diff = (1.0f - F) * s.albedo * (1.0f - s.metal) / kPi;
    return (diff + spec) * E * (nl * shadow);
}

float3 sun_light(Surface s, float shadow) {
    return direct_light(s, kSunDir, kSunLux, shadow);
}

// Radiance from everything else: the GI interface's diffuse and specular.
float3 ambient_light(Surface s, float3 world) {
    const float nv = max(dot(s.n, s.v), 1e-4f);
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), s.albedo, s.metal);
    const float3 env = env_brdf(f0, s.roughness, nv);
    const float3 diff = s.albedo * (1.0f - s.metal) / kPi * gi_diffuse(world, s.n, s.v);
    return diff * (1.0f - env) + env * gi_specular(world, s.n, s.v, s.roughness);
}

// Thin translucent leaves and blades: lit on the side facing the sun, and
// THROUGH from the side away from it. `wrap` softens the terminator the way
// a crown of many cards does (a crown is a volume, not a surface); `through`
// is the share of the light the leaf passes on, peaked towards the sun so a
// back-lit crown glows; `transmit` its colour (a leaf passes green).
float3 leaf_light(float3 albedo, float3 n, float3 v, float3 l, float3 E, float wrap, float through, float shadow) {
    const float lam = saturate((dot(n, l) + wrap) / (1.0f + wrap));
    const float back = saturate(dot(-v, l));
    const float3 diff = albedo / kPi * lam;
    // Forward scattering through the leaf, in the leaf's own colour, which
    // is roughly what a leaf's transmittance is.
    const float3 trans = albedo / kPi * (through * back * back * back);
    return (diff + trans) * E * shadow;
}

float3 leaf_sun_light(float3 albedo, float3 n, float3 v, float wrap, float through, float shadow) {
    return leaf_light(albedo, n, v, kSunDir, kSunLux, wrap, through, shadow);
}

#endif
