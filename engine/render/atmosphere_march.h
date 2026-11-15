// ---------------------------------------------------------------------------
// Integrating light along a ray through the atmosphere (ADR-004 L2), shared
// by the GPU (the sky-view table and the aerial-perspective volume) and the
// CPU (the multiple-scattering table, the sun and sky values the scene
// shaders light with, and --sky-report).
//
// Include atmosphere.h first, then define how the two static tables are
// sampled, then include this:
//
//   ATM_F3 atm_sample_transmittance(float r, float mu);  // to the top
//   ATM_F3 atm_sample_ms(float r, float mu_s);           // Hillaire's psi_ms
//
// The C++ side samples arrays; the HLSL side samples textures. The tables
// themselves are built on the CPU and uploaded, so there is one builder.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDER_ATMOSPHERE_MARCH_H
#define ENGINE_RENDER_ATMOSPHERE_MARCH_H

// Up to two lights: the sun and the moon. A light with zero illuminance costs
// a few multiplies.
struct AtmLights {
    ATM_F3 dir0; ATM_F3 lux0;   // towards the light; illuminance at the top of the atmosphere
    ATM_F3 dir1; ATM_F3 lux1;
};

struct AtmState {
    ATM_F3 L;      // in-scattered radiance so far
    ATM_F3 T;      // transmittance so far
    ATM_F3 fms;    // the multiple-scattering table's transfer term (its build only)
};

ATM_FN AtmState atm_state() {
    AtmState s;
    s.L = ATM3(0.0f, 0.0f, 0.0f);
    s.T = ATM3(1.0f, 1.0f, 1.0f);
    s.fms = ATM3(0.0f, 0.0f, 0.0f);
    return s;
}

// What one light scatters towards -d at point p. `ms_build` uses the
// isotropic phase and no multiple-scattering term, which is what building the
// multiple-scattering table needs.
ATM_FN ATM_F3 atm_light_source(ATM_F3 p, float r, ATM_F3 d, AtmMedium m, ATM_F3 ldir, ATM_F3 lux,
                               bool ms_build) {
    const ATM_F3 up = p / r;
    const float mu_l = atm_dot(up, ldir);
    const ATM_F3 Tl = atm_sample_transmittance(r, mu_l) * atm_planet_visibility(r, mu_l);
    if (ms_build) {
        const float iso = 1.0f / (4.0f * kAtmPi);
        return lux * Tl * (m.scat_r + ATM3(m.scat_m, m.scat_m, m.scat_m)) * iso;
    }
    const float c = atm_dot(d, ldir);
    const ATM_F3 single = Tl * (m.scat_r * atm_phase_rayleigh(c) + ATM3(1.0f, 1.0f, 1.0f) * (m.scat_m * atm_phase_mie(c)));
    const ATM_F3 multi = atm_sample_ms(r, mu_l) * (m.scat_r + ATM3(m.scat_m, m.scat_m, m.scat_m));
    return lux * (single + multi);
}

// Integrate from t0 to t1 along o + t d in `steps` steps, continuing `s`.
// Hillaire's energy-conserving form: each step's source is integrated
// against the step's own extinction rather than taken as a point sample.
ATM_FN AtmState atm_integrate(AtmState s, ATM_F3 o, ATM_F3 d, float t0, float t1, int steps,
                              AtmLights lights, bool ms_build) {
    const float dt = (t1 - t0) / float(steps);
    for (int i = 0; i < steps; ++i) {
        const float t = t0 + (float(i) + 0.3f) * dt;
        const ATM_F3 p = o + d * t;
        const float r = atm_length(p);
        const AtmMedium m = atm_medium(atm_max(r - kAtmGround, 0.0f));
        ATM_F3 S = atm_light_source(p, r, d, m, lights.dir0, lights.lux0, ms_build) +
                   atm_light_source(p, r, d, m, lights.dir1, lights.lux1, ms_build);
        const ATM_F3 ext = atm_max3(m.ext, 1e-7f);
        const ATM_F3 Ts = atm_exp3(-ext * dt);
        s.L += s.T * (S - S * Ts) / ext;
        if (ms_build) {
            const ATM_F3 ms = m.scat_r + ATM3(m.scat_m, m.scat_m, m.scat_m);
            s.fms += s.T * (ms - ms * Ts) / ext;
        }
        s.T *= Ts;
    }
    return s;
}

// Light the ground reflects back along -d where the ray ends on it.
ATM_FN ATM_F3 atm_ground_bounce(ATM_F3 p, ATM_F3 ldir, ATM_F3 lux) {
    const ATM_F3 up = atm_normalize(p);
    const float nl = atm_saturate(atm_dot(up, ldir));
    const ATM_F3 Tl = atm_sample_transmittance(kAtmGround, nl) * atm_planet_visibility(kAtmGround, nl);
    return lux * Tl * (nl * kAtmGroundAlbedo / kAtmPi);
}

// The whole ray from o along d: to the ground or out of the atmosphere.
ATM_FN AtmState atm_march(ATM_F3 o, ATM_F3 d, int steps, AtmLights lights, bool ms_build) {
    const float t_ground = atm_ray_sphere(o, d, kAtmGround);
    const float t_top = atm_ray_sphere(o, d, kAtmTop);
    float t_max = t_top;
    if (t_ground >= 0.0f) t_max = t_ground;
    AtmState s = atm_state();
    if (t_max <= 0.0f) return s;
    s = atm_integrate(s, o, d, 0.0f, t_max, steps, lights, ms_build);
    if (t_ground >= 0.0f) {
        const ATM_F3 p = o + d * t_ground;
        s.L += s.T * (atm_ground_bounce(p, lights.dir0, lights.lux0) + atm_ground_bounce(p, lights.dir1, lights.lux1));
    }
    return s;
}

// One texel of the multiple-scattering table (Hillaire's section 5.5): the
// second-order light arriving at (r, mu_s) from a unit sun, from 64
// directions round the sphere, and the fraction f_ms that each further order
// passes on; the infinite series sums to L2 / (1 - f_ms).
ATM_FN ATM_F3 atm_ms_texel(float r, float mu_s) {
    const ATM_F3 o = ATM3(0.0f, r, 0.0f);
    AtmLights lights;
    lights.dir0 = ATM3(atm_sqrt(atm_max(0.0f, 1.0f - mu_s * mu_s)), mu_s, 0.0f);
    lights.lux0 = ATM3(1.0f, 1.0f, 1.0f);
    lights.dir1 = ATM3(0.0f, 1.0f, 0.0f);
    lights.lux1 = ATM3(0.0f, 0.0f, 0.0f);
    ATM_F3 L2 = ATM3(0.0f, 0.0f, 0.0f), fms = ATM3(0.0f, 0.0f, 0.0f);
    const int n = 8;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const float theta = 2.0f * kAtmPi * (float(i) + 0.5f) / float(n);
            const float phi = atm_acos(1.0f - 2.0f * (float(j) + 0.5f) / float(n));
            const ATM_F3 d = ATM3(atm_cos(theta) * atm_sin(phi), atm_cos(phi), atm_sin(theta) * atm_sin(phi));
            const AtmState s = atm_march(o, d, 20, lights, true);
            L2 += s.L;
            fms += s.fms;
        }
    }
    // Average over the sphere (each direction stands for 4 pi / 64 sr), then
    // the isotropic phase back towards the point.
    const float w = 1.0f / float(n * n);
    L2 = L2 * w;
    fms = fms * (w * 4.0f * kAtmPi / (4.0f * kAtmPi));
    const ATM_F3 one = ATM3(1.0f, 1.0f, 1.0f);
    return L2 / (one - fms);
}

#endif
