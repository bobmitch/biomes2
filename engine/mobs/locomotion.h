// ---------------------------------------------------------------------------
// How creatures move (ADR-007, decision 5). Every live creature with a gait
// follows a navmesh path to its goal: corners from NavWorld::path, re-planned
// when the goal moves or every couple of seconds, at its kind's walk or run
// speed, steering a little apart from its neighbours.
//
// NEAR the player (inside 34 m, out again past 38) a creature has a physics
// body -- the player's capsule and rules, PhysicsWorld::add_walker -- and the
// path's direction is the velocity it asks for: it collides with the
// terrain, trunks, rocks, logs, the player and each other, wades, and floats
// and swims as the player does. FURTHER OUT there is no physics: it slides
// along the navmesh surface (Detour's moveAlongSurface), its feet at the
// mesh's height, or at the player's floating depth in a swim area. The
// hand-over keeps the position: a body is made where the creature stands.
//
// What is not saved lives here (paths, bodies, measured speed); what is --
// gait and goal -- is on the Mob.
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_LOCOMOTION_H
#define ENGINE_MOBS_LOCOMOTION_H

#include "engine/mobs/creatures.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace engine {
class PhysicsWorld;
namespace nav { class NavWorld; }

namespace mobs {

class Locomotion {
public:
    struct Tuning {
        float near_in = 34.0f, near_out = 38.0f;   // physics bodies inside, released past
        float arrive = 0.6f;                       // metres from the goal that is there
        float corner = 0.5f;                       // ... from a corner that is passed
        float repath_near = 1.5f, repath_mid = 3.0f;
        float turn_rate = 8.0f;                    // rad/s
        float separation = 1.2f;                   // metres kept between creatures (mid range)
        float float_depth = 1.35f;                 // a swimmer's feet under the surface (physics' kFloatDepth)
        float swim_speed = 0.5f;                   // of the asked-for speed, mid range (physics' kSwimSpeed)
    };
    // Per creature, this session.
    struct Body {
        int walker = 0;                 // physics body, near only
        bool close = false;
        std::vector<float> corners;     // the path, xyz each
        size_t next = 0;
        float path_goal[3] = {0, 0, 0};
        float repath = 0.0f;
        bool  path_complete = false;
        // What it did, for the animator.
        float speed = 0.0f, vertical = 0.0f;
        bool  grounded = true, swimming = false;
        float stuck = 0.0f;             // seconds asked to move and not moving
        float idle = 0.0f;              // seconds standing (the stand-in wander's clock)
        bool  facing = false;           // turning, standing, to face_yaw
        float face_yaw = 0.0f;
    };

    void set(const CreatureTable* t, nav::NavWorld* nav, PhysicsWorld* physics, float sea_level) {
        table_ = t;
        nav_ = nav;
        physics_ = physics;
        sea_ = sea_level;
    }
    Tuning& tuning() { return tuning_; }

    // A frame: tiers, paths, steering, movement. dt 0 (paused) moves nothing.
    void update(float dt, std::vector<Mob>& live, const float player[3]);
    // Until behaviour scripts exist (phase 5): a creature standing still
    // for 5-15 s walks to a random point of the mesh within 15 m of home.
    void wander(float dt, std::vector<Mob>& live, double now);
    // Turn a creature, standing, to face (x, z) (a behaviour's "look").
    void face(const Mob& m, float x, float z);
    // Kinds whose behaviour is a script: wander() leaves them alone.
    std::vector<bool> skip_wander;
    // Every body back to physics (a load, a new world).
    void clear();

    const Body* body(uint32_t id) const;
    size_t near_count() const;
    unsigned short include_for(int kind) const;

private:
    const CreatureTable* table_ = nullptr;
    nav::NavWorld* nav_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    float sea_ = 0.0f;
    Tuning tuning_;
    std::unordered_map<uint32_t, Body> bodies_;
};

}  // namespace mobs
}  // namespace engine

#endif
