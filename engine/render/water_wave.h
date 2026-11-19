// ---------------------------------------------------------------------------
// The sea surface, as a function of position, time, the weather and depth.
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
//     no hash, no save and no generator decision, so `sin`, `cos` and `tanh`
//     are allowed here where they are forbidden four directories away. That
//     is the same line that lets the slope metric use `atan`.
//
// The sea is a FIXED set of thirty-two wave trains whose amplitudes the weather
// sets (engine/render/weather.cpp builds a WwSea from the wind and the swell).
// Only amplitudes ever change at runtime. A wave's heading or wavelength
// changing over time slides its phase by k * |x| * the change, which 5 km
// from the origin is faster than the wave itself travels: a wind that veered
// would set the whole sea skating sideways. So a veering wind does not turn
// waves; it hands the energy to the trains that already point its way.
// ---------------------------------------------------------------------------
#ifndef ENGINE_WATER_WAVE_H
#define ENGINE_WATER_WAVE_H

#define WW_WAVES 32
#include "engine/render/wind.h"   // a value noise, for the swash's lobes

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
#define WW_OUT_F float&
#define WW_IN_SEA const WwSea&
#define WW_IN_SWASH const WwSwash&
inline float ww_saturate(float a) { return a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a); }
inline float ww_sin(float a) { return std::sin(a); }
inline float ww_cos(float a) { return std::cos(a); }
inline float ww_sqrt(float a) { return std::sqrt(a); }
#else
#define WW_FN
#define WW_F2 float2
#define WW_F3 float3
#define WW_OUT_F3 out float3
#define WW_OUT_F out float
#define WW_IN_SEA WwSea
#define WW_IN_SWASH WwSwash
#define ww_saturate saturate
#define ww_sin sin
#define ww_cos cos
#define ww_sqrt sqrt
#endif

// tanh for x >= 0, as a rational (the [3/2] Pade form, within 2.5% and
// exactly 1 from x = 3 on). The depth term runs it for every train at every
// pixel, and HLSL's tanh is two exps and a divide: thirty-two of them, three
// times a pixel, were most of the sea's cost. One formula in both languages,
// so the CPU's swimmer and the drawn sea still agree.
WW_FN float ww_tanh_pos(float x) {
    if (x >= 3.0f) return 1.0f;
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

// What the weather decides: each train's amplitude in deep water (metres,
// the sine's amplitude, so a train alone is 2a crest to trough) and the
// target steepness -- how sharp the crests are allowed to get.
struct WwSea {
    float amp[WW_WAVES];
    float steep;
};

// Headings: consecutive trains a golden angle (137.5 deg) apart, so ANY run
// of consecutive wavelengths -- the long ones a swell lives in, the short
// ones a breeze lives in -- covers the compass about evenly, and whichever
// way the wind blows several trains of every band point near it.
//
// Thirty-two, not sixteen. With sixteen the trains round a gale's peak were
// four, about 90 degrees apart, and the wind's spread handed nearly all the
// energy to the two either side of it: two trains crossing at 85 degrees are
// an egg crate, and the crests stood in a regular lattice of foam squares.
// The offset keeps every heading at least 2.5 degrees off both axes (the
// best a golden-angle set of 32 allows): a train along an axis lines up with
// the chunk grid and reads as a rendering artefact even when it is not.
static const float ww_dir_x[WW_WAVES] = {
    0.17175515f, 0.53880538f, -0.96635179f, 0.88631009f, -0.34072316f, -0.38383278f,
    0.90677585f, -0.95342381f, 0.49927423f, 0.21712524f, -0.81947703f, 0.99138847f,
    -0.64256098f, -0.04377953f, 0.70712431f, -0.99904339f, 0.76620269f, -0.13090465f,
    -0.57315266f, 0.97615452f, -0.86641926f, 0.30158668f, 0.42165800f, -0.92342165f,
    0.94014678f, -0.46304830f, -0.25727197f, 0.84245698f, -0.98513115f, 0.61035312f,
    0.08502036f, -0.73573586f
};
static const float ww_dir_z[WW_WAVES] = {
    -0.98513967f, 0.84243027f, -0.25722406f, -0.46309224f, 0.94016367f, -0.92340262f,
    0.42161304f, 0.30163396f, -0.86644402f, 0.97614375f, -0.57311203f, -0.13095381f,
    0.76623455f, -0.99904122f, 0.70708925f, -0.04373000f, -0.64259897f, 0.99139496f,
    -0.81944861f, 0.21707685f, 0.49931719f, -0.95343876f, 0.90675495f, -0.38378700f,
    -0.34076978f, 0.88633305f, -0.96633904f, 0.53876361f, 0.17180399f, -0.79212945f,
    0.99637921f, -0.67726860f
};
// Metres, log-spaced from a long swell to a ripple (each 0.889 of the one
// before), so no two are harmonics of one another and no pattern resolves at
// any distance.
static const float ww_wavelengths[WW_WAVES] = {
    157.00f, 139.58f, 124.10f, 110.33f, 98.09f, 87.21f, 77.53f, 68.93f, 61.28f, 54.49f,
    48.44f, 43.07f, 38.29f, 34.04f, 30.27f, 26.91f, 23.92f, 21.27f, 18.91f, 16.81f,
    14.95f, 13.29f, 11.81f, 10.50f, 9.34f, 8.30f, 7.38f, 6.56f, 5.83f, 5.19f, 4.61f,
    4.10f
};

WW_FN WW_F2 ww_direction(int i) {
    WW_F2 d;
    d.x = ww_dir_x[i];
    d.y = ww_dir_z[i];
    return d;
}

WW_FN float ww_wavelength(int i) {
    return ww_wavelengths[i];
}

// Deep-water dispersion: long waves travel faster, which is why a swell
// outruns its chop. Without it every wavelength moves at one speed and the
// whole surface slides rigidly, which is the single most obvious tell of fake
// water in motion.
//
// Deep water ON PURPOSE, everywhere. Shallow water really is slower
// (w^2 = g k tanh(k h)), but a speed that varies across the sea cannot go into
// `k x - w t`: the phase difference between two places grows with t without
// bound, and after a few minutes the wavefronts over a shoal are wound into
// stripes. Doing it properly means integrating phase along the ray, which a
// pure function of (x, z, t) cannot. Depth changes amplitude only.
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

// Breaking: water h deep cannot hold a sea whose significant height is much
// over about half of h (surf-zone measurements put Hs / h at 0.4-0.6), so the
// local sea is scaled down to WW_BREAK_HS x depth of Hs. Its biggest single
// waves then reach about the depth itself, which is where they break.
//
// The first cap was on the SUM of the amplitudes -- every crest coinciding at
// once -- which with thirty-two trains is about three times the height a
// random sea ever shows, so over five metres of water a gale was cut to a
// fifth of what the water holds, and a coast in a storm looked flat.
#define WW_BREAK_HS 0.55f
// Q (the Gerstner throw over the amplitude) never above this, so a calm sea
// whose slopes are tiny does not get its little waves thrown into loops.
#define WW_MAX_Q 2.5f

// Depth: a train feels the bottom from a fraction of its wavelength down, and
// tanh(k h) -- the same factor in finite-depth dispersion -- fades it from 1
// in deep water to 0 at the shore. So the long swell dies first over a shoal
// and the short chop survives into a lagoon, which is what a sheltered
// lagoon looks like.
//
// The term runs WW_DEPTH_GAIN times faster than tanh(k h) itself, so a train
// is near full size from a quarter of its wavelength down rather than a
// half: plain tanh(k h) took half the swell off every coast-side camera, and
// real waves SHOAL there -- they lose speed, not height, until they break.
// The breaking cap is what finally takes the height.
#define WW_DEPTH_GAIN 2.0f
WW_FN float ww_depth_factor(float k, float depth) {
    return depth <= 0.0f ? 0.0f : ww_tanh_pos(WW_DEPTH_GAIN * k * depth);
}

// One train's amplitude HERE, before the breaking cap: the weather's, times
// the depth's share, times what this sampling rate can carry, times `fade`.
WW_FN float ww_raw_amp(WW_IN_SEA sea, int i, float depth, float fade, float min_wavelength) {
    if (sea.amp[i] <= 0.0f) return 0.0f;   // pruned by the weather (weather.cpp)
    const float wl = ww_wavelength(i);
    const float k = 6.2831853f / wl;
    return sea.amp[i] * fade * ww_depth_factor(k, depth) * ww_wave_lod(wl, min_wavelength);
}

// The sea HERE, as three numbers every train's amplitude follows from.
// Factored out because three functions need the identical numbers -- the
// surface, its inversion for physics and the foam's normaliser -- and a
// second copy of this is exactly how a swimmer ends up floating on a
// different sea than the one drawn. HLSL has no array out-parameters worth
// trusting (see WW_OUT_F3), so callers recompute each train with
// ww_raw_amp x scale.
//   x: the scale on every raw amplitude (the breaking cap);
//   y: Q, the Gerstner throw over the amplitude;
//   z: the significant height after the cap, 4 x the RMS elevation.
WW_FN WW_F3 ww_local(WW_IN_SEA sea, float depth, float fade, float min_wavelength) {
    float sum_a2 = 0.0f, sum_ka = 0.0f;
    for (int i = 0; i < WW_WAVES; i = i + 1) {
        const float a = ww_raw_amp(sea, i, depth, fade, min_wavelength);
        sum_a2 = sum_a2 + a * a;
        sum_ka = sum_ka + a * (6.2831853f / ww_wavelength(i));
    }
    // A train of amplitude a has variance a^2 / 2; Hs = 4 sqrt(variance).
    const float hs = 2.8284271f * ww_sqrt(sum_a2);
    const float cap = WW_BREAK_HS * (depth > 0.0f ? depth : 0.0f);
    const float scale = hs > cap ? (hs > 1e-9f ? cap / hs : 0.0f) : 1.0f;
    const float ka = sum_ka * scale;
    // Steepness is shared out over the trains so the SUM of their k a Q stays
    // at `steep`, under the self-intersection limit. This one stays a
    // worst case on purpose: crests that do coincide must still not fold.
    float q = ka > 1e-9f ? sea.steep / ka : WW_MAX_Q;
    q = q > WW_MAX_Q ? WW_MAX_Q : q;
    WW_F3 r;
    r.x = scale; r.y = q; r.z = hs * scale;
    return r;
}

// The significant height that SURVIVES at this sampling rate and depth.
// Anything asking "how big are the waves here" has to use this, not the
// weather's deep-water amplitudes, which describe a sea sampled perfectly.
// The whitecap test once measured crest height against an amplitude two
// thirds of which the grid was never carrying: foam covered 2.9% of a storm
// sea and did not move when the threshold was lowered, because the numerator
// could not reach it.
WW_FN float ww_significant_height_lod(WW_IN_SEA sea, float depth, float min_wavelength) {
    return ww_local(sea, depth, 1.0f, min_wavelength).z;
}

// The weather's deep-water significant height.
WW_FN float ww_significant_height(WW_IN_SEA sea) {
    float a2 = 0.0f;
    for (int i = 0; i < WW_WAVES; i = i + 1) a2 = a2 + sea.amp[i] * sea.amp[i];
    return 2.8284271f * ww_sqrt(a2);
}

// The sum of the weather's deep-water amplitudes: an upper bound on the
// crest (and the trough's depth) anywhere, every train in phase at once.
WW_FN float ww_total_amplitude(WW_IN_SEA sea) {
    float a = 0.0f;
    for (int i = 0; i < WW_WAVES; i = i + 1) a = a + sea.amp[i];
    return a;
}

// The displacement of the point that RESTS at (x, z), and the surface normal
// there. Gerstner waves move water horizontally as well as vertically, so the
// returned position is not above the input point -- that is the whole reason
// crests sharpen and troughs flatten.
//
// `depth` is metres of water under the point (anything over ~80 m is deep for
// every train). `fade` scales displacement toward zero, for distance: a 4 m
// wave sampled every 167 m near the horizon is noise, and the honest thing to
// draw there is a flat sheet with a sheen on it rather than aliasing confetti.
// `scale` and `qc` are ww_local's x and y; ww_surface below works them out,
// and the water's pixel shader takes them from its vertices instead, which
// saves a pass over every train at every pixel.
//
// `out_jacobian` is the determinant of the horizontal throw's Jacobian:
// 1 on still water, falling as crests compress and folding at 0, which is
// where a real wave breaks -- the foam's measure (water.hlsl), the same
// matrix ww_rest_point inverts.
WW_FN WW_F3 ww_surface_with(float x, float z, float t, WW_IN_SEA sea, float depth, float fade,
                            float min_wavelength, float scale, float qc, WW_OUT_F3 out_normal,
                            WW_OUT_F out_jacobian) {

    WW_F3 p;
    p.x = x; p.y = 0.0f; p.z = z;

    float nx = 0.0f, nz = 0.0f, ny = 1.0f;
    float jxx = 1.0f, jxz = 0.0f, jzz = 1.0f;

    for (int i = 0; i < WW_WAVES; i = i + 1) {
        const float a = ww_raw_amp(sea, i, depth, fade, min_wavelength) * scale;
        if (a <= 0.0f) continue;
        const WW_F2  d = ww_direction(i);
        const float  k = 6.2831853f / ww_wavelength(i);
        const float  w = ww_angular_speed(k);

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
        const float g = qc * wa * s;
        jxx = jxx - g * d.x * d.x;
        jxz = jxz - g * d.x * d.y;
        jzz = jzz - g * d.y * d.y;
    }
    out_jacobian = jxx * jzz - jxz * jxz;

    const float len = ww_sqrt(nx * nx + ny * ny + nz * nz);
    const float inv = len > 1e-6f ? 1.0f / len : 1.0f;
    out_normal.x = nx * inv;
    out_normal.y = ny * inv;
    out_normal.z = nz * inv;
    return p;
}

WW_FN WW_F3 ww_surface(float x, float z, float t, WW_IN_SEA sea, float depth, float fade,
                       float min_wavelength, WW_OUT_F3 out_normal) {
    const WW_F3 loc = ww_local(sea, depth, fade, min_wavelength);
    float jac;
    return ww_surface_with(x, z, t, sea, depth, fade, min_wavelength, loc.x, loc.y, out_normal, jac);
}

// Newton steps ww_rest_point takes. --sea-report prints the horizontal miss
// this leaves at every wind; read it before changing this.
#define WW_INVERT_STEPS 4

// The point that RESTS somewhere and is DISPLACED to (x, z) -- the inverse of
// ww_surface's horizontal throw.
//
// This is the question physics asks and the shader never does. The shader
// starts from a rest point and draws wherever it lands; a swimmer starts from
// where it IS and needs the surface above that spot. Evaluating ww_surface at
// the swimmer's own (x, z) answers a different question -- the height of the
// water that started there, which has since moved sideways by up to the
// Gerstner throw -- and at a storm that is a crest a metre or more off.
// --sea-report prints how far off, beside the miss this leaves.
//
// Newton rather than the plain fixed point: the throw's Jacobian has norm up
// to the steepness, 0.85 at full storm, so the fixed point would gain barely
// a bit per step exactly when the answer matters most. The determinant stays
// at least (1 - steepness)^2 > 0 because the steepness is capped below 1 --
// the same cap that keeps the drawn surface from folding through itself.
//
// Depth is taken as constant over the throw (a few metres): the shader reads
// it at the rest point, physics at the asked point, and on a beach sloping
// 1 in 20 that is a few centimetres of depth.
WW_FN WW_F2 ww_rest_point(float x, float z, float t, WW_IN_SEA sea, float depth,
                          float min_wavelength) {
    const WW_F3 loc = ww_local(sea, depth, 1.0f, min_wavelength);
    const float scale = loc.x;
    const float qc = loc.y;
    float rx = x, rz = z;
    for (int it = 0; it < WW_INVERT_STEPS; it = it + 1) {
        float px = rx, pz = rz;
        float jxx = 1.0f, jxz = 0.0f, jzz = 1.0f;   // symmetric
        for (int i = 0; i < WW_WAVES; i = i + 1) {
            const float a = ww_raw_amp(sea, i, depth, 1.0f, min_wavelength) * scale;
            if (a <= 0.0f) continue;
            const WW_F2 d = ww_direction(i);
            const float k = 6.2831853f / ww_wavelength(i);
            const float phase = k * (d.x * rx + d.y * rz) - ww_angular_speed(k) * t;
            const float s = ww_sin(phase);
            const float c = ww_cos(phase);
            px = px + qc * a * d.x * c;
            pz = pz + qc * a * d.y * c;
            const float g = qc * a * k * s;
            jxx = jxx - g * d.x * d.x;
            jxz = jxz - g * d.x * d.y;
            jzz = jzz - g * d.y * d.y;
        }
        const float ex = px - x;
        const float ez = pz - z;
        const float det = jxx * jzz - jxz * jxz;
        const float inv = det > 1e-6f ? 1.0f / det : 1.0f;
        rx = rx - (jzz * ex - jxz * ez) * inv;
        rz = rz - (jxx * ez - jxz * ex) * inv;
    }
    WW_F2 r;
    r.x = rx; r.y = rz;
    return r;
}

// Height of the sea surface directly above (x, z), relative to sea level, at
// full detail and no distance fade -- which is what the drawn sea carries
// within a few metres of the camera, where the grid is finer than the
// shortest wave by an order of magnitude.
WW_FN float ww_height_at(float x, float z, float t, WW_IN_SEA sea, float depth) {
    const WW_F2 r = ww_rest_point(x, z, t, sea, depth, 0.0f);
    WW_F3 n;
    const WW_F3 p = ww_surface(r.x, r.y, t, sea, depth, 1.0f, 0.0f, n);
    return p.y;
}

// --- the swash: water running up the beach and back --------------------------
//
// The trains die over the shoal (ww_depth_factor), so on their own the sea's
// edge sat at the still-water line in every weather, and a gale's coast had
// the same dry sand to the waterline as a calm day's. Real waves run on up the
// beach after they break -- the uprush -- and fall back, a sheet of water
// rising and sinking with each arriving wave.
//
// This is that sheet's height over sea level: up to `runup` (from the sea's
// significant height, Renderer::update_weather), riding up as a long wave's
// crest arrives and back as its trough does -- the eight longest trains'
// elevation at the point, over their own rms, so the uprush comes in sets and
// varies along the shore as the swell does. ww_swash_reach says how much of it
// a point gets by its depth: all of it at and above the still-water line, none
// past a metre and a half of water, where the trains carry the sea.
//
// One function for the sheet the sea draws (water.hlsl), the wet sand the
// terrain shades under it (mesh.hlsl) and the water the swimmer wades in
// (physics_world.cpp): three copies of a shore would disagree about where it
// is.
#define WW_SWASH_TRAINS 8
struct WwSwash {
    float amp[WW_SWASH_TRAINS];   // the weather's deep-water amplitudes of trains 0..7
    float runup;                  // the highest the sheet runs, metres over sea level
};

WW_FN float ww_swash(float x, float z, float t, WW_IN_SWASH sw) {
    float e = 0.0f, var = 0.0f;
    for (int i = 0; i < WW_SWASH_TRAINS; i = i + 1) {
        const float a = sw.amp[i];
        if (a <= 0.0f) continue;
        const WW_F2 d = ww_direction(i);
        const float k = 6.2831853f / ww_wavelength(i);
        e = e + a * ww_sin(k * (d.x * x + d.y * z) - ww_angular_speed(k) * t);
        var = var + 0.5f * a * a;
    }
    const float ez = var > 1e-12f ? e / ww_sqrt(var) : 0.0f;
    // Lobes along the shore, ~7 m: a sheet that ran up to one contour drew a
    // straight-edged slab; real uprush comes in tongues (and cusps the beach).
    const float lobe = 0.75f + 0.5f * wind_vnoise2(x * (1.0f / 7.0f), z * (1.0f / 7.0f));
    return sw.runup * lobe * ww_saturate(0.5f + 0.35f * ez);
}

WW_FN float ww_swash_reach(float depth) {
    return ww_saturate(1.0f - depth / 1.5f);
}

// The water's height over sea level at (x, z) where it RESTS over `depth` of
// water (negative on land), swash included, and never below the bed: in a
// storm a trough over half a metre of water fell under the seabed and bared
// it. What physics floats and wades on.
WW_FN float ww_water_at(float x, float z, float t, WW_IN_SEA sea, WW_IN_SWASH sw, float depth) {
    const float d = depth > 0.0f ? depth : 0.0f;
    float h = ww_height_at(x, z, t, sea, d) + ww_swash(x, z, t, sw) * ww_swash_reach(depth);
    const float bed = 0.03f - depth;
    if (h < bed) h = bed;
    return h;
}

#endif  // ENGINE_WATER_WAVE_H
