// ---------------------------------------------------------------------------
// The wind at a point, for everything that bends in it (engine/render/weather.h).
//
// `wv` is (direction x, direction z, mean speed m/s, gustiness 0..1) and
// `drift` the metres the air has moved since t = 0. The field itself is
// engine/render/wind.h, shared with the CPU (the RT scene leans its trees in
// the same gusts); this file only adapts it to the shaders' float4 / float2.
//
// Each renderer passes its own copies of wv and drift in its own constants;
// this file holds only the functions, so it can be included anywhere.
// ---------------------------------------------------------------------------
#ifndef WIND_HLSLI
#define WIND_HLSLI

#include "engine/render/wind.h"

// The local speed over the mean, ~0.3 to ~2.2, averaging 1.
float wind_gust(float2 p, float4 wv, float2 drift, float t) {
    return wind_gust_at(p.x, p.y, wv.x, wv.y, wv.w, drift.x, drift.y, t);
}

// The local wind as a velocity (x, z), m/s.
float2 wind_velocity(float2 p, float4 wv, float2 drift, float t) {
    return wv.xy * (wv.z * wind_gust(p, wv, drift, t));
}

#endif
