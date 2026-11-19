// ---------------------------------------------------------------------------
// The wind at a point, for everything that bends in it (engine/render/weather.h).
//
// `wv` is (direction x, direction z, mean speed m/s, gustiness 0..1) and
// `drift` the metres the air has moved since t = 0. Gusts are a noise field
// carried BY the air -- sampled at p - drift -- so a gust front is seen to
// cross a meadow, then a wood, at the wind's own speed. Sampling at
// p - dir * speed * t instead would make every gust jump the moment the wind
// changed.
//
// Each renderer passes its own copies of wv and drift in its own constants;
// this file holds only the functions, so it can be included anywhere.
// ---------------------------------------------------------------------------
#ifndef WIND_HLSLI
#define WIND_HLSLI

float wind_hash(float2 p) {
    p = frac(p * float2(0.1031f, 0.1030f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}

float wind_vnoise(float2 p) {
    const float2 i = floor(p);
    const float2 f = frac(p);
    const float2 u = f * f * (3.0f - 2.0f * f);
    const float a = wind_hash(i);
    const float b = wind_hash(i + float2(1.0f, 0.0f));
    const float c = wind_hash(i + float2(0.0f, 1.0f));
    const float d = wind_hash(i + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// The local speed over the mean, ~0.3 to ~2.2, averaging 1. Gust cells are
// ~60 m, stretched along the wind as real gusts are, with a smaller, faster
// turbulence on top that also evolves in place (the `t` term) so a still
// day does not freeze into one pattern.
float wind_gust(float2 p, float4 wv, float2 drift, float t) {
    const float2 dir = wv.xy;
    const float2 q = p - drift;
    // Coordinates along and across the wind.
    const float2 a = float2(dot(q, dir), dot(q, float2(-dir.y, dir.x)));
    const float big = wind_vnoise(a * float2(1.0f / 90.0f, 1.0f / 45.0f));
    const float small = wind_vnoise(a * (1.0f / 14.0f) + float2(0.0f, t * 0.11f));
    const float n = saturate(big * 0.75f + small * 0.25f);
    // n^2 averages ~1/3 for value noise, so 3 n^2 averages ~1.
    return (1.0f - wv.w) + wv.w * 3.0f * n * n;
}

// The local wind as a velocity (x, z), m/s.
float2 wind_velocity(float2 p, float4 wv, float2 drift, float t) {
    return wv.xy * (wv.z * wind_gust(p, wv, drift, t));
}

#endif
