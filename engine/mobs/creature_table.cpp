#include "engine/mobs/creature_table.h"

#include "worldgen/assign/archetypes.h"
#include "worldgen/assign/json_lite.h"

#include <stdexcept>

namespace engine {
namespace mobs {

namespace {

Move parse_move(const std::string& s) {
    if (s == "ground") return Move::Ground;
    if (s == "amphibious") return Move::Amphibious;
    if (s == "water_surface") return Move::WaterSurface;
    if (s == "water_deep") return Move::WaterDeep;
    if (s == "air") return Move::Air;
    throw std::runtime_error("move '" + s + "': ground, amphibious, water_surface, water_deep or air");
}

Size parse_size(const std::string& s) {
    if (s == "small") return Size::Small;
    if (s == "human") return Size::Human;
    if (s == "large") return Size::Large;
    throw std::runtime_error("size '" + s + "': small, human or large");
}

}  // namespace

int CreatureTable::find(const std::string& id) const {
    for (size_t i = 0; i < kinds.size(); ++i)
        if (kinds[i].id == id) return int(i);
    return -1;
}

bool CreatureTable::load(const char* path, const worldgen::ArchetypeTable& biomes, std::string& error) {
    kinds.clear();
    spawns.clear();
    zones = ZoneTuning();
    try {
        const jsonlite::Value root = jsonlite::parse_file(path);
        if (root.has("zones")) {
            const jsonlite::Value& z = root.at("zones");
            if (z.has("size_m")) zones.size_m = float(z.number("size_m"));
            if (z.has("active_m")) zones.active_m = float(z.number("active_m"));
            if (z.has("release_m")) zones.release_m = float(z.number("release_m"));
            if (z.has("max_drawn")) zones.max_drawn = int(z.number("max_drawn"));
            if (zones.size_m < 8.0f || zones.active_m <= 0.0f || zones.release_m < zones.active_m)
                throw std::runtime_error("zones: size_m >= 8, active_m > 0, release_m >= active_m");
        }
        for (const jsonlite::Value& e : root.array("kinds")) {
            CreatureKind k;
            k.id = e.string("id");
            k.name = e.has("name") ? e.string("name") : k.id;
            if (find(k.id) >= 0) throw std::runtime_error("kind '" + k.id + "' twice");
            auto num = [&](const char* key, float& v) { if (e.has(key)) v = float(e.number(key)); };
            if (e.has("model")) k.model = e.string("model");
            if (k.model != "player") throw std::runtime_error(k.id + ": the only model is 'player'");
            if (e.has("tint")) {
                const jsonlite::Array& t = e.array("tint");
                if (t.size() != 3) throw std::runtime_error(k.id + ": tint is [r, g, b]");
                for (int c = 0; c < 3; ++c) k.tint[c] = float(t[size_t(c)].num);
            }
            if (e.has("move")) k.move = parse_move(e.string("move"));
            if (e.has("size")) k.size = parse_size(e.string("size"));
            num("health", k.health);
            num("walk_speed", k.walk_speed);
            num("run_speed", k.run_speed);
            num("hear_range_m", k.hear_range_m);
            num("hear_threshold_db", k.hear_threshold_db);
            num("view_range_m", k.view_range_m);
            num("view_cone_deg", k.view_cone_deg);
            num("dark_gain", k.dark_gain);
            num("view_vertical_deg", k.view_vertical_deg);
            if (e.has("script")) k.script = e.string("script");
            if (k.health <= 0.0f || k.hear_range_m < 0.0f || k.view_range_m < 0.0f || k.dark_gain < 0.0f)
                throw std::runtime_error(k.id + ": health > 0, ranges and dark_gain >= 0");
            kinds.push_back(k);
        }
        if (root.has("spawns")) {
            for (const jsonlite::Value& e : root.array("spawns")) {
                SpawnRule r;
                r.kind = find(e.string("kind"));
                if (r.kind < 0) throw std::runtime_error("spawn: no kind '" + e.string("kind") + "'");
                r.biome_name = e.string("biome");
                for (size_t b = 0; b < biomes.size(); ++b)
                    if (biomes[b].name == r.biome_name) r.biome = int(b);
                if (r.biome < 0) throw std::runtime_error("spawn: no biome '" + r.biome_name + "' in archetypes.json");
                auto num = [&](const char* key, float& v) { if (e.has(key)) v = float(e.number(key)); };
                auto inum = [&](const char* key, int& v) { if (e.has(key)) v = int(e.number(key)); };
                num("min_weight", r.min_weight);
                inum("max_per_zone", r.max_per_zone);
                inum("max_active", r.max_active);
                num("interval_h", r.interval_h);
                num("chance", r.chance);
                num("min_player_m", r.min_player_m);
                num("min_same_m", r.min_same_m);
                if (e.has("hours")) {
                    const jsonlite::Array& h = e.array("hours");
                    if (h.size() != 2) throw std::runtime_error("spawn hours is [from, to)");
                    r.hour_from = float(h[0].num);
                    r.hour_to = float(h[1].num);
                }
                if (e.has("group")) {
                    const jsonlite::Array& g = e.array("group");
                    if (g.size() != 2) throw std::runtime_error("spawn group is [min, max]");
                    r.group_min = int(g[0].num);
                    r.group_max = int(g[1].num);
                }
                if (r.interval_h <= 0.0f || r.chance < 0.0f || r.chance > 1.0f || r.max_per_zone < 0 ||
                    r.max_active < 0 || r.group_min < 1 || r.group_max < r.group_min)
                    throw std::runtime_error("spawn " + kinds[size_t(r.kind)].id +
                                             ": interval_h > 0, chance 0..1, caps >= 0, 1 <= group min <= max");
                spawns.push_back(r);
            }
        }
    } catch (const std::exception& ex) {
        error = std::string(path) + ": " + ex.what();
        kinds.clear();
        spawns.clear();
        return false;
    }
    return true;
}

}  // namespace mobs
}  // namespace engine
