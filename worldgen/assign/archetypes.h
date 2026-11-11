// The biome archetype contract (design doc: "Biome archetype contract").
// All tuning is data. Nothing in this header or its implementation branches on
// a biome's name or identity -- `name` exists for diagnostics only.
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace worldgen {

struct BiomeArchetype {
    std::string name;              // diagnostics only; never used for logic

    // --- placement (layer 3) ---
    float    area_fraction    = 0.0f;   // share of LAND area; all entries sum to 1
    float    danger_band_min  = 0.0f;   // [min, max] of D this biome may occupy
    float    danger_band_max  = 1.0f;
    float    danger_preferred = 0.0f;   // target D; drives the assignment cost
    uint32_t adjacency_mask   = 0u;     // bitmask of biomes this one must not border
    bool     allow_island     = true;

    // --- height profile (layer 4) ---
    float    base_elevation = 0.0f;
    float    amplitude      = 0.0f;
    uint32_t octaves        = 4u;
    float    lacunarity     = 2.0f;
    float    gain           = 0.5f;
    float    ridge_blend    = 0.0f;
    float    flatness       = 1.0f;

    // --- volumetric (layer 6) ---
    float    overhang_strength = 0.0f;
    float    cave_density      = 0.0f;
    float    cave_depth_min    = 0.0f;
    float    cave_depth_max    = 0.0f;

    // --- surface ---
    uint32_t material_id        = 0u;
    float    water_level_offset = 0.0f;
};

struct ArchetypeTable {
    std::vector<BiomeArchetype> biomes;
    float sea_level = 0.0f;

    size_t size() const { return biomes.size(); }
    const BiomeArchetype& operator[](size_t i) const { return biomes[i]; }
};

// Throws std::runtime_error naming every violated invariant at once.
// These are the startup asserts the design doc requires -- checked when the
// table is loaded, never per-sample.
void validate_archetypes(const ArchetypeTable& t);

// Loads, resolves adjacency names to a symmetric mask, then validates.
ArchetypeTable load_archetypes(const std::string& path);

}  // namespace worldgen
