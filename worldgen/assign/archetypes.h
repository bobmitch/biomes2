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

    // --- trees (layer 8) ---
    // Chance a candidate cell holds a tree inside a patch, and the share of
    // the biome's area that is patch rather than clearing. Both 0 when the
    // biome has no trees block.
    float    tree_density = 0.0f;
    float    tree_cover   = 0.0f;
    // Relative weight per species, indexed like TreeTable::species.
    std::vector<float> tree_species;
};

// One tree species. `form` and everything the engine reads under "shape" are
// presentation; placement reads only the height and altitude ranges.
struct TreeSpecies {
    std::string name;              // diagnostics, and the key biomes refer to it by
    std::string form;              // which mesh generator draws it; worldgen ignores it
    float height_min = 10.0f, height_max = 20.0f;   // metres
    float alt_lo = -1.0e9f, alt_hi = 1.0e9f;        // metres above sea level
};

struct TreeTable {
    float cell          = 4.0f;     // candidate spacing, metres
    float patch_freq    = 0.0045f;  // cycles per metre of the patch/clearing noise
    float patch_edge    = 0.10f;    // noise units over which a patch edge thins out
    float max_slope     = 0.80f;    // rise over run; steeper ground holds no tree
    float shore         = 0.8f;     // metres above sea level a trunk needs
    float altitude_ramp = 60.0f;    // metres of fade inside each end of a species' band
    int   blend_sharpen = 4;        // biome blend weights to this power for trees
    std::vector<TreeSpecies> species;
};

struct ArchetypeTable {
    std::vector<BiomeArchetype> biomes;
    float sea_level = 0.0f;
    TreeTable trees;

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
