// ---------------------------------------------------------------------------
// Jolt, fed by the readback ring.
//
// One static HeightFieldShape body per resident chunk, built from the heights
// the meshing kernel extracted, added when the readback lands and removed when
// the chunk is retired. The character is a CharacterVirtual on top of them.
//
// A heightfield cannot express an overhang -- a column has exactly one surface
// -- so the sphere in the trivial field is walked over rather than under. B5's
// MeshShape is where that stops being an approximation, and the chunk
// classification it needs is precisely "does this chunk need the expensive
// shape or will the cheap one do".
// ---------------------------------------------------------------------------
#ifndef ENGINE_PHYSICS_WORLD_H
#define ENGINE_PHYSICS_WORLD_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/streaming/streamer.h"
#include "engine/render/water_wave.h"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <vector>
#include <memory>
#include <unordered_map>

namespace engine {

// Two object layers is all this needs: terrain that never moves and a
// character that does. B6's edits and B7's props arrive as more of the same.
namespace Layers {
static const JPH::ObjectLayer NON_MOVING = 0;
static const JPH::ObjectLayer MOVING = 1;
static const JPH::uint NUM_LAYERS = 2;
}  // namespace Layers

namespace BroadPhaseLayers {
static const JPH::BroadPhaseLayer NON_MOVING(0);
static const JPH::BroadPhaseLayer MOVING(1);
static const JPH::uint NUM_LAYERS = 2;
}  // namespace BroadPhaseLayers

// What the character is in. Wading is ground movement slowed by the water;
// swimming is buoyancy instead of ground.
enum class Medium { Dry, Wading, Swimming };

// The sea, as physics sees it. There is NO water body in Jolt: the sea is a
// function, and the character asks it each step. That keeps it out of the
// broadphase, out of the chunk bodies and -- the constraint that matters --
// out of residency: streamer.h never learns the sea exists, so --band-report
// keeps measuring the band against the terrain alone.
struct SeaParams {
    bool  on = false;
    float level = 0.0f;     // world y of still water
    float state = 0.0f;     // 0 calm .. 1 storm, the weather's dial (reports only)
    WwSea sea = {};         // the weather's sea (water_wave.h): what the swimmer floats on
    WwSwash swash = {};     // ...and the water running up the beach (ww_swash)
    float time = 0.0f;      // the SAME clock the frame's water draw is given
};

class PhysicsWorld {
public:
    // Declared here and defined in the .cpp, because unique_ptr<Impl> needs a
    // destructor where Impl is a complete type and Impl is deliberately not.
    PhysicsWorld();
    ~PhysicsWorld();

    bool init(std::string& error);
    void shutdown();

    // A chunk's heights have arrived. Replaces any shape already standing for
    // that chunk, so a remesh at a new LOD swaps cleanly.
    void set_chunk(const ChunkKey& key, const float origin[3], float voxel,
                   const float* heights);
    // The chunk's real triangles, for one a heightfield cannot represent.
    // Replaces whatever shape it had, so an overhang chunk starts out filled
    // in solid and becomes correct four frames later.
    void set_chunk_mesh(const ChunkKey& key, const float* verts, unsigned nverts,
                        const unsigned* idx, unsigned nidx);

    void remove_chunk(const ChunkKey& key);

    // Props: trees. A handle is the body's index-and-sequence number, stable
    // for the body's life.
    //
    // A static capsule between two world points: a standing trunk, a stump.
    uint32_t add_static_capsule(const float a[3], const float b[3], float radius);
    // A felled tree, mid-fall: a trunk-thick capsule the whole tree's length
    // on the local axis, posed at (pos, rot) and already turning at `spin`
    // rad/s. It is TWO capsules, not one, and that is about mass rather than
    // shape: the bole [y0, y1] is solid wood at 650 kg/m^3, the part above
    // it -- branches, twigs and leaves, mostly air -- carries `crown_mass` of
    // the whole body's mass and no more. Drawn as one length of trunk either
    // way, since the two overlap by a radius and the hull is the same
    // cylinder. Modelled as ONE uniform capsule the crown weighed as much as
    // the trunk under it: the centre of mass sat at half the height instead
    // of a third, which is a slow fall and far too much angular momentum to
    // shed once the thing is on the ground.
    // `asleep` adds it without waking it, for a log loaded from a save.
    uint32_t add_log(const float pos[3], const float rot[4], float trunk_r, float y0, float y1,
                     float top, float crown_mass, const float spin[3], bool asleep);
    // A piece of a felled trunk -- half a log, a billet of firewood: one
    // dense capsule between two points in the body's OWN frame, since the
    // trunk's axis wanders and a piece from up the tree is not on the
    // model's centre line. Given a velocity as well as a spin, so a half
    // that breaks off a rolling log carries its motion.
    uint32_t add_piece(const float pos[3], const float rot[4], float radius, const float a[3],
                       const float b[3], const float vel[3], const float spin[3], bool asleep);
    // A clump of crown falling away from a felled tree: one light sphere,
    // temporary. Low density, because a crown is mostly air -- it should
    // tumble and settle, not bounce the log it came off.
    uint32_t add_debris(const float pos[3], const float rot[4], float radius, float density,
                        const float vel[3], const float spin[3]);
    // Rocks (layer 9): a static mesh shape built once per model, in the
    // model's own unit space, and static bodies that scale and place it.
    // add_prop_mesh returns the shape's index, or -1.
    int      add_prop_mesh(const float* verts, unsigned nverts, const unsigned* idx, unsigned nidx);
    uint32_t add_static_prop(int mesh, const float pos[3], const float rot[4], float scale);
    const std::string& prop_error() const { return prop_error_; }
    void     remove_body(uint32_t handle);
    // Where a body is: its shape origin and rotation. `active` false once
    // Jolt has put it to sleep, which is what "came to rest" means.
    bool     body_pose(uint32_t handle, float pos[3], float rot[4], bool& active) const;
    // How fast it is going, and how fast it is turning. A falling tree's
    // momentum is what it does to the tree it lands on.
    bool     body_velocity(uint32_t handle, float lin[3], float ang[3]) const;
    // ... and how fast one particular world point of it is moving: a tree
    // turning about its butt has a still base and a fast top.
    bool     body_point_velocity(uint32_t handle, const float p[3], float v[3]) const;
    void     set_body_velocity(uint32_t handle, const float lin[3], const float ang[3]);
    // The mass a blow at `point` pushing along `dir` actually meets: the
    // collision reduced mass, 1 / (1/m + (r x n) . I^-1 (r x n)) with r from
    // the body's centre of mass. It is NOT the body's mass, and for a long
    // turning thing it is not close to it -- a light crown eight metres off
    // the centre of a falling trunk meets a small fraction of the tree,
    // which is why a branch snaps instead of felling what it lands on, and
    // why pushing a log end-on meets the whole log. This is what "the
    // momentum it had at the point it hit" means for a body that is turning
    // as well as moving.
    bool     body_impact_mass(uint32_t handle, const float point[3], const float dir[3],
                              float& mass) const;
    // Drag, changed after the fact. A tree going over on its stump has
    // nothing damping its rotation and the damping a LOG wants would stop
    // it leaving the vertical at all; a log lying on the ground wants the
    // damping back.
    void     set_body_damping(uint32_t handle, float linear, float angular);
    // Stop it dead and put it to sleep. A capsule on a slope rolls for ever
    // whatever the damping -- gravity always wins the balance -- so a log
    // that has slowed to a creep is told it has arrived. It stays dynamic:
    // walk into it, chop it or split it and Jolt wakes it again.
    void     rest_body(uint32_t handle);

    // A HINGE between a dynamic body and an anchor, about `axis` through
    // `point` in world space. A felled tree needs one at its stump: left as
    // a free body it rotates about its own CENTRE OF MASS, which is halfway
    // up the trunk, so the butt swings out sideways the instant it is cut
    // and the whole thing reads as a kick rather than a topple. Hinged, it
    // is a pendulum about its base -- barely moving at first and
    // accelerating as it goes over, which is what a falling tree does.
    // Release it once the trunk is far enough over to slip off the stump.
    uint32_t add_hinge(uint32_t body, uint32_t anchor, const float point[3], const float axis[3]);
    void     remove_hinge(uint32_t handle);

    // Contacts, for the things that have to know the MOMENT they touched
    // something: a falling tree's canopy, which breaks off against the first
    // thing it meets. Only watched bodies are recorded -- with none watched
    // the listener returns on one atomic read, so a world with nothing in
    // the air pays nothing for this. A speculative contact (Jolt reports
    // one before the surfaces actually meet) is not a touch. The list is the
    // last step's and update() clears it.
    void watch_contacts(uint32_t handle, bool on);
    struct Touch {
        uint32_t body = 0;               // the watched body
        uint32_t other = 0;              // what it met
        bool     prop = false;           // ... a trunk, stump, rock or log; else terrain
        float    point[3] = {0, 0, 0};   // where, in world metres
    };
    const std::vector<Touch>& touches() const { return touches_; }
    // The contact listener's way in. Not for callers.
    bool watching_contacts() const { return watching_.load(std::memory_order_relaxed) != 0; }
    void note_contact(uint32_t h1, uint32_t h2, const float point[3]);

    // How hard the character may shove a dynamic body, in newtons. Jolt's
    // own default is 100 N, which will not move anything made of wood: a
    // 130 kg round of firewood needs about 380 N to roll and 1.1 kN to
    // slide. Set it every frame -- a nudge costs nothing, a heave costs
    // stamina, and main.cpp decides which the player is doing.
    void     set_push_strength(float newtons);
    // What the character is leaning on, as of the last update(): the
    // heaviest dynamic prop it is touching AND moving into, and how fast
    // that prop is actually going. `on` false when it is pushing nothing.
    struct Push {
        bool     on = false;
        uint32_t body = 0;
        float    mass = 0.0f;    // kg
        float    speed = 0.0f;   // m/s, what it is actually doing
    };
    const Push& push() const { return push_; }

    // A BUMP: walking into something loose hands it the character's
    // momentum. The sustained push above is a FORCE, which Jolt caps at
    // mMaxStrength * dt and the ground's friction cancels outright below
    // the breakaway threshold -- so walking into a log a hundred times did
    // exactly what walking into it once did. An impulse has no threshold:
    // it changes the log's velocity there and then and friction only
    // decelerates it afterwards, so every knock moves it something and the
    // distance goes as the SQUARE of how fast you came in. That quadratic
    // is the mechanic -- nudge it at a walk and almost nothing happens,
    // back off and charge it and it goes -- and it needs no new key, since
    // walking, jogging and sprinting are already three different speeds.
    //
    // `k` is the share of the character's momentum handed over; 1 is the
    // honest value, because Jolt's character stops dead against a wall and
    // loses all of its own.
    void set_bump_transfer(float k) { bump_transfer_ = k; }
    struct Bump {
        uint32_t body = 0;
        float    speed = 0.0f;    // closing speed along the contact, m/s
        float    impulse = 0.0f;  // N s handed over
        float    mass = 0.0f;     // kg of what took it
    };
    const std::vector<Bump>& bumps() const { return bumps_; }
    // Only a NEW contact bumps, and then not again for this long: a contact
    // that chatters on and off must not machine-gun the log, and leaning on
    // one must not be the same as walking into it over and over.
    static constexpr float kBumpCooldown = 0.35f;
    static constexpr float kBumpMinSpeed = 0.6f;   // slower is a lean, not a knock
    size_t   prop_count() const { return props_.size(); }
    // Any ray: metres to the first body, -1 for none; `prop` 1 if that body
    // is a prop (a trunk, a stump, a log) rather than terrain.
    float    probe_ray(const float from[3], const float dir[3], float len, int& prop) const;
    // The character's active contacts: how many, and how many are props.
    void     character_contacts(int& total, int& props) const;

    size_t body_count() const { return bodies_.size(); }
    size_t mesh_body_count() const { return mesh_bodies_; }

    // Place the character. Used once at the start and never again -- moving it
    // by hand afterwards would defeat the thing being measured.
    void place_character(float x, float y, float z);
    // Straight up at inVy. The only way to ask whether the UNDERSIDE of an
    // overhang is collision at all: walking cannot reach it.
    void launch_character(float vy);

    // vx/vz is the desired horizontal velocity in metres per second. `swim`
    // is vertical intent in water, -1 dive to +1 surface; `jump` counts as +1
    // there, so one key gets you out of the water and over the lip.
    void update(float dt, float vx, float vz, bool jump, float swim = 0.0f);

    // The sea. Set every frame, because time and sea state move.
    void set_sea(const SeaParams& s) { sea_ = s; }
    // The GENERATED terrain's column height. Water is a half-space -- every
    // bit of air under sea level is water -- and this only weights its waves:
    // the deep sea's trains by the generated depth, and the beach's swash
    // (water_wave.h ww_sea_at), as water.hlsl draws them.
    void set_column_height(std::function<float(float, float)> fn) {
        column_height_ = fn;
    }

    // Diagnostics: what physics actually has. A ray straight up, once
    // ignoring back faces and once not, and what kind of body it met; and
    // what kind of body stands for a chunk. "The capsule went through it"
    // has several causes and these tell them apart.
    struct UpProbe {
        bool  hit_front = false, hit_any = false;
        float dist_front = 0.0f, dist_any = 0.0f;
        int   lod = -1;        // of the body the any-face ray hit
        int   is_mesh = -1;    // 1 mesh shape, 0 heightfield
    };
    UpProbe probe_up(float x, float y, float z, float len) const;
    // 0: no body, 1: heightfield, 2: mesh shape.
    int chunk_shape(const ChunkKey& k) const;

    Medium medium() const { return player_.medium; }
    // World y of the water's surface over the character, last step. Only
    // meaningful when sea_present().
    float  water_surface() const { return player_.water_y; }
    bool   sea_present() const { return player_.sea_here; }
    // Metres of the character below the surface, 0 dry to 2.3 fully under.
    float  submersion() const { return player_.submerged; }

    // Buoyancy balances gravity at this depth of the feet below the surface,
    // which puts the eye (1.6 m above the feet) 0.25 m clear of still water.
    static constexpr float kFloatDepth = 1.35f;
    // Hysteresis on Swimming, so floating at kFloatDepth in a swell does not
    // flicker between swimming and wading every wave.
    static constexpr float kSwimEnter = 1.05f;
    static constexpr float kSwimLeave = 0.85f;
    // Horizontal speed in water as a fraction of the asked-for speed.
    static constexpr float kSwimSpeed = 0.5f;

    // --- Creatures' bodies (ADR-007): the player's capsule and rules -- ground,
    // slopes, steps, buoyancy, swimming -- for as many as are near. Each is a
    // CharacterVirtual of its own; they and the player collide with each
    // other. step_walker moves one at once (ExtendedUpdate); update() then
    // steps the rest of the world.
    int    add_walker(float x, float y, float z);
    void   remove_walker(int id);
    void   place_walker(int id, float x, float y, float z);
    void   step_walker(int id, float dt, float vx, float vz, bool jump, float swim = 0.0f);
    bool   walker_position(int id, float out[3]) const;
    bool   walker_supported(int id) const;
    Medium walker_medium(int id) const;
    size_t walkers() const { return walker_state_.size(); }

    void  character_position(float out[3]) const;
    bool  character_grounded() const;
    bool  character_supported() const;   // on ground or on a steep slope
    // LOD of the chunk the character is standing on, or -1 for none. The
    // question "what is it actually standing on" is not answerable from the
    // position alone once several levels have bodies in the same place.
    int   character_ground_lod() const;
    float character_half_extent() const { return half_height_ + radius_; }
    float character_radius() const { return radius_; }

    // One character's water and motion state between steps.
    struct BodyState {
        Medium medium = Medium::Dry;
        bool   sea_here = false;
        float  water_y = 0.0f;
        float  submerged = 0.0f;
        float  water_rate = 0.0f;   // m/s the surface rises along the path
        // Applied AFTER the on-ground branch, which zeroes any upward
        // velocity it finds. Setting the velocity directly from outside is
        // silently undone on the next step.
        float  pending_launch = 0.0f;
    };

private:
    struct Impl;
    void step_body(void* character, BodyState& b, float dt, float vx, float vz, bool jump, float swim);
    std::unique_ptr<Impl> impl_;
    std::string prop_error_;   // why the last add_prop_mesh failed

    std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash> bodies_;
    std::unordered_map<uint32_t, JPH::BodyID> props_;
    size_t dynamic_props_ = 0;
    std::unordered_map<JPH::uint32, int> body_lod_;
    std::unordered_map<JPH::uint32, int> body_is_mesh_;
    size_t mesh_bodies_ = 0;
    BodyState player_;
    std::unordered_map<int, BodyState> walker_state_;
    int next_walker_ = 1;

    uint32_t  next_hinge_ = 0;
    Push      push_;
    std::vector<Bump>     bumps_;     // this frame's
    std::vector<uint32_t> contact_watch_;
    std::atomic<int>      watching_{0};   // read by every contact, on any thread
    mutable std::mutex    touch_mu_;      // Jolt calls back from its job threads
    std::vector<Touch>    touches_;
    std::vector<uint32_t> touched_;   // loose props in contact last frame
    std::unordered_map<uint32_t, float> bump_cd_;   // handle -> seconds until it may be bumped again
    float     bump_transfer_ = 1.0f;
    SeaParams sea_;
    std::function<float(float, float)> column_height_;

    float radius_ = 0.35f;
    float half_height_ = 0.80f;
};

}  // namespace engine

#endif  // ENGINE_PHYSICS_WORLD_H
