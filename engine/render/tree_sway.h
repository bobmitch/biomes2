// ---------------------------------------------------------------------------
// How far the wind moves a standing tree's crown: compiled as C++ AND HLSL,
// so the mesh trees.hlsl bends and the instance the RT scene leans
// (rt_scene.cpp) move by one formula in one gust (wind.h).
//
// The offset at the TOP of the tree, in world metres: x and z go down the tree
// as h^2 (h the height over the top), y -- the crown dropping as it leans --
// as h^3. trees.hlsl applies exactly that per vertex. A ray-tracing instance
// can only be transformed affinely, so the RT scene applies a SHEAR -- linear
// in h, the trunk's base fixed -- fitted to h^2 over the crown, where the
// shadow comes from (ts_shear_fit).
//
//  - lean:  the crown pushed downwind by the drag, speed squared -- 3% of the
//           height in a 20 m/s gale for a stiffness of 1, nothing in a breeze;
//  - swing: the crown swinging about that lean, mostly along the wind with a
//           little across it, speed^1.5 -- ~5 cm on a 20 m tree at the median
//           3.5 m/s, ~40 cm in a gale;
//  - bend:  the species' own give (archetypes.json shape.bend): a birch bends
//           further than an oak, a spruce less.
// Frequencies stay fixed: one that followed the wind would slide the phase
// by t x its change, and t is in thousands of seconds.
// ---------------------------------------------------------------------------
#ifndef ENGINE_TREE_SWAY_H
#define ENGINE_TREE_SWAY_H

#include "engine/render/wind.h"

#ifdef __cplusplus
#define TS_FN inline
#include <cmath>
#include <cstdint>
struct ts_float3 { float x, y, z; };
#define TS_F3 ts_float3
#define TS_UINT uint32_t
#define TS_OUT_F float&
inline float ts_sin(float a) { return std::sin(a); }
inline float ts_sqrt(float a) { return std::sqrt(a); }
#else
#define TS_FN
#define TS_F3 float3
#define TS_UINT uint
#define TS_OUT_F out float
#define ts_sin sin
#define ts_sqrt sqrt
#endif

// The shear the RT scene leans an instance by, over the offset at the top:
// the least-squares line through h^2 over the crown (h from 0.35 to 1),
// pinned at the base. 0.8: off by a fifth of the lean at the very top and
// under a tenth through the middle of the crown.
#define TS_SHEAR_FIT 0.8f

// `top` the model's top (its own metres, as trees.hlsl takes it), `id` the
// tree's id (its phase), (px, pz) the trunk, `t` the frame clock; the wind as
// weather.h gives it -- direction (dx, dz), mean speed `u`, gustiness, drift
// (sx, sz); `mult` the settings' multiplier; `bend` the species'. `out_u` is
// the local wind speed, which the leaf cards flutter by.
TS_FN TS_F3 ts_crown_offset(float top, TS_UINT id, float px, float pz, float t,
                            float dx, float dz, float u, float gust, float sx, float sz,
                            float mult, float bend, TS_OUT_F out_u) {
    const float phase = float(id & 1023u) * 0.37f;
    const float uh = u * wind_gust_at(px, pz, dx, dz, gust, sx, sz, t);
    out_u = uh;
    const float u20 = uh / 20.0f;
    const float lean = 0.03f * u20 * u20 * bend;
    const float swing = (0.022f * u20 * ts_sqrt(u20) + 0.0015f) * bend;
    const float along = ts_sin(t * 0.9f + phase) * 0.6f + ts_sin(t * 2.1f + phase * 1.7f) * 0.25f;
    const float across = ts_sin(t * 1.3f + phase * 2.3f) * 0.3f;
    const float a = (lean + swing * along) * top * mult;
    const float c = swing * across * top * mult;
    TS_F3 o;
    o.x = dx * a - dz * c;
    o.z = dz * a + dx * c;
    o.y = -0.5f * lean * top * mult;
    return o;
}

#endif  // ENGINE_TREE_SWAY_H
