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
    void remove_chunk(const ChunkKey& key);
    size_t body_count() const { return bodies_.size(); }

    // Place the character. Used once at the start and never again -- moving it
    // by hand afterwards would defeat the thing being measured.
    void place_character(float x, float y, float z);

    // vx/vz is the desired horizontal velocity in metres per second.
    void update(float dt, float vx, float vz, bool jump);

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
    float radius_ = 0.35f;
    float half_height_ = 0.80f;
};

}  // namespace engine

#endif  // ENGINE_PHYSICS_WORLD_H
