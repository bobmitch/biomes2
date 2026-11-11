#include "archetypes.h"
#include "json_lite.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace worldgen {

void validate_archetypes(const ArchetypeTable& t) {
    std::vector<std::string> errs;
    const size_t n = t.size();

    if (n == 0) throw std::runtime_error("archetypes: table is empty");
    if (n > 32) errs.push_back("more than 32 biomes; adjacency_mask is a uint32");

    // 1. area_fraction sums to 1.0 within 1e-4.
    double sum = 0.0;
    for (const auto& b : t.biomes) sum += b.area_fraction;
    if (std::fabs(sum - 1.0) > 1e-4) {
        std::ostringstream o;
        o << "area_fraction sums to " << sum << ", expected 1.0 +/- 1e-4";
        errs.push_back(o.str());
    }

    // 2. Every danger_band is non-empty and danger_preferred lies inside it.
    for (const auto& b : t.biomes) {
        if (!(b.danger_band_min < b.danger_band_max)) {
            errs.push_back(b.name + ": danger_band is empty or inverted");
        }
        if (b.danger_preferred < b.danger_band_min || b.danger_preferred > b.danger_band_max) {
            errs.push_back(b.name + ": danger_preferred lies outside its danger_band");
        }
        if (b.area_fraction <= 0.0f) {
            errs.push_back(b.name + ": area_fraction must be positive");
        }
    }

    // 3. The union of all danger_bands covers [0,1] with no gap. A gap means
    //    some cells have no legal candidate and fall through to the relaxation
    //    branch, which silently degrades the radial ordering.
    {
        std::vector<std::pair<float, float> > bands;
        bands.reserve(n);
        for (const auto& b : t.biomes) bands.push_back(std::make_pair(b.danger_band_min, b.danger_band_max));
        std::sort(bands.begin(), bands.end());
        if (bands.front().first > 0.0f) {
            errs.push_back("danger_band union does not reach D = 0");
        }
        float reach = bands.front().second;
        for (size_t i = 1; i < bands.size(); ++i) {
            if (bands[i].first > reach) {
                std::ostringstream o;
                o << "danger_band union has a gap at D in [" << reach << ", " << bands[i].first << "]";
                errs.push_back(o.str());
            }
            reach = std::max(reach, bands[i].second);
        }
        if (reach < 1.0f) errs.push_back("danger_band union does not reach D = 1");
    }

    // 4. adjacency_mask is symmetric, and no biome forbids more than half the others.
    for (size_t i = 0; i < n; ++i) {
        int forbids = 0;
        for (size_t j = 0; j < n; ++j) {
            const bool ij = ((t.biomes[i].adjacency_mask >> j) & 1u) != 0u;
            const bool ji = ((t.biomes[j].adjacency_mask >> i) & 1u) != 0u;
            if (ij) ++forbids;
            if (ij != ji) {
                errs.push_back("adjacency_mask is not symmetric between " + t.biomes[i].name +
                               " and " + t.biomes[j].name);
            }
            if (i == j && ij) {
                errs.push_back(t.biomes[i].name + " forbids bordering itself");
            }
        }
        if (forbids * 2 > static_cast<int>(n)) {
            errs.push_back(t.biomes[i].name + " forbids more than half the other biomes; "
                           "assignment is over-constrained");
        }
    }

    if (!errs.empty()) {
        std::ostringstream o;
        o << "archetypes.json failed " << errs.size() << " invariant check(s):";
        for (size_t i = 0; i < errs.size(); ++i) o << "\n  - " << errs[i];
        throw std::runtime_error(o.str());
    }
}

ArchetypeTable load_archetypes(const std::string& path) {
    const jsonlite::Value root = jsonlite::parse_file(path);
    ArchetypeTable t;
    t.sea_level = root.has("sea_level") ? static_cast<float>(root.number("sea_level")) : 0.0f;

    const jsonlite::Array& list = root.array("biomes");
    t.biomes.reserve(list.size());

    for (size_t idx = 0; idx < list.size(); ++idx) {
        const jsonlite::Value& e = list[idx];
        BiomeArchetype b;
        b.name = e.string("name");

        b.area_fraction = static_cast<float>(e.number("area_fraction"));

        const jsonlite::Array& band = e.array("danger_band");
        if (band.size() != 2)
            throw std::runtime_error("archetypes: " + b.name + " danger_band needs exactly 2 entries");
        b.danger_band_min  = static_cast<float>(band[0].num);
        b.danger_band_max  = static_cast<float>(band[1].num);
        b.danger_preferred = static_cast<float>(e.number("danger_preferred"));
        b.allow_island     = e.boolean("allow_island");

        b.base_elevation = static_cast<float>(e.number("base_elevation"));
        b.amplitude      = static_cast<float>(e.number("amplitude"));
        b.octaves        = static_cast<uint32_t>(e.number("octaves"));
        b.lacunarity     = static_cast<float>(e.number("lacunarity"));
        b.gain           = static_cast<float>(e.number("gain"));
        b.ridge_blend    = static_cast<float>(e.number("ridge_blend"));
        b.flatness       = static_cast<float>(e.number("flatness"));

        b.overhang_strength = static_cast<float>(e.number("overhang_strength"));
        b.cave_density      = static_cast<float>(e.number("cave_density"));

        const jsonlite::Array& depth = e.array("cave_depth_band");
        if (depth.size() != 2)
            throw std::runtime_error("archetypes: " + b.name + " cave_depth_band needs exactly 2 entries");
        b.cave_depth_min = static_cast<float>(depth[0].num);
        b.cave_depth_max = static_cast<float>(depth[1].num);

        b.material_id        = static_cast<uint32_t>(e.number("material_id"));
        b.water_level_offset = static_cast<float>(e.number("water_level_offset"));

        t.biomes.push_back(b);
    }

    // Resolve adjacency_forbid names to a bitmask, applying both directions so
    // the table states the rule once rather than twice. An unknown name is a
    // typo that would otherwise silently drop a constraint, so it is fatal.
    for (size_t i = 0; i < list.size(); ++i) {
        const jsonlite::Array& forbid = list[i].array("adjacency_forbid");
        for (size_t k = 0; k < forbid.size(); ++k) {
            if (forbid[k].type != jsonlite::Value::String)
                throw std::runtime_error("archetypes: adjacency_forbid entries must be names");
            size_t j = t.size();
            for (size_t m = 0; m < t.size(); ++m) {
                if (t.biomes[m].name == forbid[k].str) { j = m; break; }
            }
            if (j == t.size())
                throw std::runtime_error("archetypes: " + t.biomes[i].name +
                                         " forbids unknown biome " + forbid[k].str);
            t.biomes[i].adjacency_mask |= (1u << j);
            t.biomes[j].adjacency_mask |= (1u << i);
        }
    }

    validate_archetypes(t);
    return t;
}

}  // namespace worldgen
