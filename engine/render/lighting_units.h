// ---------------------------------------------------------------------------
// The light in the world, in physical units -- one copy, read by the C++
// renderer (crack test, clear colour, shadow and post constants) and by every
// scene shader through lighting.hlsli. Compiles as C++ AND as HLSL, the same
// trick as water_wave.h; it holds constants only, so nothing here can drift
// between the two.
//
// ADR-004 L1. Illuminance in lux, radiance in cd/m^2 (nits), per channel in
// linear sRGB primaries. Until L2's atmosphere computes these from the sun's
// elevation, they are a clear midday with the sun 54 degrees up:
//
//   sun     ~100,000 lux on a surface facing it, slightly warm;
//   sky     ~13,400 lux on open level ground (an average radiance of
//           ~4,300 nits), blue;
//   horizon ~5,000 nits, paler and brighter than the zenith (~2,800).
//
// The ratio is what matters for the look: a level surface in shadow gets
// 14% of what one in sun gets, which is a clear day. The sky's HUES are the
// pre-L1 sky's, linearised (blue/red about 3 at the horizon, 9 at the
// zenith): the first values here were nearly neutral, and a sea view, which
// is all sky and reflected sky, metered them to a grey slate. The absolute scale is
// what auto-exposure meters against, so a torch (L3) in lumens will sit at
// its true brightness beside the sun rather than at a guessed one.
//
// Presentation only. Nothing upstream of the golden hash includes this.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDER_LIGHTING_UNITS_H
#define ENGINE_RENDER_LIGHTING_UNITS_H

// The direction TOWARDS the sun, normalised: (0.45, 0.78, 0.35) / |...|.
static const float kSunDirX = 0.465778f;
static const float kSunDirY = 0.807348f;
static const float kSunDirZ = 0.362271f;

// Sun illuminance normal to the beam, lux.
static const float kSunLuxR = 103700.0f;
static const float kSunLuxG = 99600.0f;
static const float kSunLuxB = 93400.0f;

// The sky's mean radiance over the upper hemisphere, nits: pi times this is
// the illuminance it puts on level ground.
static const float kSkyMeanR = 2300.0f;
static const float kSkyMeanG = 4300.0f;
static const float kSkyMeanB = 9800.0f;

// The sky at the horizon and at the zenith, nits. The horizon is also the
// clear colour of the ambient target and the colour of the fog, so it is
// chosen to be exactly representable in half float (the crack test compares
// against what the target holds): multiples of 4 below 8192, of 8 above.
static const float kSkyHorizonR = 3160.0f;
static const float kSkyHorizonG = 5080.0f;
static const float kSkyHorizonB = 9552.0f;
static const float kSkyZenithR = 950.0f;
static const float kSkyZenithG = 2700.0f;
static const float kSkyZenithB = 9100.0f;

// What the ground reflects back up, as an albedo: a mix of grass, soil and
// shade. The no-GI ambient's lower hemisphere is this times what lands on it.
static const float kGroundBounceR = 0.13f;
static const float kGroundBounceG = 0.15f;
static const float kGroundBounceB = 0.10f;

// Scene shaders clamp to this. Half float tops out at 65504 and an Inf in the
// targets poisons TAA's history for good; a sun glint on water is the one
// thing bright enough to reach it.
static const float kSceneMaxNits = 60000.0f;

#endif
