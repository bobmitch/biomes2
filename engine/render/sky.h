#pragma once
// ---------------------------------------------------------------------------
// The sky on the CPU (ADR-004 L2): the atmosphere's two static tables, the
// sun and moon from the time of day, and each frame's light for the scene
// shaders -- all from atmosphere.h / atmosphere_march.h, the same text the
// GPU draws the sky with. See those headers for the model.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

namespace engine {

// What everything lights with this frame. Directions point TOWARDS the light;
// radiance in nits, illuminance in lux.
struct SkyFrame {
    float hours = 14.0f;
    float sun_dir[3] = {0, 1, 0}, moon_dir[3] = {0, -1, 0};
    float sun_lux_top[3] = {0, 0, 0}, moon_lux_top[3] = {0, 0, 0};
    float sun_elevation_deg = 0.0f;
    // The key light: whichever of sun and moon puts more light on the ground.
    // It casts the shadows and is the scene shaders' direct light.
    float key_dir[3] = {0, 1, 0};
    float key_lux[3] = {0, 0, 0};      // at the camera's altitude
    bool  key_is_moon = false;
    float sky_mean[3] = {0, 0, 0};     // mean radiance of the upper hemisphere
    float ground[3] = {0, 0, 0};       // radiance of the lit ground below
    float zenith[3] = {0, 0, 0};
    float horizon_sun[3] = {0, 0, 0};  // the horizon under the key light's azimuth
    float horizon_away[3] = {0, 0, 0}; // ... and opposite it
    float sun_disk[3] = {0, 0, 0};     // disk radiance, seen from the camera
    float moon_disk[3] = {0, 0, 0};
    float cam_alt_km = 0.0f;
};

class Sky {
public:
    // The transmittance and multiple-scattering tables: ~50 ms, once.
    void build();
    bool built() const { return !trans_.empty(); }
    // RGBA float texels, row-major, for upload.
    const std::vector<float>& transmittance() const { return trans_; }
    const std::vector<float>& multiple_scattering() const { return ms_; }

    // Sun and moon at this hour (latitude 40 N, declination +15: a late
    // spring day), and everything the scene lights with, from a camera at
    // world height cam_y metres.
    SkyFrame evaluate(float hours, float cam_y) const;

    // Radiance along a direction from an altitude, with the lights of `f`.
    // `steps` matches the GPU's 30 when checking its table.
    void radiance(const SkyFrame& f, float alt_km, const float dir[3], float out[3], int steps = 24) const;
    // The direction of a sky-view texel, as the GPU builds it.
    static void sky_texel_dir(int x, int y, float alt_km, float out[3]);

    static void light_dirs(float hours, float sun[3], float moon[3]);

private:
    std::vector<float> trans_, ms_;
};

// --sky-report: the model against published clear-sky references.
bool write_sky_report(const char* path, std::string& error);

}  // namespace engine
