// ---------------------------------------------------------------------------
// Blows (ADR-007's "Later": combat). Who swings at whom, and what a blow
// does. No GPU, no window, no physics: the caller hands in the live
// creatures and where the player stands, so a headless test drives it as the
// window does (--combat-test).
//
// A CREATURE'S BLOW is in two parts, because a blow that lands the instant
// it is decided cannot be avoided. The script decides to swing (c:attack());
// `begin` starts a WINDUP of the kind's windup_s. When that runs out the
// blow LANDS: the player takes its damage only if they are still within
// reach_m, chest to chest, and still within arc_deg of the creature's
// facing -- so stepping back or round during the telegraph is a miss. Then
// recover_s before the next.
//
// THE PLAYER'S BLOW is one ray from the eye, as a chop is: `strike` takes
// the nearest creature whose body capsule it enters within the tool's reach
// and takes the tool's damage off it. A creature whose health reaches zero
// is the caller's to remove (Creatures::kill), which keeps the Lua hook and
// the message with the rest of the window's bookkeeping.
//
// Nothing here is saved. Health is (it is on the Mob), so a creature
// frozen half dead thaws half dead; a windup in flight is not -- a frozen
// creature's swing is forgotten, and it swings again when it thaws.
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_COMBAT_H
#define ENGINE_MOBS_COMBAT_H

#include "engine/mobs/creature_table.h"
#include "engine/mobs/creatures.h"

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace engine {
namespace mobs {

// A body, for the player's ray: a vertical capsule on the feet, by size
// class (the navmesh's agents).
void body_capsule(Size s, float& radius, float& height);

class Combat {
public:
    // What a landed blow does to the player. `from` is the creature's feet.
    struct Blow {
        uint32_t id = 0;
        int kind = -1;
        float damage = 0.0f;
        float stamina = 0.0f;
        float from[3] = {0, 0, 0};
    };
    using HitFn = std::function<void(const Blow&)>;

    // The player, as a blow finds them.
    struct Target {
        bool present = false;
        float feet[3] = {0, 0, 0};
        float chest = 1.2f;        // over the feet: what a blow and a ray aim at
    };

    void set_table(const CreatureTable* t) { table_ = t; }

    // Begin a swing: false if the kind cannot attack, or this one is still
    // winding up or recovering.
    bool begin(const Mob& m);
    bool winding_up(uint32_t id) const;   // the telegraph is running
    bool busy(uint32_t id) const;         // ... or recovering from one
    // Seconds until the blow lands, or 0.
    float windup_left(uint32_t id) const;

    // A frame: windups and recoveries run on, blows that come due land.
    // Creatures that are no longer live are forgotten.
    void update(float dt, const std::vector<Mob>& live, const Target& p, const HitFn& hit);

    // The player's swing, from `eye` along `dir` (unit): the nearest
    // creature it reaches. 0 for none. Takes `damage` off it; `died` says
    // its health reached zero (the caller removes it), and `point` is where
    // the ray met it.
    uint32_t strike(std::vector<Mob>& live, const float eye[3], const float dir[3], float reach,
                    float damage, bool& died, float point[3]);

    // Ids whose windup began since the last call (the animator's cue).
    std::vector<uint32_t> take_started();
    // Ids whose blow landed on the player since the last call, for a report.
    uint64_t blows_landed() const { return landed_; }
    uint64_t blows_missed() const { return missed_; }
    void clear();

private:
    struct Swing {
        float windup = 0.0f;    // > 0: in flight
        float recover = 0.0f;
    };
    const Swing* find(uint32_t id) const;

    const CreatureTable* table_ = nullptr;
    std::unordered_map<uint32_t, Swing> swings_;
    std::vector<uint32_t> started_;
    uint64_t landed_ = 0, missed_ = 0;
};

}  // namespace mobs
}  // namespace engine

#endif
