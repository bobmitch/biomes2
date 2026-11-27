// ---------------------------------------------------------------------------
// The creatures in the world (ADR-007, decisions 1 and 2): zones, spawning,
// freezing, saving. No GPU, no physics, no window: the caller hands in the
// player, the clock and an environment of queries (standable ground, biome
// weights, whether the player could see a point), so a headless test drives
// it as the window does.
//
// ZONES are the 64 m grid the forage and rock tiles stream on. A zone whose
// centre comes within active_m of the player is ACTIVE: it spawns, and the
// creatures in it are live. Past release_m it FREEZES: every creature
// standing in it is taken out of the live list exactly as it is -- where it
// stands, facing, health, state, home -- and kept in the zone's record,
// along with how long each spawn rule still has to wait. Coming back thaws
// them as they were. A frozen zone's clock does not run: nothing is caught
// up, so a long absence does not return to a crowd, and leaving does not
// shake a chase. A creature belongs to the zone it is IN, not where it was
// born: one that wanders across a border freezes with its new zone.
//
// SPAWNING: per active zone and rule, an attempt every interval_h game
// hours, with the rule's chance, under its caps (in the zone; over every
// active zone), its hours, its distances, and out of the player's sight.
// The rolls are a hash of the world, the zone, the rule and the zone's
// attempt count, so a test reproduces.
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_CREATURES_H
#define ENGINE_MOBS_CREATURES_H

#include "engine/mobs/creature_table.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {
namespace mobs {

// One creature: everything a save keeps.
struct Mob {
    uint32_t id = 0;
    int kind = -1;
    float pos[3] = {0, 0, 0};     // feet
    float yaw = 0.0f;             // Camera's convention: forward (sin, 0, cos)
    float health = 0.0f;
    float home[3] = {0, 0, 0};    // where it spawned
    int   state = 0;              // the behaviour's (phase 5); 0 idle
    float state_t = 0.0f;         // seconds in that state
    double born = 0.0;            // game seconds
    // Where it is going (ADR-007 phase 4): gait 0 standing, 1 walking, 2
    // running, towards `goal`. Saved, so a frozen chase resumes.
    int   gait = 0;
    float goal[3] = {0, 0, 0};
    // A remembered place, the behaviour's (phase 5): where the player was
    // last seen or heard. Saved with the rest.
    float mark[3] = {0, 0, 0};
};

struct ZoneKey {
    int x = 0, z = 0;
    bool operator==(const ZoneKey& o) const { return x == o.x && z == o.z; }
};
struct ZoneKeyHash {
    size_t operator()(const ZoneKey& k) const {
        return std::hash<long long>()((static_cast<long long>(k.x) << 32) ^ (k.z & 0xFFFFFFFFll));
    }
};

// What the spawner asks of the world.
struct SpawnEnv {
    // Standable ground under (x, z) for a body: its height, or false (water,
    // too steep, a channel, a trunk or rock in the way).
    std::function<bool(float x, float z, float& y)> ground;
    // The blended weight of biome b at (x, z), 0..1.
    std::function<float(float x, float z, int biome)> biome_weight;
    // Whether the player could see a body standing with these feet and head
    // (in the view and a clear line to either).
    std::function<bool(const float feet[3], const float head[3])> visible;
};

class Creatures {
public:
    void set_table(const CreatureTable* t) { table_ = t; }
    const CreatureTable* table() const { return table_; }
    // The world's identity: seeds the rolls; a save from another is refused.
    void set_world(uint64_t field_id);
    void clear();

    // A frame: wake and freeze zones round the player, run the spawn
    // attempts that came due since the last call (game seconds `now`).
    void update(double now, const float player[3], const SpawnEnv& env);

    ZoneKey zone_of(float x, float z) const;
    void zone_centre(const ZoneKey& k, float& x, float& z) const;
    bool active(const ZoneKey& k) const;

    // A creature put down by hand (a test, a script): live at once, in
    // whatever zone it stands. Its id.
    uint32_t spawn_at(int kind, const float pos[3], float yaw, double now);
    Mob* find_live(uint32_t id);

    const std::vector<Mob>& live() const { return live_; }
    std::vector<Mob>& live() { return live_; }
    // Every frozen creature, for a report.
    size_t frozen_count() const;
    size_t active_zones() const { return active_.size(); }
    size_t zones() const { return zones_.size(); }
    int live_of_kind(int kind) const;
    int in_zone(const ZoneKey& k, int kind) const;
    // For a test: a zone's remaining wait for rule r (game seconds), or -1.
    double zone_wait(const ZoneKey& k, int rule) const;

    // Counters since the last clear (reports).
    struct Stats {
        uint64_t attempts = 0, rolls_won = 0, spawned = 0;
        uint64_t no_point = 0, capped = 0, out_of_hours = 0;
        uint64_t frozen = 0, thawed = 0;
    };
    const Stats& stats() const { return stats_; }

    // Binary, kinds by id so the table can change under a save; under the
    // world's field id. Live creatures are written as frozen in their zones;
    // a load freezes everything and the next update thaws what is near.
    bool save(const char* path, std::string& error) const;
    bool load(const char* path, std::string& error);

private:
    struct ZoneRec {
        std::vector<Mob> frozen;
        std::vector<double> wait;   // per rule: game seconds to the next attempt
        uint32_t attempts = 0;
    };
    ZoneRec& record(const ZoneKey& k);
    void freeze(const ZoneKey& k);
    void thaw(const ZoneKey& k);
    void try_spawn(const ZoneKey& k, ZoneRec& z, int rule, double now, const float player[3], const SpawnEnv& env);
    uint64_t roll(const ZoneKey& k, uint64_t a, uint64_t b) const;

    const CreatureTable* table_ = nullptr;
    uint64_t world_ = 0;
    std::unordered_map<ZoneKey, ZoneRec, ZoneKeyHash> zones_;
    std::unordered_map<ZoneKey, bool, ZoneKeyHash> active_;
    std::vector<Mob> live_;
    uint32_t next_id_ = 1;
    double last_ = -1.0;
    Stats stats_;
};

}  // namespace mobs
}  // namespace engine

#endif
