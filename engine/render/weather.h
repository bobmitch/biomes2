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
//   rain      0..1, from the storm dial past rain_storm[0] (ADR-005 C3).
//   wet       0..1, how wet the world's surfaces are (C4): the rain of the
//             last hour or so, each minute of it fading with age over
//             dry_s, saturating over soak_s. Puddles come after the ground
//             is soaked and go before it dries.
//   drift     where the air has carried a thing since t = 0, in metres --
//             the integral of the wind. Anything ADVECTED by the wind (the
//             mist's patches, the gust field) moves by this, never by
//             wind x t: once the wind changes, wind x t jumps.
//
// Presentation only: nothing here reaches the golden hash or a save. Tuning
// is archetypes.json's "weather" block.
// ---------------------------------------------------------------------------
#include "engine/render/water_wave.h"

#include <cmath>
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
    float rain_storm[2] = {0.55f, 0.9f};  // the storm dial where rain starts, and where it is a downpour
    float dry_s = 900.0f;          // how long rain that has stopped keeps the ground wet (e-folding)
    float soak_s = 120.0f;         // seconds of downpour that wet the ground through
    float puddle_s[2] = {240.0f, 600.0f};  // soak (rain-seconds) where puddles start, and the span to full
    float leaf_dry_s = 300.0f;     // how long the crowns keep dripping after the rain (e-folding)
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
    float rain = 0.0f;           // 0..1, now
    float wet = 0.0f;            // 0..1, the surfaces (C4)
    float puddle = 0.0f;         // 0..1, the puddles
    float drip = 0.0f;           // 0..1, how wet the crowns are: they drip after the rain
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
    float rain_for(float storm) const;
    // Wetness and puddles after `soak` rain-seconds (the decayed integral of
    // the rain), so an override can ask for a steady rain's.
    void wet_for(double soak, float& wet, float& puddle) const;
    // The soak a rain held steady at `rain` for ever settles to.
    double steady_soak(float rain) const;
    // The crowns' wetness after `soak` rain-seconds of their own, shorter memory.
    float drip_for(double leaf_soak) const { return float(1.0 - std::exp(-leaf_soak / 60.0)); }
    double steady_leaf_soak(float rain) const { return double(rain) * settings_.leaf_dry_s; }

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
