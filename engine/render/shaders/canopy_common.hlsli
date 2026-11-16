// The canopy map's constants, shared by its build (canopy.hlsl) and its
// readers (canopy.hlsli). canopy.h has the C++ side.
#ifndef ENGINE_CANOPY_COMMON_HLSLI
#define ENGINE_CANOPY_COMMON_HLSLI

static const uint  kCanopySize = 512u;   // texels a side, CanopyMap::kSize
static const float kCanopyTexel = 1.0f;  // metres
static const int   kCanopySlices = 6;
// The diffuse sky through a slab of optical depth tau: the cosine-weighted
// mean of exp(-tau / cos) is 2 E3(tau), which exp(-1.5 tau) follows within
// a few percent from tau 0.5 to 2.
static const float kCanopyPath = 1.5f;
// Slice k is roughly a disc of radius 0.82 x 2^k texels; the sky a point
// sees through a canopy dz above it spreads over a disc of radius ~dz (the
// median cosine-weighted direction is 45 degrees off vertical).
static const float kCanopySpread = 1.22f;
// How far a sky ray mixes the crowns in its footprint, 0 (each ray one
// column) to 1 (all of them): canopy.hlsli.
static const float kCanopyMix = 0.25f;

#endif
