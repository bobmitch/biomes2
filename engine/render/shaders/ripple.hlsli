// ---------------------------------------------------------------------------
// Rain rings on standing water (ADR-005 C4), shared by the terrain's shading
// (wet.hlsli) and the composite's puddle reflections (post.hlsl), so the
// rings that shade a puddle are the rings that bend what it reflects.
// Nothing here reads a constant buffer: the caller passes the rain and the
// clock.
// ---------------------------------------------------------------------------
#ifndef RIPPLE_HLSLI
#define RIPPLE_HLSLI

float wet_hash(float2 p) {
    p = frac(p * float2(0.1031f, 0.1030f));
    p += dot(p, p.yx + 33.33f);
    return frac((p.x + p.y) * p.x);
}

// The rings' slope (d height / dx, dz): one drop per 0.4 m cell at a time,
// a ring 5 cm wide spreading to 22 cm and fading. `rain` 0..1 is the share of
// cells with drops; `clock` seconds.
float2 rain_ripples(float2 xz, float rain, float clock) {
    const float cell = 0.4f;
    const float2 base = floor(xz / cell);
    float2 grad = 0.0f;
    [unroll] for (int j = -1; j <= 1; ++j) {
        [unroll] for (int i = -1; i <= 1; ++i) {
            const float2 c = base + float2(i, j);
            const float h0 = wet_hash(c), h1 = wet_hash(c + 41.7f), h2 = wet_hash(c + 93.1f);
            if (h2 > rain) continue;
            const float2 centre = (c + 0.2f + 0.6f * float2(h0, h1)) * cell;
            const float period = 0.7f + 0.6f * h0;
            const float phase = frac(clock / period + h1);
            const float2 d = xz - centre;
            const float dist = length(d);
            const float x = (dist - phase * 0.22f) / 0.025f;
            if (abs(x) < 1.0f && dist > 1e-4f) {
                const float amp = 0.0025f * (1.0f - phase) * (1.0f - phase);
                grad += d / dist * (amp * 3.14159265f / 0.025f * cos(x * 3.14159265f));
            }
        }
    }
    return grad;
}

// The water's normal: up, bent by the rings while it rains, the rings faded
// as the pixel's footprint `fp` (m) outgrows them.
float3 puddle_normal(float2 xz, float rain, float clock, float fp) {
    const float k = saturate(2.0f - fp / 0.02f);
    if (rain <= 0.0f || k <= 0.0f) return float3(0.0f, 1.0f, 0.0f);
    const float2 g = rain_ripples(xz, rain, clock) * k;
    return normalize(float3(-g.x, 1.0f, -g.y));
}

#endif
