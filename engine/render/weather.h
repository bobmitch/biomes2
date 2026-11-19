#pragma once
// ---------------------------------------------------------------------------
// Weather: the wind, and the sea it raises.
//
// One state, read by everything that moves in the wind -- grass, trees, the
// mist's patches -- and by the sea, whose wave trains it sets
// (water_wave.h). Before this each of those had its own idea of the wind:
// grass and trees blew one fixed way, the mist another, and the waves ran on
// headings no wind chose.
//
// The weather is a PURE FUNCTION of the seed and the clock the frame is drawn
// at (the same clock the waves and the swimmer use), so a save, a replay and
// a headless still all see the same sky without anything being stored. Its
// parts:
//
//   storm     0..1, the dial: a slow, smooth noise of time (period ~8 min)
//             pushed towards its ends, so most of the time is a breeze and a
//             gale comes now and then. The wind speed is wind_min +
//             (wind_max - wind_min) x storm^2.
//   wind      a heading that wanders by up to ~+-100 degrees over a quarter
//             of an hour, and gustiness that rises with the storm.
//   sea_wind  the wind speed the sea is developed FOR: the wind averaged over
//             the last sea_lag seconds, so waves build after the wind rises
//             and lie down after it drops.
//   swell     long waves from a storm somewhere else: its own slow heading,
//             and a height from the storm averaged over a much longer window
//             (swell_lag), so a swell keeps running after a gale has passed.
//   drift     where the air has carried a thing since t = 0, in metres --
//             the integral of the wind. Anything ADVECTED by the wind (the
//             mist's patches, the gust field) moves by this, never by
//             wind x t: once the wind changes, wind x t jumps.
//
// Presentation only: nothing here reaches the golden hash or a save. Tuning
// is archetypes.json's "weather" block.
// ---------------------------------------------------------------------------
#include "engine/render/water_wave.h"

#include <cstdint>
#include <string>

namespace engine {

struct WeatherSettings {
    float period_s = 480.0f;       // the storm noise's period
    float wind_min = 0.5f;         // m/s at storm 0
    float wind_max = 24.0f;        // m/s at storm 1
    float veer_period_s = 900.0f;  // the heading's noise period
    float veer = 1.75f;            // radians either side of the base heading
    float sea_lag_s = 90.0f;       // how long the wind sea takes to answer the wind
    float fetch_m = 150000.0f;     // open water the wind blows over (fetch-limited growth)
    float peak_shift = 1.4f;       // the spectrum's peak frequency times this: a younger, steeper sea
    float swell_max = 3.0f;        // significant height of the swell at its biggest
    float swell_min = 0.25f;       // ...and at its smallest
    float swell_wavelength = 110.0f;
    float swell_lag_s = 600.0f;
    float mist_drift = 0.5f;       // share of the wind's speed the mist drifts at
};

struct WeatherState {
    float storm = 0.0f;          // 0..1
    float wind_dir[2] = {1, 0};  // unit, x and z, the way the air MOVES
    float wind_speed = 0.0f;     // m/s, mean at 10 m
    float gust = 0.0f;           // 0..1, how far gusts swing the speed
    float sea_wind = 0.0f;       // m/s the wind sea is developed for
    float swell_dir[2] = {1, 0};
    float swell_hs = 0.0f;       // significant height, m
    float drift[2] = {0, 0};     // metres the air has moved since t = 0
};

class Weather {
public:
    // Reads archetypes.json's "weather" block; missing is fine (defaults).
    bool load(const char* archetypes_path, std::string& error);
    void set_seed(uint32_t seed) { seed_ = seed; drift_t_ = 0.0; drift_[0] = drift_[1] = 0.0; }
    const WeatherSettings& settings() const { return settings_; }

    // Overrides, for A/B and the window's keys. Negative means automatic.
    void set_storm(float s) { storm_fixed_ = s; wind_fixed_ = -1.0f; }
    void set_wind(float mps) { wind_fixed_ = mps; storm_fixed_ = -1.0f; }
    void set_heading(float radians_or_negative_for_auto) { heading_fixed_ = radians_or_negative_for_auto; heading_set_ = radians_or_negative_for_auto >= 0.0f; }
    void set_swell(float hs) { swell_fixed_ = hs; }
    void set_auto() { storm_fixed_ = -1.0f; wind_fixed_ = -1.0f; heading_set_ = false; swell_fixed_ = -1.0f; }
    bool automatic() const { return storm_fixed_ < 0.0f && wind_fixed_ < 0.0f; }

    // The weather at time t, drift included. Advancing the drift integral is
    // the one stateful part: it is cached, so asking for increasing t costs
    // the steps since the last call, and asking for an earlier t starts over.
    WeatherState at(double t);
    // The storm dial and wind heading alone, at time t (no drift, no lag).
    float storm_at(double t) const;
    float heading_at(double t) const;
    float speed_for(float storm) const;

    // The sea this weather raises: every train's deep-water amplitude.
    WwSea sea(const WeatherState& w) const;
    // A rough 0..1 for the shader's highlight and whitecaps: the local wind
    // speed over 20 m/s. Whitecaps start near Beaufort 4 (~5.5 m/s).
    static float roughness(const WeatherState& w) {
        const float r = w.wind_speed / 20.0f;
        return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    }

private:
    float noise(uint32_t stream, double x) const;
    void wind_vector(double t, float& x, float& z) const;

    WeatherSettings settings_;
    uint32_t seed_ = 0;
    float storm_fixed_ = -1.0f, wind_fixed_ = -1.0f, heading_fixed_ = -1.0f, swell_fixed_ = -1.0f;
    bool heading_set_ = false;
    double drift_t_ = 0.0;
    double drift_[2] = {0.0, 0.0};
};

}  // namespace engine
