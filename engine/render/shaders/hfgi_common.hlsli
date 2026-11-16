// The heightfield GI's layout (ADR-004 L6; height_gi.h has the C++ side):
// a 256 x 256 map of 2 m texels round the camera -- 512 m -- and the L1
// spherical-harmonic basis its gather and its readers share.
#ifndef ENGINE_HFGI_COMMON_HLSLI
#define ENGINE_HFGI_COMMON_HLSLI

static const uint  kHfSize = 256u;
static const float kHfTexel = 2.0f;
static const float kHfExtent = 512.0f;      // kHfSize * kHfTexel
static const float kHfNone = -100000.0f;    // height where the top-down view saw nothing

// L1 SH, stored (c0, cx, cy, cz). Irradiance from a radiance projection:
// E(n) = pi Y00 c0 + (2 pi / 3) Y1 (c . n) -- the clamped-cosine lobe's
// zonal factors (Ramamoorthi and Hanrahan).
static const float kHfY00 = 0.282095f;
static const float kHfY1 = 0.488603f;

float hf_irradiance(float4 c, float3 n) {
    return max(0.0f, 3.14159265f * kHfY00 * c.x + 2.0943951f * kHfY1 * dot(c.yzw, n));
}

#endif
