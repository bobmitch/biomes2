#pragma once
// ---------------------------------------------------------------------------
// Local lights (ADR-004 L3): torches, fires, later magic. Point or spot, in
// lumens, physically falling off as the inverse square of distance and
// windowed to zero at their range so a light only touches the clusters it
// can reach. Flicker is animation on top, driven by the renderer's clock.
//
// The CPU owns the list; each frame the renderer turns it into GPU records
// (flicker applied, a carried torch placed at the camera), sorts them into
// froxel clusters on the GPU (lights.hlsl), gives the most important few a
// shadow in the light atlas, and every lit shader walks its pixel's cluster
// (local_lights.hlsli).
// ---------------------------------------------------------------------------
#include <cstdint>

namespace engine {

struct LocalLight {
    float pos[3] = {0.0f, 0.0f, 0.0f};
    float lumens = 200.0f;
    // Linear sRGB with a luminance of 1: the light's colour, not its power.
    float color[3] = {1.92f, 0.81f, 0.21f};
    float range = 18.0f;              // metres; the window takes it to zero here
    int   type = 0;                   // 0 point, 1 spot
    float spot_dir[3] = {0.0f, -1.0f, 0.0f};
    float spot_outer_deg = 40.0f, spot_inner_deg = 30.0f;   // half-angles
    bool  shadow = true;
    float flicker = 1.0f;             // 0 steady, 1 a flame
    uint32_t id = 0;                  // set by the renderer
};

// A burning torch: ~200 lm at ~1900 K. In daylight it is invisible beside a
// sun of 100,000 lux, as it should be; at night it is everything.
inline LocalLight make_torch() {
    LocalLight l;
    l.lumens = 200.0f;
    l.range = 18.0f;
    return l;
}

// A campfire: brighter, redder, wider.
inline LocalLight make_campfire() {
    LocalLight l;
    l.lumens = 1500.0f;
    l.color[0] = 2.10f; l.color[1] = 0.76f; l.color[2] = 0.14f;
    l.range = 30.0f;
    return l;
}

// --- GPU layouts, mirrored in local_lights.hlsli and lights.hlsl ------------

struct LocalLightGpu {
    float pos[3];      float range;
    float intensity[3]; float type;    // candela, per channel
    float spot_dir[3]; float cos_outer;
    float cos_inner;   int32_t shadow;  // first face in the face table, or -1
    float pad[2];
};
static_assert(sizeof(LocalLightGpu) == 64, "LocalLightGpu must match local_lights.hlsli");

struct LightFaceGpu {
    float view_proj[16];
    float rect[4];     // atlas uv of the tile: x, y, size, texel
};
static_assert(sizeof(LightFaceGpu) == 80, "LightFaceGpu must match local_lights.hlsli");

static const uint32_t kMaxLights        = 256;
// Froxel clusters: 16 x 9 screen tiles x 24 slices, log-spaced 0.5-400 m.
static const uint32_t kClusterX         = 16;
static const uint32_t kClusterY         = 9;
static const uint32_t kClusterZ         = 24;
static const uint32_t kClusterCount     = kClusterX * kClusterY * kClusterZ;
static const uint32_t kClusterMaxItems  = 128;   // lights.hlsl kMaxItems, local_lights.hlsli
static const float    kClusterNear      = 0.5f;
static const float    kClusterFar       = 400.0f;
// Shadows: a 4096^2 atlas of 512^2 tiles. The eight most important shadowed
// lights hold six tiles each (a cube); at most twelve faces are redrawn a
// frame, and a light that has not moved keeps the faces it has.
static const uint32_t kLightAtlas       = 4096;
static const uint32_t kLightTile        = 512;
static const uint32_t kMaxShadowLights  = 8;
static const uint32_t kMaxFacesPerFrame = 12;

}  // namespace engine
