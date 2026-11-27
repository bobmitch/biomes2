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

    // Layer 8's species table, before the biomes: a biome refers to species by
    // name, and an unknown name is a typo that would silently plant nothing.
    if (root.has("trees")) {
        const jsonlite::Value& tr = root.obj.at("trees");
        TreeTable& tt = t.trees;
        tt.cell          = static_cast<float>(tr.number("cell"));
        tt.patch_freq    = static_cast<float>(tr.number("patch_freq"));
        tt.patch_edge    = static_cast<float>(tr.number("patch_edge"));
        tt.max_slope     = static_cast<float>(tr.number("max_slope"));
        tt.shore         = static_cast<float>(tr.number("shore"));
        tt.altitude_ramp = static_cast<float>(tr.number("altitude_ramp"));
        tt.blend_sharpen = static_cast<int>(tr.number("blend_sharpen"));
        if (tt.blend_sharpen < 1 || tt.blend_sharpen > 16)
            throw std::runtime_error("archetypes: trees.blend_sharpen must be 1..16");
        const jsonlite::Array& sp = tr.array("species");
        for (size_t i = 0; i < sp.size(); ++i) {
            TreeSpecies s;
            s.name = sp[i].string("name");
            s.form = sp[i].string("form");
            const jsonlite::Array& hh = sp[i].array("height");
            const jsonlite::Array& aa = sp[i].array("altitude");
            if (hh.size() != 2 || aa.size() != 2)
                throw std::runtime_error("archetypes: tree " + s.name + " needs 2-entry height and altitude");
            s.height_min = static_cast<float>(hh[0].num);
            s.height_max = static_cast<float>(hh[1].num);
            s.alt_lo = static_cast<float>(aa[0].num);
            s.alt_hi = static_cast<float>(aa[1].num);
            if (!(s.height_min > 0.0f && s.height_min <= s.height_max && s.alt_lo < s.alt_hi))
                throw std::runtime_error("archetypes: tree " + s.name + " has an empty height or altitude range");
            tt.species.push_back(s);
        }
        if (!(tt.cell > 0.0f && tt.altitude_ramp > 0.0f))
            throw std::runtime_error("archetypes: trees.cell and trees.altitude_ramp must be positive");
    }

    // Layer 9's classes. Optional: a table without them places no rocks.
    if (root.has("rocks")) {
        const jsonlite::Value& rr = root.obj.at("rocks");
        RockTable& rt = t.rocks;
        rt.present       = true;
        rt.patch_freq    = static_cast<float>(rr.number("patch_freq"));
        rt.patch_edge    = static_cast<float>(rr.number("patch_edge"));
        rt.shore         = static_cast<float>(rr.number("shore"));
        rt.blend_sharpen = static_cast<int>(rr.number("blend_sharpen"));
        if (rt.blend_sharpen < 1 || rt.blend_sharpen > 16)
            throw std::runtime_error("archetypes: rocks.blend_sharpen must be 1..16");
        auto read_class = [](const jsonlite::Value& c, RockClass& k, const char* what) {
            const jsonlite::Array& sz = c.array("size");
            if (sz.size() != 2) throw std::runtime_error(std::string("archetypes: rocks.") + what + ".size needs 2 entries");
            k.cell      = static_cast<float>(c.number("cell"));
            k.size_min  = static_cast<float>(sz[0].num);
            k.size_max  = static_cast<float>(sz[1].num);
            k.sink      = static_cast<float>(c.number("sink"));
            k.max_slope = static_cast<float>(c.number("max_slope"));
            k.slope_lo  = c.has("slope_lo") ? static_cast<float>(c.number("slope_lo")) : 0.0f;
            k.slope_hi  = c.has("slope_hi") ? static_cast<float>(c.number("slope_hi")) : 0.0f;
            if (!(k.cell > 0.0f && k.size_min > 0.0f && k.size_min <= k.size_max &&
                  k.size_max * 0.5f < k.cell && k.sink >= 0.0f && k.sink < 1.0f && k.slope_lo <= k.slope_hi))
                throw std::runtime_error(std::string("archetypes: rocks.") + what +
                                         " needs cell > 0, 0 < size_min <= size_max < 2 cell, 0 <= sink < 1");
        };
        read_class(rr.obj.at("boulder"), rt.boulder, "boulder");
        read_class(rr.obj.at("outcrop"), rt.outcrop, "outcrop");
    }

    // Layer 4's micro-relief and layer 5b's flat sites. Both optional: a table
    // without them has neither.
    if (root.has("detail")) {
        const jsonlite::Value& d = root.obj.at("detail");
        t.detail.period  = static_cast<float>(d.number("period"));
        t.detail.octaves = static_cast<int>(d.number("octaves"));
        t.detail.gain    = static_cast<float>(d.number("gain"));
        if (!(t.detail.period > 0.0f && t.detail.octaves >= 1 && t.detail.octaves <= 8 &&
              t.detail.gain > 0.0f && t.detail.gain < 1.0f))
            throw std::runtime_error("archetypes: detail needs period > 0, octaves 1..8, 0 < gain < 1");
    }
    // Layer 10's species, before the biomes, which refer to them by name.
    // Optional: a table without a forage block grows nothing to pick.
    if (root.has("forage")) {
        const jsonlite::Value& fr = root.obj.at("forage");
        ForageTable& ft = t.forage;
        ft.present = true;
        if (fr.has("blend_sharpen")) ft.blend_sharpen = static_cast<int>(fr.number("blend_sharpen"));
        if (ft.blend_sharpen < 1 || ft.blend_sharpen > 16)
            throw std::runtime_error("archetypes: forage.blend_sharpen must be 1..16");
        const jsonlite::Array& sp = fr.array("species");
        if (sp.size() > 15) throw std::runtime_error("archetypes: forage holds at most 15 species");
        for (size_t k = 0; k < sp.size(); ++k) {
            const jsonlite::Value& e = sp[k];
            ForageSpecies s;
            s.name = e.string("name");
            s.cell = static_cast<float>(e.number("cell"));
            const jsonlite::Array& pl = e.array("plants");
            if (pl.size() != 2) throw std::runtime_error("archetypes: forage " + s.name + ".plants needs 2 entries");
            s.plants_min   = static_cast<int>(pl[0].num);
            s.plants_max   = static_cast<int>(pl[1].num);
            s.patch_radius = static_cast<float>(e.number("patch_radius"));
            s.spacing      = static_cast<float>(e.number("spacing"));
            s.max_slope    = static_cast<float>(e.number("max_slope"));
            s.shore        = static_cast<float>(e.number("shore"));
            if (!(s.cell > 0.0f && s.plants_min >= 1 && s.plants_min <= s.plants_max && s.plants_max <= 4 &&
                  s.patch_radius >= 0.0f && 2.0f * s.patch_radius < 0.5f * s.cell && s.spacing >= 0.0f &&
                  s.max_slope > 0.0f))
                throw std::runtime_error("archetypes: forage " + s.name +
                                         " needs cell > 0, 1 <= plants <= 4, patch_radius under a quarter cell, max_slope > 0");
            for (size_t m = 0; m < ft.species.size(); ++m)
                if (ft.species[m].name == s.name) throw std::runtime_error("archetypes: forage " + s.name + " twice");
            ft.species.push_back(s);
        }
    }

    if (root.has("caves")) {
        const jsonlite::Value& c = root.obj.at("caves");
        CaveTable& ct = t.caves;
        ct.present     = true;
        ct.tries       = static_cast<int>(c.number("tries"));
        ct.cover       = static_cast<float>(c.number("cover"));
        ct.max_slope   = static_cast<float>(c.number("max_slope"));
        ct.shore       = static_cast<float>(c.number("shore"));
        ct.river_clear = static_cast<float>(c.number("river_clear"));
        ct.segment     = static_cast<float>(c.number("segment"));
        ct.wall_noise  = static_cast<float>(c.number("wall_noise"));
        ct.wall_period = static_cast<float>(c.number("wall_period"));
        ct.max_grade   = static_cast<float>(c.number("max_grade"));
        if (ct.tries < 1 || ct.segment <= 0.0f || ct.wall_period <= 0.0f || ct.wall_noise <= 0.0f ||
            ct.max_grade <= 0.0f)
            throw std::runtime_error("archetypes: caves needs tries >= 1, segment, wall_period, max_grade and wall_noise > 0");
    }
    if (root.has("flats")) {
        const jsonlite::Value& f = root.obj.at("flats");
        FlatTable& ft = t.flats;
        ft.present = true;
        const jsonlite::Array& rr = f.array("radius");
        if (rr.size() != 2) throw std::runtime_error("archetypes: flats.radius needs 2 entries");
        ft.cell         = static_cast<float>(f.number("cell"));
        ft.radius_min   = static_cast<float>(rr[0].num);
        ft.radius_max   = static_cast<float>(rr[1].num);
        ft.shoulder     = static_cast<float>(f.number("shoulder"));
        ft.shoulder_min = static_cast<float>(f.number("shoulder_min"));
        ft.max_grade    = static_cast<float>(f.number("max_grade"));
        ft.river_clear  = static_cast<float>(f.number("river_clear"));
        ft.shore        = static_cast<float>(f.number("shore"));
        ft.tree_clear   = static_cast<float>(f.number("tree_clear"));
        // A site sits in the middle 56% of its cell (world.cpp), so two can
        // never touch while the outer edge stays under 0.22 cell.
        const float outer = ft.radius_max + (ft.radius_max * ft.shoulder > ft.shoulder_min
                                             ? ft.radius_max * ft.shoulder : ft.shoulder_min);
        if (!(ft.radius_min > 0.0f && ft.radius_min <= ft.radius_max && ft.shoulder >= 0.0f &&
              ft.cell > 0.0f && outer + ft.tree_clear < 0.22f * ft.cell && ft.max_grade > 0.0f))
            throw std::runtime_error("archetypes: flats needs 0 < radius_min <= radius_max and "
                                     "radius + shoulder + tree_clear under 0.22 cell");
    }

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
        b.detail_amp     = e.has("detail") ? static_cast<float>(e.number("detail")) : 0.0f;
        b.flat_chance    = e.has("flats") ? static_cast<float>(e.number("flats")) : 0.0f;
        if (b.detail_amp < 0.0f || b.flat_chance < 0.0f || b.flat_chance > 1.0f)
            throw std::runtime_error("archetypes: " + b.name + " needs detail >= 0 and flats in [0, 1]");

        b.overhang_strength = static_cast<float>(e.number("overhang_strength"));
        b.cave_density      = static_cast<float>(e.number("cave_density"));

        const jsonlite::Array& depth = e.array("cave_depth_band");
        if (depth.size() != 2)
            throw std::runtime_error("archetypes: " + b.name + " cave_depth_band needs exactly 2 entries");
        b.cave_depth_min = static_cast<float>(depth[0].num);
        b.cave_depth_max = static_cast<float>(depth[1].num);

        if (e.has("caves")) {
            const jsonlite::Value& cb = e.obj.at("caves");
            auto pair = [&](const char* k, float* out) {
                const jsonlite::Array& a = cb.array(k);
                if (a.size() != 2 || a[0].num < 0.0 || a[1].num < a[0].num)
                    throw std::runtime_error("archetypes: " + b.name + " caves." + k + " needs [lo, hi], 0 <= lo <= hi");
                out[0] = static_cast<float>(a[0].num);
                out[1] = static_cast<float>(a[1].num);
            };
            b.cave_per_km2 = static_cast<float>(cb.number("per_km2"));
            if (b.cave_per_km2 < 0.0f)
                throw std::runtime_error("archetypes: " + b.name + " caves.per_km2 must be >= 0");
            if (b.cave_per_km2 > 0.0f) {
                pair("mouth", b.cave_mouth);
                pair("tunnel", b.cave_tunnel);
                pair("radius", b.cave_radius);
                pair("depth", b.cave_depth);
                pair("chamber", b.cave_chamber);
                pair("chamber_height", b.cave_chamber_h);
                b.cave_turn = static_cast<float>(cb.number("turn"));
                b.cave_chambers = static_cast<int>(cb.number("chambers"));
                b.cave_sealed = cb.has("sealed") ? static_cast<float>(cb.number("sealed")) : 0.0f;
                if (b.cave_chambers < 1 || b.cave_chambers > 4 || b.cave_radius[0] <= 0.0f ||
                    b.cave_chamber_h[0] <= 0.0f || b.cave_sealed < 0.0f || b.cave_sealed > 1.0f)
                    throw std::runtime_error("archetypes: " + b.name + " caves needs 1-4 chambers, radius and chamber_height > 0, sealed in [0, 1]");
            }
        }

        b.material_id        = static_cast<uint32_t>(e.number("material_id"));
        b.water_level_offset = static_cast<float>(e.number("water_level_offset"));

        b.tree_species.assign(t.trees.species.size(), 0.0f);
        b.forage.assign(t.forage.species.size(), 0.0f);
        if (e.has("forage")) {
            const jsonlite::Value& fb = e.obj.at("forage");
            for (jsonlite::Object::const_iterator it = fb.obj.begin(); it != fb.obj.end(); ++it) {
                if (it->first.size() > 0 && it->first[0] == '_') continue;
                size_t k = t.forage.species.size();
                for (size_t m = 0; m < t.forage.species.size(); ++m)
                    if (t.forage.species[m].name == it->first) { k = m; break; }
                if (k == t.forage.species.size())
                    throw std::runtime_error("archetypes: " + b.name + " grows unknown forage " + it->first);
                if (it->second.num < 0.0 || it->second.num > 1.0)
                    throw std::runtime_error("archetypes: " + b.name + " forage " + it->first + " must be in [0, 1]");
                b.forage[k] = static_cast<float>(it->second.num);
            }
        }
        if (e.has("trees")) {
            const jsonlite::Value& tb = e.obj.at("trees");
            b.tree_density = static_cast<float>(tb.number("density"));
            b.tree_cover   = static_cast<float>(tb.number("cover"));
            if (b.tree_density < 0.0f || b.tree_density > 1.0f || b.tree_cover < 0.0f || b.tree_cover > 1.0f)
                throw std::runtime_error("archetypes: " + b.name + " trees density and cover must be in [0, 1]");
            const jsonlite::Value& ws = tb.obj.at("species");
            for (jsonlite::Object::const_iterator it = ws.obj.begin(); it != ws.obj.end(); ++it) {
                size_t k = t.trees.species.size();
                for (size_t m = 0; m < t.trees.species.size(); ++m) {
                    if (t.trees.species[m].name == it->first) { k = m; break; }
                }
                if (k == t.trees.species.size())
                    throw std::runtime_error("archetypes: " + b.name + " plants unknown tree " + it->first);
                if (it->second.num < 0.0)
                    throw std::runtime_error("archetypes: " + b.name + " has a negative weight for " + it->first);
                b.tree_species[k] = static_cast<float>(it->second.num);
            }
        }
        if (e.has("rocks")) {
            const jsonlite::Value& rb = e.obj.at("rocks");
            b.rock_boulders = static_cast<float>(rb.number("boulders"));
            b.rock_outcrops = static_cast<float>(rb.number("outcrops"));
            b.rock_cover    = static_cast<float>(rb.number("cover"));
            if (b.rock_boulders < 0.0f || b.rock_boulders > 1.0f || b.rock_outcrops < 0.0f ||
                b.rock_outcrops > 1.0f || b.rock_cover < 0.0f || b.rock_cover > 1.0f)
                throw std::runtime_error("archetypes: " + b.name + " rocks boulders, outcrops and cover must be in [0, 1]");
        }

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
