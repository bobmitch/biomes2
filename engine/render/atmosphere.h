// ---------------------------------------------------------------------------
// The atmosphere (ADR-004 L2): Hillaire 2020, "A Scalable and Production
// Ready Sky and Atmosphere Rendering Technique", on Bruneton's Earth.
//
// Compiled BOTH as C++ and as HLSL, the trick water_wave.h uses and for the
// same reason: --sky-report measures the functions the shaders draw, and the
// per-frame sun and sky values the scene shaders light with are computed on
// the CPU from the same text the GPU builds its sky from.
//
// This file is the medium and the geometry: densities, coefficients, phase
// functions, ray-sphere tests and the lookup-table parameterisations.
// atmosphere_march.h integrates along a ray and needs the two static tables;
// each side defines how it samples them before including it.
//
// Units: kilometres for the atmosphere, planet-centred, +y up. The camera
// sits on top of the planet: the world is 20 km across and the planet 6360
// km in radius, so where on the disc you stand changes nothing in the sky.
// Radiance in nits, illuminance in lux, as lighting_units.h.
//
// Presentation only. No hash, no save; invariant 9 does not reach it.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDER_ATMOSPHERE_H
#define ENGINE_RENDER_ATMOSPHERE_H

#ifdef __cplusplus
#include <cmath>
#define ATM_FN inline
struct atm_f3 { float x, y, z; };
inline atm_f3 ATM3(float x, float y, float z) { atm_f3 r; r.x = x; r.y = y; r.z = z; return r; }
inline atm_f3 operator+(atm_f3 a, atm_f3 b) { return ATM3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline atm_f3 operator-(atm_f3 a, atm_f3 b) { return ATM3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline atm_f3 operator-(atm_f3 a) { return ATM3(-a.x, -a.y, -a.z); }
inline atm_f3 operator*(atm_f3 a, atm_f3 b) { return ATM3(a.x * b.x, a.y * b.y, a.z * b.z); }
inline atm_f3 operator*(atm_f3 a, float s) { return ATM3(a.x * s, a.y * s, a.z * s); }
inline atm_f3 operator*(float s, atm_f3 a) { return ATM3(a.x * s, a.y * s, a.z * s); }
inline atm_f3 operator/(atm_f3 a, atm_f3 b) { return ATM3(a.x / b.x, a.y / b.y, a.z / b.z); }
inline atm_f3 operator/(atm_f3 a, float s) { return ATM3(a.x / s, a.y / s, a.z / s); }
inline atm_f3& operator+=(atm_f3& a, atm_f3 b) { a = a + b; return a; }
inline atm_f3& operator*=(atm_f3& a, atm_f3 b) { a = a * b; return a; }
inline float atm_dot(atm_f3 a, atm_f3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float atm_length(atm_f3 a) { return std::sqrt(atm_dot(a, a)); }
inline atm_f3 atm_normalize(atm_f3 a) { return a / atm_length(a); }
inline atm_f3 atm_exp3(atm_f3 a) { return ATM3(std::exp(a.x), std::exp(a.y), std::exp(a.z)); }
inline atm_f3 atm_max3(atm_f3 a, float b) {
    return ATM3(a.x > b ? a.x : b, a.y > b ? a.y : b, a.z > b ? a.z : b);
}
inline float atm_exp(float a) { return std::exp(a); }
inline float atm_sqrt(float a) { return std::sqrt(a); }
inline float atm_max(float a, float b) { return a > b ? a : b; }
inline float atm_min(float a, float b) { return a < b ? a : b; }
inline float atm_clamp(float a, float lo, float hi) { return a < lo ? lo : (a > hi ? hi : a); }
inline float atm_saturate(float a) { return atm_clamp(a, 0.0f, 1.0f); }
inline float atm_abs(float a) { return a < 0.0f ? -a : a; }
inline float atm_acos(float a) { return std::acos(atm_clamp(a, -1.0f, 1.0f)); }
inline float atm_cos(float a) { return std::cos(a); }
inline float atm_sin(float a) { return std::sin(a); }
inline float atm_atan2(float y, float x) { return std::atan2(y, x); }
inline float atm_pow(float a, float b) { return std::pow(a, b); }
#define ATM_F3 atm_f3
#define ATM_OUT_FLOAT float&
#define ATM_OUT_F3 atm_f3&
#else
#define ATM_FN
#define ATM_F3 float3
#define ATM3(x, y, z) float3(x, y, z)
#define ATM_OUT_FLOAT out float
#define ATM_OUT_F3 out float3
#define atm_dot dot
#define atm_length length
#define atm_normalize normalize
#define atm_exp3 exp
#define atm_max3 max
#define atm_exp exp
#define atm_sqrt sqrt
#define atm_max max
#define atm_min min
#define atm_clamp clamp
#define atm_saturate saturate
#define atm_abs abs
#define atm_acos(a) acos(clamp(a, -1.0f, 1.0f))
#define atm_cos cos
#define atm_sin sin
#define atm_atan2 atan2
#define atm_pow pow
#endif

static const float kAtmPi = 3.14159265f;

// --- the medium: Bruneton's Earth, as Hillaire ships it, but with a real
// day's aerosol. The shipped Mie (3.996e-3 per km over a 1.2 km scale
// height) is an optical depth of 0.005, cleaner than any real clear day
// (0.05-0.15), and --sky-report showed what that costs: the sky on level
// ground at HALF the IES clear-sky reference at every elevation, and the
// low sun's beam 1.6x to 8x too strong. Aerosol optical depth 0.09 then put
// the beam on the reference and the sky 1.27x over it; 0.073 (scattering
// 0.066), single-scattering albedo 0.9, splits the difference (the report's
// last line).
static const float kAtmGround = 6360.0f;   // km
static const float kAtmTop    = 6460.0f;
static const float kAtmRayleighHeight = 8.0f;
static const float kAtmMieHeight      = 1.2f;
static const float kAtmMieScattering  = 0.055f;      // per km
static const float kAtmMieExtinction  = 0.0611f;
static const float kAtmMieG           = 0.8f;
// What the planet's surface reflects, for light bounced up into the sky and
// for the multiple-scattering table. Grass and soil, darker than snow.
static const float kAtmGroundAlbedo   = 0.1f;

ATM_FN ATM_F3 atm_rayleigh_scattering() { return ATM3(5.802e-3f, 13.558e-3f, 33.1e-3f); }
ATM_FN ATM_F3 atm_ozone_absorption()    { return ATM3(0.650e-3f, 1.881e-3f, 0.085e-3f); }

struct AtmMedium {
    ATM_F3 scat_r;    // Rayleigh scattering
    float  scat_m;    // Mie scattering (grey)
    ATM_F3 ext;       // total extinction
};

// The medium at an altitude in km above the ground.
ATM_FN AtmMedium atm_medium(float h) {
    const float dr = atm_exp(-h / kAtmRayleighHeight);
    const float dm = atm_exp(-h / kAtmMieHeight);
    // Ozone: a tent 30 km wide centred at 25 km.
    const float doz = atm_max(0.0f, 1.0f - atm_abs(h - 25.0f) / 15.0f);
    AtmMedium m;
    m.scat_r = atm_rayleigh_scattering() * dr;
    m.scat_m = kAtmMieScattering * dm;
    m.ext = m.scat_r + ATM3(1.0f, 1.0f, 1.0f) * (kAtmMieExtinction * dm) + atm_ozone_absorption() * doz;
    return m;
}

ATM_FN float atm_phase_rayleigh(float c) {
    return 3.0f / (16.0f * kAtmPi) * (1.0f + c * c);
}

// Cornette-Shanks, which Hillaire uses: Henyey-Greenstein with Rayleigh's
// (1 + c^2) and a normalisation for it.
ATM_FN float atm_phase_mie(float c) {
    const float g = kAtmMieG;
    const float k = 3.0f / (8.0f * kAtmPi) * (1.0f - g * g) / (2.0f + g * g);
    const float d = atm_max(1.0f + g * g - 2.0f * g * c, 1e-6f);
    return k * (1.0f + c * c) / (d * atm_sqrt(d));
}

// The nearest t >= 0 at which o + t d meets a sphere of radius R about the
// origin, or -1. `d` is unit.
ATM_FN float atm_ray_sphere(ATM_F3 o, ATM_F3 d, float R) {
    const float b = atm_dot(o, d);
    const float c = atm_dot(o, o) - R * R;
    const float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    const float s = atm_sqrt(disc);
    const float t0 = -b - s, t1 = -b + s;
    if (t0 >= 0.0f) return t0;
    if (t1 >= 0.0f) return t1;
    return -1.0f;
}

// How far a ray leaves the atmosphere, or meets the ground, from radius r at
// cos(zenith) mu.
ATM_FN float atm_distance_to_top(float r, float mu) {
    const float disc = r * r * (mu * mu - 1.0f) + kAtmTop * kAtmTop;
    return atm_max(0.0f, -r * mu + atm_sqrt(atm_max(disc, 0.0f)));
}

// The light's visibility past the planet's limb, softened over about half a
// degree so a sunset fades rather than switching off in one frame. 1 when the
// light is clear of the horizon at radius r.
ATM_FN float atm_planet_visibility(float r, float mu_l) {
    const float s = kAtmGround / r;
    const float mu_h = -atm_sqrt(atm_max(0.0f, 1.0f - s * s));
    return atm_saturate((mu_l - mu_h) / 0.0175f + 0.5f);
}

// --- the transmittance table: Bruneton's parameterisation -----------------
// u from the distance to the top of the atmosphere, v from the radius, so the
// resolution goes where the transmittance changes fastest: near the horizon.

static const int kAtmTransW = 256;
static const int kAtmTransH = 64;

ATM_FN void atm_trans_uv(float r, float mu, ATM_OUT_FLOAT u, ATM_OUT_FLOAT v) {
    const float H = atm_sqrt(kAtmTop * kAtmTop - kAtmGround * kAtmGround);
    const float rho = atm_sqrt(atm_max(0.0f, r * r - kAtmGround * kAtmGround));
    const float d = atm_distance_to_top(r, mu);
    const float d_min = kAtmTop - r, d_max = rho + H;
    u = (d - d_min) / atm_max(d_max - d_min, 1e-6f);
    v = rho / H;
}

ATM_FN void atm_trans_rmu(float u, float v, ATM_OUT_FLOAT r, ATM_OUT_FLOAT mu) {
    const float H = atm_sqrt(kAtmTop * kAtmTop - kAtmGround * kAtmGround);
    const float rho = H * v;
    r = atm_sqrt(rho * rho + kAtmGround * kAtmGround);
    const float d_min = kAtmTop - r, d_max = rho + H;
    const float d = d_min + u * (d_max - d_min);
    mu = d == 0.0f ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * r * d);
    mu = atm_clamp(mu, -1.0f, 1.0f);
}

// The transmittance from radius r to the top of the atmosphere along mu, by
// direct integration: what the table holds. 40 steps.
ATM_FN ATM_F3 atm_transmittance_integrate(float r, float mu) {
    const int steps = 40;
    const float d = atm_distance_to_top(r, mu);
    const float dt = d / float(steps);
    ATM_F3 depth = ATM3(0.0f, 0.0f, 0.0f);
    for (int i = 0; i < steps; ++i) {
        const float t = (float(i) + 0.5f) * dt;
        const float h = atm_sqrt(r * r + t * t + 2.0f * r * mu * t) - kAtmGround;
        depth += atm_medium(atm_max(h, 0.0f)).ext * dt;
    }
    return atm_exp3(-depth);
}

// --- the multiple-scattering table: (cos sun zenith, altitude) -------------
static const int kAtmMsSize = 32;

ATM_FN void atm_ms_rmu(float u, float v, ATM_OUT_FLOAT r, ATM_OUT_FLOAT mu_s) {
    mu_s = u * 2.0f - 1.0f;
    r = kAtmGround + atm_saturate(v) * (kAtmTop - kAtmGround);
}

// --- the sky-view table: the whole sky round the camera --------------------
// u is WORLD azimuth (not relative to the sun, as Hillaire's is), so one
// table holds the sun's sky and the moon's together. v is the zenith angle,
// split at the horizon -- which sits below 90 degrees by the planet's
// curvature -- with a square-root squeeze towards it, where the sky changes
// fastest.
static const int kAtmSkyW = 192;
static const int kAtmSkyH = 108;

ATM_FN ATM_F3 atm_sky_dir(float u, float v, float r) {
    const float cos_beta = atm_sqrt(atm_max(0.0f, r * r - kAtmGround * kAtmGround)) / r;
    const float beta = atm_acos(cos_beta);
    const float zenith_horizon = kAtmPi - beta;
    float zen;
    if (v < 0.5f) {
        float c = 1.0f - 2.0f * v;
        c = 1.0f - c * c;
        zen = zenith_horizon * c;
    } else {
        float c = v * 2.0f - 1.0f;
        zen = zenith_horizon + beta * c * c;
    }
    const float az = u * 2.0f * kAtmPi;
    const float s = atm_sin(zen);
    return ATM3(s * atm_cos(az), atm_cos(zen), s * atm_sin(az));
}

ATM_FN void atm_sky_uv(ATM_F3 dir, float r, ATM_OUT_FLOAT u, ATM_OUT_FLOAT v) {
    const float cos_beta = atm_sqrt(atm_max(0.0f, r * r - kAtmGround * kAtmGround)) / r;
    const float beta = atm_acos(cos_beta);
    const float zenith_horizon = kAtmPi - beta;
    const float zen = atm_acos(dir.y);
    if (zen < zenith_horizon) {
        float c = zen / zenith_horizon;
        c = 1.0f - atm_sqrt(atm_max(0.0f, 1.0f - c));
        v = c * 0.5f;
    } else {
        const float c = (zen - zenith_horizon) / atm_max(beta, 1e-6f);
        v = atm_sqrt(atm_max(0.0f, c)) * 0.5f + 0.5f;
    }
    float az = atm_atan2(dir.z, dir.x);
    if (az < 0.0f) az += 2.0f * kAtmPi;
    u = az / (2.0f * kAtmPi);
}

#endif
