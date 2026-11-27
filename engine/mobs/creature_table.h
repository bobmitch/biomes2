// ---------------------------------------------------------------------------
// The creature table: game/data/creatures.json (ADR-007). Kinds -- what a
// creature is, how it moves and perceives -- and spawn rules -- where, how
// often, how many. Gameplay data: never in the golden hash, and worldgen
// never reads it. Biomes are named here and resolved to indices against the
// archetype table at load, so the engine code knows no biome by name
// (invariant 7).
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_CREATURE_TABLE_H
#define ENGINE_MOBS_CREATURE_TABLE_H

#include <string>
#include <vector>

namespace worldgen { struct ArchetypeTable; }

namespace engine {
namespace mobs {

// How a creature gets about (ADR-007 decision 4). Only ground and
// amphibious move in phase 4; the rest need 3D navigation.
enum class Move { Ground, Amphibious, WaterSurface, WaterDeep, Air };
// The navmesh it walks: small (r 0.3, h 0.8), human (r 0.4, h 1.8), large.
enum class Size { Small, Human, Large };

struct CreatureKind {
    std::string id, name;
    std::string model = "player";        // the only body there is, for now
    float tint[3] = {1.0f, 1.0f, 1.0f};  // multiplies the body's albedo (linear)
    Move move = Move::Ground;
    Size size = Size::Human;
    float health = 40.0f;
    float walk_speed = 1.4f, run_speed = 5.0f;
    // Perception (engine/mobs/perception.h).
    float hear_range_m = 20.0f, hear_threshold_db = 34.0f;
    float view_range_m = 50.0f, view_cone_deg = 45.0f, dark_gain = 1.0f;
    std::string script;                  // game/scripts/<script>
};

struct SpawnRule {
    int kind = -1;
    int biome = -1;                      // archetype index
    std::string biome_name;
    float min_weight = 0.6f;             // the biome's blended weight at the point
    int max_per_zone = 2, max_active = 4;
    float interval_h = 1.0f;             // game hours between a zone's attempts
    float chance = 0.35f;
    float hour_from = 0.0f, hour_to = 24.0f;   // of day; wraps when from > to
    int group_min = 1, group_max = 1;
    float min_player_m = 20.0f, min_same_m = 24.0f;
};

struct ZoneTuning {
    float size_m = 64.0f;      // the forage and rock tiles' grid
    float active_m = 128.0f;   // a zone whose centre is this near the player wakes
    float release_m = 160.0f;  // ... and freezes again past this (hysteresis)
    int   max_drawn = 31;      // bodies drawn, nearest first
};

class CreatureTable {
public:
    bool load(const char* path, const worldgen::ArchetypeTable& biomes, std::string& error);
    int find(const std::string& id) const;   // -1 if none

    std::vector<CreatureKind> kinds;
    std::vector<SpawnRule> spawns;
    ZoneTuning zones;
};

}  // namespace mobs
}  // namespace engine

#endif
