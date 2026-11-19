#include "engine/render/weather.h"

#include "worldgen/assign/json_lite.h"
#include "worldgen/world.h"

#include <cmath>
#include <stdexcept>

namespace engine {

namespace {
const double kTwoPi = 6.283185307179586;
const float kG = 9.81f;

}  // namespace

bool Weather::load(const char* archetypes_path, std::string& error) {
    try {
        const jsonlite::Value root = jsonlite::parse_file(archetypes_path);
        if (!root.has("weather")) return true;
        const jsonlite::Value& w = root.at("weather");
        WeatherSettings& s = settings_;
        if (w.has("period_s")) s.period_s = float(w.number("period_s"));
        if (w.has("wind_min")) s.wind_min = float(w.number("wind_min"));
        if (w.has("wind_max")) s.wind_max = float(w.number("wind_max"));
        if (w.has("veer_period_s")) s.veer_period_s = float(w.number("veer_period_s"));
        if (w.has("veer")) s.veer = float(w.number("veer"));
        if (w.has("sea_lag_s")) s.sea_lag_s = float(w.number("sea_lag_s"));
        if (w.has("fetch_km")) s.fetch_m = float(w.number("fetch_km")) * 1000.0f;
        if (w.has("peak_shift")) s.peak_shift = float(w.number("peak_shift"));
        if (w.has("swell_max")) s.swell_max = float(w.number("swell_max"));
        if (w.has("swell_min")) s.swell_min = float(w.number("swell_min"));
        if (w.has("swell_wavelength")) s.swell_wavelength = float(w.number("swell_wavelength"));
        if (w.has("swell_lag_s")) s.swell_lag_s = float(w.number("swell_lag_s"));
        if (w.has("mist_drift")) s.mist_drift = float(w.number("mist_drift"));
    } catch (const std::exception& e) {
        error = std::string("archetypes.json weather: ") + e.what();
        return false;
    }
    return true;
}

// 1D value noise in [0, 1]: a hashed value at every integer, eased between
// them (C1). `stream` keeps the storm, the heading and the swell independent.
float Weather::noise(uint32_t stream, double x) const {
    const double fl = std::floor(x);
    const float f = float(x - fl);
    const int64_t i = int64_t(fl);
    const uint64_t base = (uint64_t(seed_) << 32) ^ (uint64_t(stream) * 0x9E3779B97F4A7C15ull);
    const float a = float(worldgen::hash64(base ^ uint64_t(i)) >> 40) / 16777216.0f;
    const float b = float(worldgen::hash64(base ^ uint64_t(i + 1)) >> 40) / 16777216.0f;
    const float e = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * e;
}

float Weather::storm_at(double t) const {
    if (storm_fixed_ >= 0.0f) return storm_fixed_ > 1.0f ? 1.0f : storm_fixed_;
    if (wind_fixed_ >= 0.0f) {
        const float r = (wind_fixed_ - settings_.wind_min) / (settings_.wind_max - settings_.wind_min);
        return std::sqrt(r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r));
    }
    const double p = settings_.period_s;
    // Two octaves, the second a third of the weight. Their sum bunches
    // towards the middle, so it is stretched back out (clamped: the ends
    // are calm and a full gale, and both should be reached), then bent a
    // little towards calm. --sea-report tabulates six hours of it.
    float n = (noise(1, t / p) + 0.35f * noise(2, t / (p * 0.37))) / 1.35f;
    n = (n - 0.5f) * 1.4f + 0.5f;
    n = n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
    return std::pow(n, 1.15f);
}

float Weather::speed_for(float storm) const {
    if (wind_fixed_ >= 0.0f && storm_fixed_ < 0.0f) return wind_fixed_;
    return settings_.wind_min + (settings_.wind_max - settings_.wind_min) * storm * storm;
}

float Weather::heading_at(double t) const {
    if (heading_set_) return heading_fixed_;
    const float base = float(worldgen::hash64(uint64_t(seed_) ^ 0x57494E44ull) >> 40) / 16777216.0f * float(kTwoPi);
    const float n = noise(3, t / settings_.veer_period_s) - 0.5f;
    return base + 2.0f * settings_.veer * n;
}

void Weather::wind_vector(double t, float& x, float& z) const {
    const float h = heading_at(t);
    const float u = speed_for(storm_at(t));
    x = std::cos(h) * u;
    z = std::sin(h) * u;
}

WeatherState Weather::at(double t) {
    WeatherState w;
    w.storm = storm_at(t);
    w.wind_speed = speed_for(w.storm);
    const float h = heading_at(t);
    w.wind_dir[0] = std::cos(h);
    w.wind_dir[1] = std::sin(h);
    w.gust = 0.3f + 0.35f * w.storm;

    // The sea answers the wind over the last few sea_lag: an exponential
    // average of the speed, sampled back three time constants. A pure
    // function of t, so a still and a long run agree about the waves.
    {
        const int n = 16;
        const double tau = settings_.sea_lag_s;
        double sum = 0.0, wsum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double back = 3.0 * tau * (double(i) + 0.5) / n;
            const double wt = std::exp(-back / tau);
            sum += wt * speed_for(storm_at(t - back));
            wsum += wt;
        }
        w.sea_wind = float(sum / wsum);
    }
    // The swell: a storm's worth averaged over a much longer window, from a
    // heading of its own that wanders slowly.
    {
        float s;
        if (swell_fixed_ >= 0.0f) {
            s = -1.0f;
            w.swell_hs = swell_fixed_;
        } else {
            const int n = 24;
            const double tau = settings_.swell_lag_s;
            double sum = 0.0, wsum = 0.0;
            for (int i = 0; i < n; ++i) {
                const double back = 3.0 * tau * (double(i) + 0.5) / n;
                const double wt = std::exp(-back / tau);
                const float st = storm_at(t - back);
                sum += wt * st * st;
                wsum += wt;
            }
            s = float(sum / wsum);
            w.swell_hs = settings_.swell_min + (settings_.swell_max - settings_.swell_min) * s;
        }
        const float base = float(worldgen::hash64(uint64_t(seed_) ^ 0x5357454Cull) >> 40) / 16777216.0f * float(kTwoPi);
        const float hs = base + 1.2f * (noise(4, t / (settings_.veer_period_s * 2.7)) - 0.5f);
        w.swell_dir[0] = std::cos(hs);
        w.swell_dir[1] = std::sin(hs);
    }

    // Drift: the integral of the wind, advanced from the cached point in
    // steps of at most half a second.
    if (t < drift_t_ - 1e-6) {
        drift_t_ = 0.0;
        drift_[0] = drift_[1] = 0.0;
    }
    if (t > drift_t_) {
        const double span = t - drift_t_;
        const int steps = int(std::ceil(span / 0.5));
        const double dt = span / steps;
        for (int i = 0; i < steps; ++i) {
            float vx, vz;
            wind_vector(drift_t_ + (double(i) + 0.5) * dt, vx, vz);
            drift_[0] += vx * dt;
            drift_[1] += vz * dt;
        }
        drift_t_ = t;
    }
    w.drift[0] = float(drift_[0]);
    w.drift[1] = float(drift_[1]);
    return w;
}

// The wind sea is a Pierson-Moskowitz spectrum, capped by fetch-limited
// (JONSWAP) growth: over an archipelago the wind rarely has the open ocean
// a fully developed sea needs, so a gale raises ~3 m, not 12. Its energy is
// shared over the thirty-two trains by the spectrum at each train's frequency
// and a cos^2 spread about the wind (a little floor, so no heading is dead
// still), then scaled so the whole holds the significant height exactly:
// with a train at every 137.5 degrees the share that points downwind varies
// with the heading, and the height must not.
//
// The swell is a narrow lump round its own wavelength and a cos^8 spread
// round its heading, scaled the same way. Energies add.
WwSea Weather::sea(const WeatherState& w) const {
    WwSea s;
    float e_wind[WW_WAVES], e_swell[WW_WAVES];

    const float U = w.sea_wind > 1.0f ? w.sea_wind : 1.0f;
    const float hs_pm = 0.21f * U * U / kG;
    const float hs_fetch = 1.6e-3f * U * std::sqrt(settings_.fetch_m / kG);
    float hs, wp;
    if (hs_fetch < hs_pm) {
        hs = hs_fetch;
        const float fp = 3.5f * kG / U * std::pow(kG * settings_.fetch_m / (U * U), -0.33f);
        wp = float(kTwoPi) * fp;
    } else {
        hs = hs_pm;
        wp = 0.877f * kG / U;
    }
    // A gale's real peak is ~120 m long and only 1 in 30 steep, which reads
    // on screen as a slow heave, not a storm. Moving the peak shorter keeps
    // the height and makes the sea a young one -- steeper, more broken --
    // which is what a storm is expected to look like up close.
    wp *= settings_.peak_shift;

    const float ks = float(kTwoPi) / settings_.swell_wavelength;
    double sum_w = 0.0, sum_s = 0.0;
    for (int i = 0; i < WW_WAVES; ++i) {
        const ww_float2 d = ww_direction(i);
        const float k = float(kTwoPi) / ww_wavelength(i);
        const float om = std::sqrt(kG * k);
        // S(w) dw with dw proportional to w on a log-spaced set.
        const float r = wp / om;
        const float spec = std::pow(om, -5.0f) * std::exp(-1.25f * r * r * r * r) * om;
        const float cw = d.x * w.wind_dir[0] + d.y * w.wind_dir[1];
        const float dw = (cw > 0.0f ? cw * cw : 0.0f) + 0.03f;
        e_wind[i] = spec * dw;
        sum_w += e_wind[i];

        const float lk = std::log(k / ks) / 0.2f;
        const float cs = d.x * w.swell_dir[0] + d.y * w.swell_dir[1];
        const float c2 = cs > 0.0f ? cs * cs : 0.0f;
        const float ds = c2 * c2 * c2 * c2;
        e_swell[i] = std::exp(-0.5f * lk * lk) * ds;
        sum_s += e_swell[i];
    }
    // m0 = Hs^2 / 16 is the variance; a train of amplitude a carries a^2 / 2.
    const double m0w = double(hs) * hs / 16.0;
    const double m0s = double(w.swell_hs) * w.swell_hs / 16.0;
    for (int i = 0; i < WW_WAVES; ++i) {
        const double e = (sum_w > 0.0 ? e_wind[i] / sum_w * m0w : 0.0) +
                         (sum_s > 0.0 ? e_swell[i] / sum_s * m0s : 0.0);
        s.amp[i] = float(std::sqrt(2.0 * e));
    }
    // Trains carrying under 1% of the biggest height AND under 2% of the
    // steepest slope are dropped (amplitude 0, which water_wave.h skips): in
    // light air most of the thirty-two are ripples no camera resolves, and
    // each costs a sine and a cosine at every pixel of sea.
    {
        float amax = 0.0f, smax = 0.0f;
        for (int i = 0; i < WW_WAVES; ++i) {
            const float k = float(kTwoPi) / ww_wavelength(i);
            if (s.amp[i] > amax) amax = s.amp[i];
            if (k * s.amp[i] > smax) smax = k * s.amp[i];
        }
        for (int i = 0; i < WW_WAVES; ++i) {
            const float k = float(kTwoPi) / ww_wavelength(i);
            if (s.amp[i] < 0.01f * amax && k * s.amp[i] < 0.02f * smax) s.amp[i] = 0.0f;
        }
    }
    // Crests sharpen with the wind: the target for the summed k a Q.
    const float st = 0.35f + 0.45f * w.storm;
    s.steep = st > 0.85f ? 0.85f : st;
    return s;
}

}  // namespace engine
