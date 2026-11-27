// ---------------------------------------------------------------------------
// What creatures notice and decide (ADR-007, decisions 3 and 6).
//
// PERCEPTION is C++, on a think tick -- 5 a second for a creature with a
// physics body (near the player), 1 a second further out, staggered by id.
// Each tick it hears the player (their level in dB through
// perception::hears, against its range and threshold) and sees them (its
// cone, its range x their visibility x the light on them, and a clear ray
// from its eyes to their chest).
//
// BEHAVIOUR is Lua: a kind's `script` (game/scripts/<script>) returns a
// table of hooks, called with a creature handle `c` --
//   on_wake(c)            when it becomes live this session (spawned,
//                         thawed, loaded): its saved state stands
//   on_see(c, ev)         every tick it sees the player: ev.x/y/z (feet), ev.dist
//   on_lose(c, ev)        the tick it stops seeing them: ev.x/y/z last seen
//   on_hear(c, ev)        every tick it hears them and does not see them:
//                         ev.x/y/z, ev.dist, ev.db (the level reaching it)
//   on_hurt(c, ev)        the moment it is struck, whatever its tick:
//                         ev.damage, ev.health (what is left), ev.x/y/z
//                         (the blow came from there), ev.dist
//   on_think(c, dt)       every tick, after the events
// -- and c's methods: id, kind, pos, home, yaw, state, set_state (an
// integer, saved; the timer restarts on a change), state_time, mark and
// set_mark (a saved place), move_to(x, y, z, "walk"|"run"), stop, moving,
// face(x, z), player (feet; nil when there is no body), player_distance,
// random_point(radius) (on its mesh near home), random, log, and for
// fighting: health, max_health, attack (begin a swing: false if it cannot,
// or is still winding up or recovering), attacking (its windup is running),
// attack_reach (its kind's, so a script need not repeat the number). One Lua state
// for every creature; its errors go to build/creature_log.txt and leave
// the creature standing.
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_MINDS_H
#define ENGINE_MOBS_MINDS_H

#include "engine/mobs/combat.h"
#include "engine/mobs/creatures.h"
#include "engine/mobs/perception.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct lua_State;

namespace engine {
namespace nav { class NavWorld; }
namespace mobs {

class Locomotion;

class Minds {
public:
    // The player as perceived this frame.
    struct Player {
        bool  present = false;      // a body on the ground (not flying)
        float feet[3] = {0, 0, 0};
        float level_db = perception::kSilent;
        float visibility = 0.0f;
        float lux = 0.0f;
    };
    // A ray from `a` to `b` is clear (terrain, trunks, rocks).
    using ClearFn = std::function<bool(const float a[3], const float b[3])>;

    ~Minds();
    // Loads every kind's script. False (and `error`) on the first that does
    // not load; the rest still do.
    bool init(const CreatureTable* t, Creatures* c, Locomotion* loco, nav::NavWorld* nav, Combat* combat,
              std::string& error);
    void shutdown();
    bool scripted(int kind) const { return kind >= 0 && size_t(kind) < scripted_.size() && scripted_[size_t(kind)]; }
    perception::LightCurve& light() { return light_; }

    // A frame: perception and the hooks for every creature whose tick came.
    void update(float dt, const Player& p, const ClearFn& clear);
    // It has just been struck from `from`: on_hurt at once, not on its tick
    // (a creature that learns it is being hit a second later is a creature
    // you can kill from behind without it ever turning round).
    void hurt(Mob& m, float damage, const float from[3]);
    // Forget every creature's runtime (a load: each wakes again).
    void clear() { minds_.clear(); }

    // Per creature, this session.
    struct Mind {
        float until = 0.0f;         // seconds to the next tick
        float since = 0.0f;         // seconds since the last
        bool  awake = false;        // on_wake called
        bool  seeing = false, hearing = false;
        float seen[3] = {0, 0, 0};  // where the player was last seen
        uint32_t ticks = 0;
        uint32_t rolls = 0;         // c:random()'s counter
    };
    const Mind* mind(uint32_t id) const;
    size_t errors() const { return errors_; }

private:
    void call(int kind, const char* hook, Mob& m, int nev, const float* ev, const char* const* names);
    void log(const std::string& s);

    const CreatureTable* table_ = nullptr;
    Creatures* creatures_ = nullptr;
    Locomotion* loco_ = nullptr;
    nav::NavWorld* nav_ = nullptr;
    Combat* combat_ = nullptr;
    lua_State* L_ = nullptr;
    std::vector<bool> scripted_;
    std::unordered_map<uint32_t, Mind> minds_;
    perception::LightCurve light_;
    Player player_;
    size_t errors_ = 0;

    friend struct MindsApi;
};

}  // namespace mobs
}  // namespace engine

#endif
