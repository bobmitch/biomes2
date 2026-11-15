#include "engine/render/sky.h"
#include "engine/render/lighting_units.h"
#include "engine/render/atmosphere.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {

// The tables the march samples, bilinear with texel centres at (i + 0.5) / n,
// exactly as the GPU samples the uploaded copies.
const float* g_trans = nullptr;
const float* g_ms = nullptr;

atm_f3 sample_table(const float* t, int w, int h, float u, float v) {
    const float x = atm_clamp(u * float(w) - 0.5f, 0.0f, float(w - 1));
    const float y = atm_clamp(v * float(h) - 0.5f, 0.0f, float(h - 1));
    const int x0 = int(x), y0 = int(y);
    const int x1 = x0 + 1 < w ? x0 + 1 : x0, y1 = y0 + 1 < h ? y0 + 1 : y0;
    const float fx = x - float(x0), fy = y - float(y0);
    auto at = [&](int i, int j) {
        const float* p = t + (size_t(j) * w + i) * 4;
        return ATM3(p[0], p[1], p[2]);
    };
    return (at(x0, y0) * (1.0f - fx) + at(x1, y0) * fx) * (1.0f - fy) +
           (at(x0, y1) * (1.0f - fx) + at(x1, y1) * fx) * fy;
}

}  // namespace

atm_f3 atm_sample_transmittance(float r, float mu) {
    float u, v;
    atm_trans_uv(r, mu, u, v);
    return sample_table(g_trans, kAtmTransW, kAtmTransH, u, v);
}

atm_f3 atm_sample_ms(float r, float mu_s) {
    if (!g_ms) return ATM3(0.0f, 0.0f, 0.0f);   // while the table itself is being built
    const float u = atm_saturate(mu_s * 0.5f + 0.5f);
    const float v = atm_saturate((r - kAtmGround) / (kAtmTop - kAtmGround));
    return sample_table(g_ms, kAtmMsSize, kAtmMsSize, u, v);
}

#include "engine/render/atmosphere_march.h"

namespace engine {

namespace {

const float kDeg = 3.14159265f / 180.0f;
// Extraterrestrial illuminance: the IES value for the sun, a full moon's for
// the moon, each in linear sRGB with a luminance of 1 per lux.
const float kSunTop[3] = {127500.0f * 1.018f, 127500.0f * 0.998f, 127500.0f * 0.967f};
const float kMoonTop[3] = {0.27f * 1.05f, 0.27f * 1.0f, 0.27f * 0.92f};
// Angular radii, for the disks' solid angles.
const float kSunRadius = 0.2667f * kDeg;
const float kMoonRadius = 0.259f * kDeg;

float luminance(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
float luminance(atm_f3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

AtmLights lights_of(const SkyFrame& f) {
    AtmLights l;
    l.dir0 = ATM3(f.sun_dir[0], f.sun_dir[1], f.sun_dir[2]);
    l.lux0 = ATM3(f.sun_lux_top[0], f.sun_lux_top[1], f.sun_lux_top[2]);
    l.dir1 = ATM3(f.moon_dir[0], f.moon_dir[1], f.moon_dir[2]);
    l.lux1 = ATM3(f.moon_lux_top[0], f.moon_lux_top[1], f.moon_lux_top[2]);
    return l;
}

void put(float out[3], atm_f3 v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

}  // namespace

void Sky::build() {
    g_trans = nullptr;
    g_ms = nullptr;
    trans_.assign(size_t(kAtmTransW) * kAtmTransH * 4, 0.0f);
    for (int j = 0; j < kAtmTransH; ++j) {
        for (int i = 0; i < kAtmTransW; ++i) {
            float r, mu;
            atm_trans_rmu((float(i) + 0.5f) / kAtmTransW, (float(j) + 0.5f) / kAtmTransH, r, mu);
            const atm_f3 t = atm_transmittance_integrate(r, mu);
            float* p = &trans_[(size_t(j) * kAtmTransW + i) * 4];
            p[0] = t.x; p[1] = t.y; p[2] = t.z; p[3] = 1.0f;
        }
    }
    g_trans = trans_.data();
    std::vector<float> ms(size_t(kAtmMsSize) * kAtmMsSize * 4, 0.0f);
    for (int j = 0; j < kAtmMsSize; ++j) {
        for (int i = 0; i < kAtmMsSize; ++i) {
            float r, mu_s;
            atm_ms_rmu((float(i) + 0.5f) / kAtmMsSize, (float(j) + 0.5f) / kAtmMsSize, r, mu_s);
            const atm_f3 m = atm_ms_texel(r, mu_s);
            float* p = &ms[(size_t(j) * kAtmMsSize + i) * 4];
            p[0] = m.x; p[1] = m.y; p[2] = m.z; p[3] = 1.0f;
        }
    }
    ms_.swap(ms);
    g_ms = ms_.data();
}

void Sky::light_dirs(float hours, float sun[3], float moon[3]) {
    // A sun at latitude 40 N on a day of declination +15. World +x is west and
    // +z south, which puts 14:00's sun where the fixed L1 sun was (0.466,
    // 0.807, 0.362) to within a degree, so a still at the default hour is the
    // scene everything before L2 was tuned on.
    const float lat = 40.0f * kDeg, dec = 15.0f * kDeg;
    const float h = (hours - 12.0f) * 15.0f * kDeg;
    const float east = -std::cos(dec) * std::sin(h);
    const float north = std::sin(dec) * std::cos(lat) - std::cos(dec) * std::sin(lat) * std::cos(h);
    const float up = std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(h);
    const float l = std::sqrt(east * east + north * north + up * up);
    sun[0] = -east / l; sun[1] = up / l; sun[2] = -north / l;
    // A full moon, opposite the sun: the simplest moon that lights the night.
    for (int k = 0; k < 3; ++k) moon[k] = -sun[k];
}

void Sky::radiance(const SkyFrame& f, float alt_km, const float dir[3], float out[3], int steps) const {
    const atm_f3 o = ATM3(0.0f, kAtmGround + alt_km, 0.0f);
    const AtmState s = atm_march(o, atm_normalize(ATM3(dir[0], dir[1], dir[2])), steps, lights_of(f), false);
    put(out, s.L);
}

void Sky::sky_texel_dir(int x, int y, float alt_km, float out[3]) {
    put(out, atm_sky_dir((float(x) + 0.5f) / kAtmSkyW, (float(y) + 0.5f) / kAtmSkyH, kAtmGround + alt_km));
}

SkyFrame Sky::evaluate(float hours, float cam_y) const {
    SkyFrame f;
    f.hours = hours;
    light_dirs(hours, f.sun_dir, f.moon_dir);
    std::memcpy(f.sun_lux_top, kSunTop, sizeof kSunTop);
    std::memcpy(f.moon_lux_top, kMoonTop, sizeof kMoonTop);
    f.sun_elevation_deg = std::asin(atm_clamp(f.sun_dir[1], -1.0f, 1.0f)) / kDeg;
    f.cam_alt_km = atm_max(cam_y, 0.0f) / 1000.0f + 0.002f;
    if (!built()) return f;
    const float r = kAtmGround + f.cam_alt_km;

    // Each light at the ground, through the atmosphere and past the limb.
    atm_f3 at_ground[2];
    const float* dirs[2] = {f.sun_dir, f.moon_dir};
    const float* tops[2] = {kSunTop, kMoonTop};
    for (int k = 0; k < 2; ++k) {
        const float mu = dirs[k][1];
        at_ground[k] = atm_sample_transmittance(r, mu) * atm_planet_visibility(r, mu) *
                       ATM3(tops[k][0], tops[k][1], tops[k][2]);
    }
    f.key_is_moon = luminance(at_ground[1]) > luminance(at_ground[0]);
    const int key = f.key_is_moon ? 1 : 0;
    std::memcpy(f.key_dir, dirs[key], sizeof f.key_dir);
    put(f.key_lux, at_ground[key]);
    const float sun_omega = 2.0f * kAtmPi * (1.0f - std::cos(kSunRadius));
    const float moon_omega = 2.0f * kAtmPi * (1.0f - std::cos(kMoonRadius));
    put(f.sun_disk, at_ground[0] / sun_omega);
    put(f.moon_disk, at_ground[1] / moon_omega);

    // The sky's irradiance on level ground: the upper hemisphere, integrated
    // against the cosine in 6 x 12 cells, each a 16-step march.
    const AtmLights lights = lights_of(f);
    const atm_f3 o = ATM3(0.0f, r, 0.0f);
    atm_f3 E = ATM3(0.0f, 0.0f, 0.0f);
    const int nz = 6, na = 12;
    for (int j = 0; j < nz; ++j) {
        const float t0 = 0.5f * kAtmPi * float(j) / nz, t1 = 0.5f * kAtmPi * float(j + 1) / nz;
        const float th = 0.5f * (t0 + t1);
        // The cell's cosine-weighted solid angle, exactly: integral of cos sin dtheta.
        const float w = 0.5f * (std::sin(t1) * std::sin(t1) - std::sin(t0) * std::sin(t0)) * (2.0f * kAtmPi / na);
        for (int i = 0; i < na; ++i) {
            const float az = 2.0f * kAtmPi * (float(i) + 0.5f) / na;
            const atm_f3 d = ATM3(std::sin(th) * std::cos(az), std::cos(th), std::sin(th) * std::sin(az));
            E += atm_march(o, d, 16, lights, false).L * w;
        }
    }
    put(f.sky_mean, E / kAtmPi);
    // The ground the lower hemisphere sees: the bounce albedo of
    // lighting_units.h under both lights and that sky.
    atm_f3 lit = E;
    for (int k = 0; k < 2; ++k) lit += at_ground[k] * atm_max(dirs[k][1], 0.0f);
    put(f.ground, ATM3(kGroundBounceR, kGroundBounceG, kGroundBounceB) * lit / kAtmPi);

    // Three samples of the sky for the scene's reflections: the zenith, and
    // the horizon (3 degrees up) under the key light's azimuth and opposite.
    const float up[3] = {0.0f, 1.0f, 0.0f};
    radiance(f, f.cam_alt_km, up, f.zenith);
    float hx = f.key_dir[0], hz = f.key_dir[2];
    const float hl = std::sqrt(hx * hx + hz * hz);
    if (hl < 1e-4f) { hx = 1.0f; hz = 0.0f; } else { hx /= hl; hz /= hl; }
    const float ce = std::cos(3.0f * kDeg), se = std::sin(3.0f * kDeg);
    const float toward[3] = {hx * ce, se, hz * ce}, away[3] = {-hx * ce, se, -hz * ce};
    radiance(f, f.cam_alt_km, toward, f.horizon_sun);
    radiance(f, f.cam_alt_km, away, f.horizon_away);
    return f;
}

// ---------------------------------------------------------------------------

bool write_sky_report(const char* path, std::string& error) {
    Sky sky;
    sky.build();
    FILE* f = std::fopen(path, "w");
    if (!f) { error = std::string("could not write ") + path; return false; }
    std::fprintf(f, "sky report (ADR-004 L2): Hillaire 2020 on Bruneton's Earth, at sea level\n\n");
    std::fprintf(f, "References, clear sky (IES Lighting Handbook):\n"
                    "  direct normal   E_dn = 127.5 exp(-0.21 m) klux, m the Kasten-Young air mass\n"
                    "  diffuse horiz.  E_kh = 0.8 + 15.5 sin(e)^0.5 klux\n"
                    "The IES 'clear' sky is hazier than this model's standard Mie: expect the direct\n"
                    "beam a little above it and the diffuse sky a little below.\n\n");
    std::fprintf(f, "  elev | direct normal klux  (ref, ratio) | sky on level ground klux (ref, ratio) | zenith nits | horizon toward / away nits | sun colour r:g:b\n");
    const float elevs[] = {90, 60, 45, 30, 20, 10, 5, 2, 0, -2, -6};
    float worst_dn = 0.0f, worst_kh = 0.0f;
    for (float e : elevs) {
        SkyFrame fr;
        fr.sun_dir[0] = std::cos(e * kDeg); fr.sun_dir[1] = std::sin(e * kDeg); fr.sun_dir[2] = 0.0f;
        for (int k = 0; k < 3; ++k) { fr.moon_dir[k] = -fr.sun_dir[k]; fr.sun_lux_top[k] = kSunTop[k]; fr.moon_lux_top[k] = 0.0f; }
        const float r = kAtmGround + 0.002f;
        const atm_f3 sun = atm_sample_transmittance(r, fr.sun_dir[1]) * atm_planet_visibility(r, fr.sun_dir[1]) *
                           ATM3(kSunTop[0], kSunTop[1], kSunTop[2]);
        // The sky on level ground, as evaluate() integrates it.
        const AtmLights lights = lights_of(fr);
        const atm_f3 o = ATM3(0.0f, r, 0.0f);
        atm_f3 E = ATM3(0.0f, 0.0f, 0.0f);
        for (int j = 0; j < 12; ++j) {
            const float t0 = 0.5f * kAtmPi * j / 12, t1 = 0.5f * kAtmPi * (j + 1) / 12, th = 0.5f * (t0 + t1);
            const float w = 0.5f * (std::sin(t1) * std::sin(t1) - std::sin(t0) * std::sin(t0)) * (2.0f * kAtmPi / 24);
            for (int i = 0; i < 24; ++i) {
                const float az = 2.0f * kAtmPi * (i + 0.5f) / 24;
                E += atm_march(o, ATM3(std::sin(th) * std::cos(az), std::cos(th), std::sin(th) * std::sin(az)),
                               32, lights, false).L * w;
            }
        }
        float zen[3], tow[3], awy[3];
        const float up[3] = {0, 1, 0};
        const float ce = std::cos(3.0f * kDeg), se = std::sin(3.0f * kDeg);
        const float td[3] = {ce, se, 0}, ad[3] = {-ce, se, 0};
        sky.radiance(fr, 0.002f, up, zen);
        sky.radiance(fr, 0.002f, td, tow);
        sky.radiance(fr, 0.002f, ad, awy);
        const float dn = luminance(sun) / 1000.0f, kh = luminance(E) / 1000.0f;
        char ref_dn[64] = "      -      ", ref_kh[64] = "      -      ";
        if (e > 0.0f) {
            const float m = 1.0f / (std::sin(e * kDeg) + 0.50572f * std::pow(e + 6.07995f, -1.6364f));
            const float rdn = 127.5f * std::exp(-0.21f * m);
            const float rkh = 0.8f + 15.5f * std::sqrt(std::sin(e * kDeg));
            std::snprintf(ref_dn, sizeof ref_dn, "%6.1f, %4.2f", rdn, dn / rdn);
            std::snprintf(ref_kh, sizeof ref_kh, "%6.1f, %4.2f", rkh, kh / rkh);
            if (e >= 10.0f) {
                worst_dn = atm_max(worst_dn, atm_abs(std::log(dn / rdn)));
                worst_kh = atm_max(worst_kh, atm_abs(std::log(kh / rkh)));
            }
        }
        const float sl = atm_max(sun.y, 1e-12f);
        std::fprintf(f, "  %4.0f | %8.2f  (%s)         | %8.2f  (%s)               | %10.1f | %10.1f / %10.1f      | %.2f:1:%.2f\n",
                     e, dn, ref_dn, kh, ref_kh, luminance(ATM3(zen[0], zen[1], zen[2])),
                     luminance(ATM3(tow[0], tow[1], tow[2])), luminance(ATM3(awy[0], awy[1], awy[2])),
                     sun.x / sl, sun.z / sl);
    }
    std::fprintf(f, "\nworst ratio to the references for the sun at 10 degrees and up: direct x%.2f, diffuse x%.2f\n",
                 std::exp(worst_dn), std::exp(worst_kh));

    std::fprintf(f, "\nThe day as the engine runs it (latitude 40 N, declination +15), camera at sea level:\n");
    std::fprintf(f, "  hour | sun elev | key light | key on ground lux | sky on level ground lux | zenith nits\n");
    for (int hr = 0; hr <= 24; hr += 2) {
        const SkyFrame fr = sky.evaluate(float(hr), 0.0f);
        std::fprintf(f, "  %4d | %8.1f | %-9s | %17.4g | %23.4g | %11.4g\n", hr, fr.sun_elevation_deg,
                     fr.key_is_moon ? "moon" : "sun", luminance(fr.key_lux), kAtmPi * luminance(fr.sky_mean),
                     luminance(fr.zenith));
    }
    // What a time-lapse pays: evaluate() runs every frame while the hour moves.
    {
        const auto t0 = std::chrono::steady_clock::now();
        const int n = 200;
        float sink = 0.0f;
        for (int i = 0; i < n; ++i) sink += sky.evaluate(6.0f + 12.0f * float(i) / n, 0.0f).key_lux[1];
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
        std::fprintf(f, "\nevaluate(): %.3f ms a call on the CPU (%g)\n", ms, double(sink) * 0.0);
    }
    std::fclose(f);
    return true;
}

}  // namespace engine
