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
    float moving;      float pad;       // 1: moved within 60 frames (the carried torch): the near field's far hits skip it
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
// Shadows: a 6144^2 atlas of 512^2 tiles (144 MB). The 24 most important
// shadowed lights hold six tiles each (a cube); at most twelve faces are
// redrawn a frame, and a light that has not moved keeps the faces it has.
// Eight before: a ninth fire lit straight through rock, its fill with it.
// A light past the 24th still does.
static const uint32_t kLightAtlas       = 6144;
static const uint32_t kLightTile        = 512;
static const uint32_t kMaxShadowLights  = 24;
static const uint32_t kMaxFacesPerFrame = 12;
// The player's body casts into the faces too. They are cached, so a face the
// body is in -- or was in, to take its old shadow out -- is redrawn when the
// pose moves: at most this many a frame, on top of the twelve.
static const uint32_t kMaxBodyFacesPerFrame = 12;
// The carried torch's flame sits just over the fist: nothing of the body
// within this many metres of it casts, or the hand would black it out.
static const float    kTorchBodyClear   = 0.4f;

}  // namespace engine
