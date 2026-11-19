// ---------------------------------------------------------------------------
// The sea's small-scale detail: the wave spectrum the FFT tiles carry
// (shaders/ocean_fft.hlsl), compiled as C++ AND HLSL like water_wave.h so the
// CPU's normalisation sums the very spectrum the GPU synthesises.
//
// The big waves are water_wave.h's 32 Gerstner trains, down to 4.1 m, which
// the geometry and physics share. Below that the sea is ripples nobody floats
// on and every glint comes from: two FFT tiles, 31 m and 7.7 m across (not
// multiples of each other, so their repeats never line up), split in
// wavenumber so no ripple is counted twice. Their HEIGHT is never drawn as
// geometry; their slopes shade, their Jacobian makes foam.
//
// Presentation only, like water_wave.h: invariant 9 does not reach it.
// ---------------------------------------------------------------------------
#ifndef ENGINE_OCEAN_SPECTRUM_H
#define ENGINE_OCEAN_SPECTRUM_H

#ifdef __cplusplus
#define OD_FN inline
#include <cmath>
inline float od_exp(float a) { return std::exp(a); }
inline float od_sqrt(float a) { return std::sqrt(a); }
inline float od_smoothstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
#else
#define OD_FN
#define od_exp exp
#define od_sqrt sqrt
#define od_smoothstep smoothstep
#endif

#define OD_N 256
#define OD_CASCADES 2
static const float od_tile[OD_CASCADES] = {31.0f, 7.7f};
// Wavenumbers: the detail starts where the Gerstner trains stop (4.5 m) and
// the two tiles hand over at 0.6 m, over the last 15% below it, so the two
// bands sum to one everywhere.
#define OD_K_LOW   (6.2831853f / 4.5f)
#define OD_K_SPLIT (6.2831853f / 0.6f)

// The wavenumber of texel (x, y) of cascade c: standard FFT order, the upper
// half of each axis the negative frequencies.
OD_FN float od_kx(int c, int x) {
    const int m = x < OD_N / 2 ? x : x - OD_N;
    return 6.2831853f / od_tile[c] * float(m);
}

// Phillips (Tessendorf 2001) round the wind, times the cascade's band: the
// spectral density of height at wavevector (kx, kz), before normalisation.
// U is the wind the sea is developed for, (dx, dz) the way it blows.
// Ripples running against the wind keep 7% (a pure |k.w|^2 has none, and a
// sea with no cross-chop reads as combed); the smallest are damped at ~1 cm.
OD_FN float od_phillips(int c, float kx, float kz, float U, float dx, float dz) {
    const float k2 = kx * kx + kz * kz;
    if (k2 < 1e-8f) return 0.0f;
    const float k = od_sqrt(k2);
    const float Lw = U * U / 9.81f;
    const float kd = (kx * dx + kz * dz) / k;
    float p = od_exp(-1.0f / (k2 * Lw * Lw + 1e-12f)) / (k2 * k2) * kd * kd;
    if (kd < 0.0f) p = p * 0.07f;
    p = p * od_exp(-k2 * 1e-4f);
    float band;
    if (c == 0) band = od_smoothstep(OD_K_LOW * 0.8f, OD_K_LOW, k) * (1.0f - od_smoothstep(OD_K_SPLIT * 0.85f, OD_K_SPLIT, k));
    else        band = od_smoothstep(OD_K_SPLIT * 0.85f, OD_K_SPLIT, k);
    return p * band;
}

// The mean square slope of the WHOLE sea surface at wind U, both components
// together: Cox and Munk's clean-surface fit, 0.003 + 0.00512 U. The detail is
// normalised so that the Gerstner trains' slope plus its own makes this, which
// is what the sun's glitter on real water is measured against.
OD_FN float od_cox_munk(float U) {
    return 0.003f + 0.00512f * U;
}

#endif  // ENGINE_OCEAN_SPECTRUM_H
