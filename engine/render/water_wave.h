// ---------------------------------------------------------------------------
// The sea surface, as a function of position, time and sea state.
//
// Compiled BOTH as C++ and as HLSL, the same trick worldgen/shaders uses and
// for the same reason: the number the harness prints and the surface the
// shader draws have to be the same surface, or the measurement is of something
// nobody is looking at.
//
// It is NOT part of the ADR-001 dialect and must never be confused with it:
//   - invariant 1 is untouched -- worldgen/ does not know this file exists;
//   - invariant 9 does not apply. That rule governs everything upstream of the
//     GOLDEN HASH, because libm is not exactly specified and glibc disagrees
//     with the MSVC CRT by an ULP. The sea surface is presentation. It feeds
//     no hash, no save and no generator decision, so `sin` and `cos` are
//     allowed here where they are forbidden four directories away. That is the
//     same line that lets the slope metric use `atan`.
//
// Storms are a planned mechanic, so wave height is a runtime variable rather
// than a constant: everything below scales from `sea_state`, 0 calm to 1 storm,
// and nothing in the shape is baked.
// ---------------------------------------------------------------------------
#ifndef ENGINE_WATER_WAVE_H
#define ENGINE_WATER_WAVE_H

#ifdef __cplusplus
#define WW_FN inline
#include <cmath>
struct ww_float2 { float x, y; };
struct ww_float3 { float x, y, z; };
#define WW_F2 ww_float2
#define WW_F3 ww_float3
// An OUT parameter has to be spelled differently in the two languages, and
// getting this wrong is silent. The first version took `WW_F3 n[1]`, which in
// C++ decays to a pointer and works, and in HLSL is an array passed BY VALUE
// -- so the shader's normal was never written and stayed uninitialised. It did
// not look like garbage either: the Fresnel term saturated and the whole sea
// came out exactly the colour of the sky, at every depth, which reads as a
// shading choice rather than a bug. Measuring the pixels was what caught it;
// three different depths returning one identical colour is not a sea.
#define WW_OUT_F3 ww_float3&
inline float ww_saturate(float a) { return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
inline float ww_sin(float a) { return std::sin(a); }
inline float ww_cos(float a) { return std::cos(a); }
inline float ww_sqrt(float a) { return std::sqrt(a); }
#else
#define WW_FN
#define WW_F2 float2
#define WW_F3 float3
#define WW_OUT_F3 out float3
#define ww_saturate saturate
#define ww_sin sin
#define ww_cos cos
#define ww_sqrt sqrt
#endif

// Four Gerstner waves. Four rather than one because a single trochoid reads as
// corrugated iron from the air -- the eye finds the period immediately. Four
// incommensurate wavelengths on spread headings do not resolve into a pattern
// at any distance you can see the sea from.
//
// Directions are unit by construction (0.6/0.8 triples and a diagonal), so no
// normalise is needed and none of them is axis aligned: an axis-aligned swell
// over a voxel world lines up with the chunk grid and reads as a rendering
// artefact even when it is not.
#define WW_WAVES 4

WW_FN WW_F2 ww_direction(int i) {
    WW_F2 d;
    if (i == 0)      { d.x =  0.80f; d.y =  0.60f; }
    else if (i == 1) { d.x = -0.60f; d.y =  0.80f; }
    else if (i == 2) { d.x =  0.70710678f; d.y = -0.70710678f; }
    else             { d.x = -0.28734789f; d.y = -0.95782629f; }
    return d;
}

// Metres. Deliberately not harmonics of one another.
WW_FN float ww_wavelength(int i) {
    if (i == 0) return 73.0f;
    if (i == 1) return 31.0f;
    if (i == 2) return 13.0f;
    return 5.3f;
}

// How the total amplitude is split. The long swell carries most of it, which
// is what makes a storm read as swell with chop on top rather than as uniform
// noise that merely got bigger.
WW_FN float ww_amplitude_share(int i) {
    if (i == 0) return 0.52f;
    if (i == 1) return 0.27f;
    if (i == 2) return 0.14f;
    return 0.07f;
}

// Total crest-to-trough amplitude in metres, from the sea state. Quadratic so
// that the lower half of the dial is a usable range of calm weather instead of
// everything above 0.2 looking like a gale. --sea-report prints what this
// actually produces; do not trust the formula over the measurement.
WW_FN float ww_total_amplitude(float sea_state) {
    return 0.10f + 3.70f * sea_state * sea_state;
}

// Gerstner steepness. At 1 the crests come to a point and beyond it the
// surface passes through itself and renders inside out, so this is capped
// below that however hard the sea state is driven. Rising with sea state is
// most of what makes a storm look like one: the crests sharpen.
WW_FN float ww_steepness(float sea_state) {
    const float q = 0.25f + 0.55f * sea_state;
    return q > 0.85f ? 0.85f : q;
}

// Deep-water dispersion: long waves travel faster, which is why a swell
// outruns its chop. Without it every wavelength moves at one speed and the
// whole surface slides rigidly, which is the single most obvious tell of fake
// water in motion.
WW_FN float ww_angular_speed(float k) {
    return ww_sqrt(9.81f * k);
}

// How much of a wave survives at this sampling rate. This is the SAME rule the
// terrain uses for its octaves, and for the same reason: a wave shorter than
// about twice the spacing you are sampling at cannot be represented, and
// drawing it anyway produces aliasing that moves when the camera does.
//
// Faded rather than cut, which also matters for the same reason it does there
// -- a wave that vanishes between one frame and the next is a pop, and the
// first version of this sea showed exactly that as a hard line in the
// mid-ground where displaced water met flat water.
//
// `min_wavelength` of 0 means "no limit": that is the C++ harness, which
// samples the function directly and has no grid to be limited by.
WW_FN float ww_wave_lod(float wavelength, float min_wavelength) {
    if (min_wavelength <= 0.0f) return 1.0f;
    return ww_saturate((wavelength / min_wavelength - 0.5f) * 2.0f);
}

// The amplitude that SURVIVES at this sampling rate. Anything asking "how
// big are the waves here" has to use this and not ww_total_amplitude, which
// describes a sea sampled perfectly. The whitecap test did not, and so it
// measured crest height against an amplitude two thirds of which the grid was
// never carrying: foam covered 2.9% of a storm sea and did not move when the
// threshold was lowered, because the numerator could not reach it.
WW_FN float ww_total_amplitude_lod(float sea_state, float min_wavelength) {
    float a = 0.0f;
    for (int i = 0; i < WW_WAVES; i = i + 1) {
        a = a + ww_amplitude_share(i) * ww_wave_lod(ww_wavelength(i), min_wavelength);
    }
    return ww_total_amplitude(sea_state) * a;
}

// The displacement of the point that RESTS at (x, z), and the surface normal
// there. Gerstner waves move water horizontally as well as vertically, so the
// returned position is not above the input point -- that is the whole reason
// crests sharpen and troughs flatten.
//
// `fade` scales displacement toward zero, for distance: a 5 m wave sampled
// every 167 m near the horizon is noise, and the honest thing to draw there is
// a flat sheet with a sheen on it rather than aliasing confetti.
WW_FN WW_F3 ww_surface(float x, float z, float t, float sea_state, float fade,
                       float min_wavelength, WW_OUT_F3 out_normal) {
    const float total = ww_total_amplitude(sea_state) * fade;
    const float steep = ww_steepness(sea_state);

    WW_F3 p;
    p.x = x; p.y = 0.0f; p.z = z;

    float nx = 0.0f, nz = 0.0f, ny = 1.0f;

    for (int i = 0; i < WW_WAVES; i = i + 1) {
        const WW_F2  d = ww_direction(i);
        const float  wl = ww_wavelength(i);
        const float  a = total * ww_amplitude_share(i) * ww_wave_lod(wl, min_wavelength);
        const float  k = 6.2831853f / wl;
        const float  w = ww_angular_speed(k);
        // Steepness is shared out over the waves so the SUM stays under the
        // self-intersection limit. Per-wave steepness would be within bounds
        // individually and fold the surface where crests coincided.
        const float  q = steep / (float(WW_WAVES) * k * (a > 1e-6f ? a : 1e-6f));
        const float  qc = q * k * a > 1.0f ? 1.0f / (k * (a > 1e-6f ? a : 1e-6f)) : q;

        const float phase = k * (d.x * x + d.y * z) - w * t;
        const float s = ww_sin(phase);
        const float c = ww_cos(phase);

        p.x = p.x + qc * a * d.x * c;
        p.z = p.z + qc * a * d.y * c;
        p.y = p.y + a * s;

        const float wa = k * a;
        nx = nx - d.x * wa * c;
        nz = nz - d.y * wa * c;
        ny = ny - qc * wa * s;
    }

    const float len = ww_sqrt(nx * nx + ny * ny + nz * nz);
    const float inv = len > 1e-6f ? 1.0f / len : 1.0f;
    out_normal.x = nx * inv;
    out_normal.y = ny * inv;
    out_normal.z = nz * inv;
    return p;
}

#endif  // ENGINE_WATER_WAVE_H
