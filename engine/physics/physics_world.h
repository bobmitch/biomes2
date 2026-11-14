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

#include <Jolt/Jolt.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>

#include <functional>
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
    float state = 0.0f;     // 0 calm .. 1 storm, the renderer's dial
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
    // The BASE terrain's column height, which is the shader's rule for where
    // sea exists at all: water.hlsl clips wherever real_terrain_height is
    // above sea level. Using the same rule is what stops a pit dug on a beach
    // filling with water nobody can see, or a cave under the sea floor being
    // dry while the sea is drawn over it.
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

    Medium medium() const { return medium_; }
    // World y of the displaced surface above the character, last step. Only
    // meaningful when sea_present().
    float  water_surface() const { return water_y_; }
    bool   sea_present() const { return sea_here_; }
    // Metres of the character below the surface, 0 dry to 2.3 fully under.
    float  submersion() const { return submerged_; }

    // Buoyancy balances gravity at this depth of the feet below the surface,
    // which puts the eye (1.6 m above the feet) 0.25 m clear of still water.
    static constexpr float kFloatDepth = 1.35f;
    // Hysteresis on Swimming, so floating at kFloatDepth in a swell does not
    // flicker between swimming and wading every wave.
    static constexpr float kSwimEnter = 1.05f;
    static constexpr float kSwimLeave = 0.85f;
    // Horizontal speed in water as a fraction of the asked-for speed.
    static constexpr float kSwimSpeed = 0.5f;

    void  character_position(float out[3]) const;
    bool  character_grounded() const;
    bool  character_supported() const;   // on ground or on a steep slope
    // LOD of the chunk the character is standing on, or -1 for none. The
    // question "what is it actually standing on" is not answerable from the
    // position alone once several levels have bodies in the same place.
    int   character_ground_lod() const;
    float character_half_extent() const { return half_height_ + radius_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::unordered_map<ChunkKey, JPH::BodyID, ChunkKeyHash> bodies_;
    std::unordered_map<JPH::uint32, int> body_lod_;
    std::unordered_map<JPH::uint32, int> body_is_mesh_;
    size_t mesh_bodies_ = 0;
    // Applied in update() AFTER the on-ground branch, which zeroes any
    // upward velocity it finds. Setting the velocity directly from outside
    // is silently undone on the next step.
    float  pending_launch_ = 0.0f;

    SeaParams sea_;
    std::function<float(float, float)> column_height_;
    Medium medium_ = Medium::Dry;
    bool   sea_here_ = false;
    float  water_y_ = 0.0f;
    float  submerged_ = 0.0f;
    float  water_rate_ = 0.0f;   // m/s the surface rises along the path

    float radius_ = 0.35f;
    float half_height_ = 0.80f;
};

}  // namespace engine

#endif  // ENGINE_PHYSICS_WORLD_H
