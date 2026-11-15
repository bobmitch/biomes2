// ---------------------------------------------------------------------------
// How every scene shader lights a surface (ADR-004 L1).
//
//   units          lighting_units.h: the sun in lux, the sky in nits.
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
//                  DDGI (L6) replaces the bodies, never the calls.
//
// Ambient occlusion is NOT applied here. The post chain multiplies the
// ambient target by GTAO (scene_out.hlsli), which is the Low tier's "sky
// irradiance x GTAO".
// ---------------------------------------------------------------------------
#ifndef ENGINE_LIGHTING_HLSLI
#define ENGINE_LIGHTING_HLSLI

#include "engine/render/lighting_units.h"

static const float  kPi = 3.14159265f;
static const float3 kSunDir   = float3(kSunDirX, kSunDirY, kSunDirZ);
static const float3 kSunLux   = float3(kSunLuxR, kSunLuxG, kSunLuxB);
static const float3 kSkyMean  = float3(kSkyMeanR, kSkyMeanG, kSkyMeanB);
static const float3 kSkyHorizon = float3(kSkyHorizonR, kSkyHorizonG, kSkyHorizonB);
static const float3 kSkyZenith  = float3(kSkyZenithR, kSkyZenithG, kSkyZenithB);
// The ground as a light source for whatever faces down at it: its albedo
// times the sun on level ground plus the sky, over pi.
static const float3 kGroundRadiance =
    float3(kGroundBounceR, kGroundBounceG, kGroundBounceB) / kPi * (kSunLux * kSunDirY + kPi * kSkyMean);

float3 srgb_to_linear(float3 c) {
    c = max(c, 0.0f);
    return c <= 0.04045f ? c / 12.92f : pow((c + 0.055f) / 1.055f, 2.4f);
}

// The sky's radiance along a direction. A gradient from the horizon to the
// zenith; below the horizon, the horizon. L2's sky-view LUT replaces it.
float3 sky_radiance(float3 dir) {
    return lerp(kSkyHorizon, kSkyZenith, saturate(dir.y * 1.4f));
}

// --- the shading-facing GI interface (ADR-004), no-RT fallback --------------
//
// `view` is the unit direction from the surface TO the eye.

// Irradiance (lux) arriving at a surface with this normal, from everything
// but the sun's direct beam.
float3 gi_diffuse(float3 world, float3 n, float3 view) {
    // Two uniform hemispheres, sky above and ground below: integrated against
    // the cosine lobe this is exact, not a fit.
    const float up = 0.5f + 0.5f * n.y;
    return kPi * (kSkyMean * up + kGroundRadiance * (1.0f - up));
}

// Radiance (nits) arriving along the mirror direction, prefiltered by
// roughness: a smooth surface sees the sky or the ground in that direction,
// a rough one sees the hemisphere's average.
float3 gi_specular(float3 world, float3 n, float3 view, float roughness) {
    const float3 r = reflect(-view, n);
    const float3 sharp = lerp(kGroundRadiance, sky_radiance(r), smoothstep(-0.15f, 0.15f, r.y));
    const float3 broad = lerp(kGroundRadiance, kSkyMean, 0.5f + 0.5f * n.y);
    return lerp(sharp, broad, roughness * roughness);
}

// How much of the sky a point sees: 0 in a sealed cave. Without GI nothing
// knows, so everything sees all of it -- which is the problem L6 exists for.
float gi_sky_visibility(float3 world, float3 n) {
    return 1.0f;
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
float3 sun_light(Surface s, float shadow) {
    const float nl = dot(s.n, kSunDir);
    if (nl <= 0.0f || shadow <= 0.0f) return 0.0f;
    const float nv = max(dot(s.n, s.v), 1e-4f);
    const float3 h = normalize(kSunDir + s.v);
    const float nh = saturate(dot(s.n, h));
    const float vh = saturate(dot(s.v, h));
    const float a = max(s.roughness * s.roughness, 0.002f);
    const float3 f0 = lerp(float3(0.04f, 0.04f, 0.04f), s.albedo, s.metal);
    const float3 F = f_schlick(f0, vh);
    const float3 spec = d_ggx(nh, a) * v_smith_ggx(nv, nl, a) * F;
    const float3 diff = (1.0f - F) * s.albedo * (1.0f - s.metal) / kPi;
    return (diff + spec) * kSunLux * (nl * shadow);
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
float3 leaf_sun_light(float3 albedo, float3 n, float3 v, float wrap, float through, float shadow) {
    const float lam = saturate((dot(n, kSunDir) + wrap) / (1.0f + wrap));
    const float back = saturate(dot(-v, kSunDir));
    const float3 diff = albedo / kPi * lam;
    // Forward scattering through the leaf, in the leaf's own colour, which
    // is roughly what a leaf's transmittance is.
    const float3 trans = albedo / kPi * (through * back * back * back);
    return (diff + trans) * kSunLux * shadow;
}

#endif
