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
    // Metres of the shared micro-relief (DetailTable) this biome carries:
    // hummocks, swales and low banks below the profile's finest octave.
    float    detail_amp     = 0.0f;
    // Chance a flat-site candidate cell (FlatTable) in this biome holds a
    // site, before the terrain tests. 0 = none.
    float    flat_chance    = 0.0f;

    // --- volumetric (layer 6) ---
    float    overhang_strength = 0.0f;
    float    cave_density      = 0.0f;
    float    cave_depth_min    = 0.0f;
    float    cave_depth_max    = 0.0f;

    // --- placed caves (layer 7, docs/worldgen-design.md) ---
    // per_km2 is the GUARANTEED density: the biome's candidate grid has cells
    // of 1 km / sqrt(per_km2), and every cell inside the biome holds one cave
    // unless every retry inside it fails. 0 = none. The rest is the shape.
    float cave_per_km2      = 0.0f;
    float cave_mouth[2]     = {0.0f, 0.0f};   // mouth radius, metres
    float cave_tunnel[2]    = {0.0f, 0.0f};   // tunnel path length, metres
    float cave_radius[2]    = {0.0f, 0.0f};   // tunnel radius, metres
    float cave_turn         = 0.0f;           // heading wander per segment
    float cave_depth[2]     = {0.0f, 0.0f};   // chamber floor below the mouth's ground, metres
    int   cave_chambers     = 1;
    float cave_chamber[2]   = {0.0f, 0.0f};   // chamber width, metres
    float cave_chamber_h[2] = {0.0f, 0.0f};   // chamber height, metres
    float cave_sealed       = 0.0f;           // chance of no mouth: a pocket only digging finds

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

    // --- rocks (layer 9) ---
    // Chance a candidate cell holds a boulder / an outcrop inside a rock
    // patch, and the share of the biome that is patch. All 0 when the biome
    // has no rocks block.
    float    rock_boulders = 0.0f;
    float    rock_outcrops = 0.0f;
    float    rock_cover    = 0.0f;
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

// One class of rock (layer 9): boulders, or outcrops set into slopes.
struct RockClass {
    float cell      = 8.0f;    // candidate spacing, metres
    float size_min  = 0.4f;    // metres across; skewed to the small end (cubic)
    float size_max  = 4.0f;
    float sink      = 0.25f;   // share of the size set below the ground
    float max_slope = 1.5f;    // rise over run; steeper ground holds none
    // Outcrops want a slope: kept with weight 0 at slope_lo rising to 1 at
    // slope_hi. Boulders leave both 0, which keeps every slope.
    float slope_lo  = 0.0f;
    float slope_hi  = 0.0f;
};

// Layer 4's micro-relief: ONE noise field shared by every biome, scaled by
// each biome's detail_amp. Scaling a shared field and blending the results is
// blending heights, not noise parameters, so it is safe across a border.
struct DetailTable {
    float period  = 72.0f;   // metres, the coarsest octave
    int   octaves = 3;
    float gain    = 0.5f;
};

// Layer 5b, flat sites: rare level ground, a pure function of a candidate
// cell like a tree, placed where the land is already gentle and pulled level
// to the mean of its own pad, with an eased shoulder.
struct FlatTable {
    float cell         = 320.0f;  // candidate spacing, metres
    float radius_min   = 14.0f;   // level pad radius, metres
    float radius_max   = 36.0f;
    float shoulder     = 0.6f;    // shoulder width as a share of the radius ...
    float shoulder_min = 10.0f;   // ... never under this, metres
    float max_grade    = 0.08f;   // rise over run the ground may already have across it
    float river_clear  = 15.0f;   // metres between the shoulder and a channel's edge
    float shore        = 2.0f;    // metres above sea level the pad needs
    float tree_clear   = 3.0f;    // metres past the pad no tree or rock stands
    bool  present      = false;
};

struct RockTable {
    RockClass boulder, outcrop;
    float patch_freq    = 0.006f;  // cycles per metre of the rock patch noise
    float patch_edge    = 0.12f;
    float shore         = 0.3f;    // metres above sea level a rock's base needs
    int   blend_sharpen = 2;       // biome blend weights to this power for rocks
    bool  present       = false;   // the table had a rocks block
};

// Placed caves, the shared part (each biome's shape is in BiomeArchetype).
struct CaveTable {
    int   tries       = 12;     // spots tried inside a cell before it holds none
    float cover       = 3.0f;   // metres of rock kept over everything past the mouth
    float max_slope   = 0.6f;   // rise over run at the mouth
    float shore       = 3.0f;   // metres above sea level the mouth needs
    float river_clear = 10.0f;  // metres between the mouth and a channel's edge
    float segment     = 8.0f;   // metres per tunnel segment
    float wall_noise  = 0.8f;   // metres of wall roughness
    float wall_period = 5.0f;   // metres
    float max_grade   = 0.6f;   // drop over run of the tunnel, so it can be walked
    bool  present     = false;
};

struct ArchetypeTable {
    std::vector<BiomeArchetype> biomes;
    float sea_level = 0.0f;
    TreeTable trees;
    RockTable rocks;
    DetailTable detail;
    FlatTable flats;
    CaveTable caves;

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
