// ---------------------------------------------------------------------------
// Drawing and colliding the rocks of layer 9 (rock_field.h places them).
//
// The meshes are generated at startup (rock_mesh.h) into one vertex and one
// index buffer. Each frame the CPU culls the resident rocks into two lists
// in an upload buffer -- the camera's (frustum, draw distance, near or far
// level by size and distance) and the sun's (every rock within reach, far
// level) -- grouped by model and level, and draws each group with one
// DrawIndexedInstanced. They use the TERRAIN's root signature: the frame's
// constants, the shadow table and the scans are already bound when they
// draw, right after the terrain; t0 is rebound to each group's instances.
//
// Physics gets a static, scaled copy of a model's far-level mesh for every
// rock within kColliderRadius of the player, diffed by id like the trunks.
// ---------------------------------------------------------------------------
#ifndef ENGINE_ROCK_RENDERER_H
#define ENGINE_ROCK_RENDERER_H

#include "engine/rocks/rock_mesh.h"
#include "worldgen/world.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class PhysicsWorld;

struct RockGpu {
    float    pos[3];
    float    scale;
    float    rot[4];
    uint32_t biome;
    float    draw_dist;
    float    pad[2];
};
static_assert(sizeof(RockGpu) == 48, "RockInst in rocks.hlsl");

class RockRenderer {
public:
    static const UINT kFrames = 2;
    static const UINT kMaxMain = 16384, kMaxShadow = 8192;
    static constexpr float kDrawDist = 700.0f;       // metres
    static constexpr float kShadowBoulders = 200.0f; // reach of the sun's list
    static constexpr float kShadowOutcrops = 600.0f;
    static constexpr float kColliderRadius = 40.0f;

    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
              std::string& error);
    bool ready() const { return ready_; }

    // Cull `rocks` into this frame's lists.
    void prepare(UINT frame, const std::vector<worldgen::RockInstance>& rocks, const float cam[3],
                 const float view_proj[16]);
    // The camera's list. The terrain's root signature, constants, shadow CBV
    // and table must be bound.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);
    // The sun's list into the current cascade (its matrix already in the
    // root constants).
    void record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame);
    // The sun's list again, into the heightfield GI's top-down albedo map
    // (its matrix in the root constants). Also what a local light's shadow
    // face draws, through record_shadow.
    void record_top(ID3D12GraphicsCommandList* cmd, UINT frame);

    void sync_colliders(PhysicsWorld* physics, const std::vector<worldgen::RockInstance>& rocks,
                        unsigned long long version, float x, float z);

    // The model a rock draws and collides with (rock_model), for probes.
    const RockModel& model(uint32_t m) const { return meshes_.models[m]; }
    const RockMeshes& meshes() const { return meshes_; }
    size_t drawn() const { return drawn_; }
    size_t triangles() const { return triangles_; }
    size_t colliders() const { return colliders_.size(); }
    // How many models' physics shapes built (of meshes().models.size()).
    int shapes_built() const { int n = 0; for (int m : phys_mesh_) n += m >= 0; return n; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Group { uint32_t model, lod, first, count; };
    void bind_mesh(ID3D12GraphicsCommandList* cmd);
    void draw_groups(ID3D12GraphicsCommandList* cmd, UINT frame, const std::vector<Group>& groups, UINT base);

    bool ready_ = false;
    RockMeshes meshes_;
    ComPtr<ID3D12Resource> vb_, ib_, inst_[kFrames];
    RockGpu* inst_cpu_[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
    D3D12_INDEX_BUFFER_VIEW ibv_ = {};
    ComPtr<ID3D12PipelineState> pso_, shadow_pso_, top_pso_;
    std::vector<Group> main_[kFrames], shadow_[kFrames];
    size_t drawn_ = 0, triangles_ = 0;

    // Physics: one shape per model, bodies by rock id.
    std::vector<int> phys_mesh_;
    PhysicsWorld* phys_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> colliders_;
    std::unordered_map<uint32_t, float>    collider_y_;   // the height each was built at
    float last_x_ = 1e30f, last_z_ = 1e30f;
    unsigned long long seen_version_ = ~0ull;
};

}  // namespace engine

#endif  // ENGINE_ROCK_RENDERER_H
