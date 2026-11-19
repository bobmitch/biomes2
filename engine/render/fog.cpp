#include "engine/render/fog.h"
#include "engine/field/world_gpu.h"
#include "worldgen/assign/json_lite.h"
#include "worldgen/world.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace engine {

bool FogMap::build(const WorldGpu& world, const char* archetypes_path, std::string& error) {
    std::vector<float> dens, height;
    try {
        const jsonlite::Value root = jsonlite::parse_file(archetypes_path);
        const jsonlite::Array& bs = root.array("biomes");
        dens.assign(bs.size(), 0.0f);
        height.assign(bs.size(), 50.0f);
        for (size_t b = 0; b < bs.size(); ++b) {
            if (!bs[b].has("fog")) continue;
            const jsonlite::Value& f = bs[b].at("fog");
            dens[b] = float(f.number("density"));
            height[b] = float(f.number("height"));
        }
        if (root.has("fog")) {
            const jsonlite::Value& g = root.at("fog");
            const jsonlite::Value& sea = g.at("sea_fog");
            settings_.sea_density = float(sea.number("density"));
            settings_.sea_height = float(sea.number("height"));
            settings_.haze = float(g.number("haze"));
            settings_.morning = float(g.number("morning"));
            settings_.patch = float(g.number("patch"));
            settings_.phase_g = float(g.number("phase_g"));
            settings_.albedo = float(g.number("albedo"));
        }
    } catch (const std::exception& e) {
        error = std::string("archetypes.json fog: ") + e.what();
        return false;
    }

    const worldgen::WorldParams& wp = world.params();
    const float R = wp.radius;
    // A little past the rim, so the sea beyond the land is misty too.
    extent_ = 2.0f * R * 1.1f;
    const float sea = wp.sea_level;
    texels_.assign(size_t(kSize) * kSize * 4, 0.0f);
    for (int j = 0; j < kSize; ++j) {
        for (int i = 0; i < kSize; ++i) {
            const float x = -0.5f * extent_ + (float(i) + 0.5f) * extent_ / kSize;
            const float z = -0.5f * extent_ + (float(j) + 0.5f) * extent_ / kSize;
            const float r = std::sqrt(x * x + z * z) / R;
            float land_d = 0.0f, land_dh = 0.0f, wsum = 0.0f, ground = sea - 100.0f;
            if (r < 1.0f) {
                int b[4];
                float w[4];
                const int n = worldgen::biome_mix(world.world(), wp, x, z, b, w);
                for (int k = 0; k < n; ++k) {
                    if (b[k] < 0 || size_t(b[k]) >= dens.size()) continue;
                    land_d += w[k] * dens[b[k]];
                    land_dh += w[k] * dens[b[k]] * height[b[k]];
                    wsum += w[k];
                }
                ground = world.height(x, z);
            }
            // The sea's share: the blend's ocean weight, or the ground under
            // the water line, whichever is more -- so a lagoon inside a
            // biome is misty as the open sea is.
            const float under = std::fmin(std::fmax((sea + 2.0f - ground) / 4.0f, 0.0f), 1.0f);
            const float s = std::fmax(1.0f - wsum, under);
            const float d = (1.0f - s) * land_d + s * settings_.sea_density;
            const float dh = (1.0f - s) * land_dh + s * settings_.sea_density * settings_.sea_height;
            float* t = &texels_[(size_t(j) * kSize + i) * 4];
            t[0] = d;
            t[1] = d > 1e-7f ? dh / d : 50.0f;
            t[2] = std::fmax(ground, sea);
            t[3] = 0.0f;
        }
    }
    return true;
}

float FogMap::time_factor(float hours) const {
    // Thickest a little after dawn, burnt off by mid-morning; a little back
    // in the evening. Gaussian bumps in the hour.
    const float m = (hours - 6.5f) / 1.6f;
    const float e = (hours - 20.0f) / 2.0f;
    return 1.0f + settings_.morning * std::exp(-m * m) + 0.4f * settings_.morning * std::exp(-e * e);
}

float FogMap::base_density(float x, float y, float z) const {
    if (!built()) return 0.0f;
    const float u = (x / extent_ + 0.5f) * kSize - 0.5f, v = (z / extent_ + 0.5f) * kSize - 0.5f;
    const int i = int(std::floor(u)), j = int(std::floor(v));
    float acc[3] = {0, 0, 0};
    for (int dj = 0; dj < 2; ++dj) {
        for (int di = 0; di < 2; ++di) {
            const int ii = (std::min)((std::max)(i + di, 0), kSize - 1), jj = (std::min)((std::max)(j + dj, 0), kSize - 1);
            const float w = (di ? u - float(i) : 1.0f - (u - float(i))) * (dj ? v - float(j) : 1.0f - (v - float(j)));
            const float* t = &texels_[(size_t(jj) * kSize + ii) * 4];
            for (int k = 0; k < 3; ++k) acc[k] += w * t[k];
        }
    }
    return settings_.haze + acc[0] * std::exp(-std::fmax(y - acc[2], 0.0f) / std::fmax(acc[1], 1.0f));
}

}  // namespace engine
