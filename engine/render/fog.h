#pragma once
// ---------------------------------------------------------------------------
// Mist (ADR-004 L4): where the air is thick, and how.
//
// A map over the whole disc, baked once per seed on the CPU from the
// generator's own biome blend (worldgen::biome_mix) and heights: per texel
// the mist's extinction at the surface, the height over which it thins by e,
// and the surface it lies on -- the ground, or the sea where there is sea.
// Per-biome densities and the sea's come from archetypes.json ("fog"). The
// froxel volume (fog.hlsl) reads it every frame and adds what moves: wind-
// drifted patches, morning mist, a thin haze everywhere.
//
// The central, safe biomes and open water are the misty ones on purpose --
// the wider map should be found, not seen. This is the hook that a later
// exploration system (mist that remembers where you have been, weather that
// rolls in) would write into: it is one density function, evaluated in one
// place.
//
// Presentation only: nothing here reaches the golden hash.
// ---------------------------------------------------------------------------
#include <string>
#include <vector>

namespace engine {

class WorldGpu;

struct FogSettings {
    float sea_density = 0.004f, sea_height = 90.0f;
    float haze = 0.00008f;       // extinction everywhere, per metre
    float morning = 2.5f;        // how much thicker the mist is round dawn
    float wind[2] = {1.5f, 0.6f};
    float patch = 160.0f;        // patch size, metres
    float phase_g = 0.55f;
    float albedo = 0.95f;
};

class FogMap {
public:
    static const int kSize = 256;   // ~78 m a texel over a 20 km disc

    bool build(const WorldGpu& world, const char* archetypes_path, std::string& error);
    bool built() const { return !texels_.empty(); }
    // RGBA float: density at the surface, thinning height, surface y, unused.
    const std::vector<float>& texels() const { return texels_; }
    float extent() const { return extent_; }   // the map spans [-extent/2, extent/2] in x and z
    const FogSettings& settings() const { return settings_; }
    // How much thicker than its base the mist is at this hour.
    float time_factor(float hours) const;
    // The baked, still part of the density at a point (no patches, no time).
    float base_density(float x, float y, float z) const;

private:
    std::vector<float> texels_;
    float extent_ = 20000.0f;
    FogSettings settings_;
};

}  // namespace engine
