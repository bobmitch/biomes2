// ---------------------------------------------------------------------------
// Glowing blue mushrooms in the placed caves: a collectable for testing
// emitters, and the first thing the player can pick up (E). No inventory
// yet: a picked mushroom is gone and counted.
//
// PLACEMENT is the engine's, never the golden hash's: clusters on the floors
// of layer 7's caves (worldgen::SeedWorld::caves, their capsules in
// entrances[]), a pure function of the cave's id, found when the camera
// first comes within reach of a cave (a few ms of field evaluations a cave)
// by marching the edited field down from each capsule's axis. archetypes.json
// `glow_mushrooms` tunes it; presentation only.
//
// LIGHT: each cluster is one local light (ADR-004 L3) at its middle, a dim
// steady blue, its lumens the mushrooms left times lumens_each, shadowed
// like any other so it does not light through rock -- while it is among
// the 24 most important (lumens over distance squared). Past that it lights
// through rock within its few metres, and the near field's probes leave it
// out (gaps.md). Only clusters within kLightRadius of the camera are lit.
//
// DRAWING: three procedural models (a stem and a domed cap, gills under
// it), instanced on the TERRAIN's root signature after the rocks, as the
// rocks are: lit by the sun, sky and local lights, plus their own glow into
// the ambient target with no AO. They cast no shadows.
// ---------------------------------------------------------------------------
#ifndef ENGINE_GLOW_MUSHROOMS_H
#define ENGINE_GLOW_MUSHROOMS_H

#include "engine/render/local_lights.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace worldgen { struct SeedWorld; }

namespace engine {

struct MushroomGpu {
    float pos[3];
    float scale;
    float rot[4];      // quaternion
    float glow[3];     // emitted radiance at full glow, nits (linear rgb)
    float pad;
};
static_assert(sizeof(MushroomGpu) == 48, "MushInst in mushrooms.hlsl");

class GlowMushrooms {
public:
    static const UINT kFrames = 2;
    static const UINT kMaxDrawn = 8192;
    static constexpr float kDrawDist = 80.0f;     // metres, flat
    static constexpr float kLightRadius = 70.0f;  // clusters lit round the camera
    static constexpr float kGenReach = 160.0f;    // a cave is populated within this of its mouth or chamber, + its length

    struct Config {
        float per_chamber = 3.0f;        // clusters per chamber capsule (a chance for the fraction)
        float tunnel_chance = 0.3f;      // a cluster per tunnel capsule
        int   cluster_min = 3, cluster_max = 9;
        float cluster_radius = 0.6f;     // m
        float size_min = 0.7f, size_max = 1.5f;
        float colour[3] = {0.35f, 0.75f, 1.0f};   // sRGB as typed
        float nits = 3.0f;               // the gills' glow; the cap shows about half
        float lumens_each = 4.0f;
        float range = 5.0f;              // m
        std::string item = "glow_mushroom";   // what a picked one gives (game/data/items.json)
    };

    // The cluster's floor, marched down from `from_y` through the edited
    // field (Renderer::ground_below); `from_y` back if it starts in rock.
    using GroundFn = std::function<float(float x, float z, float from_y)>;

    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
              const char* archetypes_path, std::string& error);
    bool ready() const { return ready_; }
    void set_world(const worldgen::SeedWorld* world) { world_ = world; generated_.clear(); clusters_.clear(); dirty_ = true; }

    // Populates the caves the camera has come near (at most `max_caves` this
    // call; a still passes a large number).
    void update(const float cam[3], const GroundFn& ground, int max_caves = 1);
    // This frame's instances (frustum, draw distance) into the upload buffer.
    void prepare(UINT frame, const float cam[3], const float view_proj[16]);
    // The terrain's root signature, constants, shadow CBV and table bound.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);
    // The lit clusters' lights round the camera, ids stable per cluster.
    void lights(const float cam[3], std::vector<LocalLight>& out) const;

    // The mushroom nearest along the ray from `eye` (the camera) along
    // `dir`, within `reach` of `hand` (the player's head) and in front of
    // `occluder` metres of ray (the terrain it meets). Picks it: true.
    bool pick(const float eye[3], const float dir[3], const float hand[3], float reach, float occluder);

    size_t picked() const { return picked_count_; }
    size_t mushrooms() const { return n_mushrooms_; }
    size_t clusters() const { return clusters_.size(); }
    size_t drawn() const { return drawn_; }
    size_t caves_populated() const { return n_generated_; }
    // The caps of the unpicked mushrooms within `radius` of p (a test aims at them).
    void targets_near(const float p[3], float radius, std::vector<float>& xyz) const;
    // The nearest cluster to a point (diagnostics, a --cam line): false if none.
    bool nearest(const float p[3], float out[3]) const;
    const Config& config() const { return cfg_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct Mushroom {
        float pos[3];
        float scale;
        float rot[4];
        uint32_t model;
        uint32_t id;
        bool picked = false;
    };
    struct Cluster {
        uint32_t id;
        float centre[3];         // the light's place: over the caps
        std::vector<Mushroom> m;
        int left = 0;
    };
    struct Model { uint32_t first, count; float height; };
    struct Vertex { float pos[3]; float nrm[3]; float emit; };

    bool load_config(const char* path, std::string& error);
    void populate(size_t cave, const GroundFn& ground);
    void build_models(std::vector<Vertex>& v, std::vector<uint32_t>& idx);

    bool ready_ = false;
    Config cfg_;
    float glow_lin_[3] = {0.0f, 0.0f, 0.0f};    // the colour, linear, luminance 1
    const worldgen::SeedWorld* world_ = nullptr;
    std::vector<char> generated_;
    std::vector<Cluster> clusters_;
    size_t n_generated_ = 0, n_mushrooms_ = 0, picked_count_ = 0, drawn_ = 0;
    bool dirty_ = true;

    std::vector<Model> models_;
    ComPtr<ID3D12Resource> vb_, ib_, inst_[kFrames];
    MushroomGpu* inst_cpu_[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
    D3D12_INDEX_BUFFER_VIEW ibv_ = {};
    ComPtr<ID3D12PipelineState> pso_;
    struct Group { uint32_t model, first, count; };
    std::vector<Group> groups_[kFrames];
};

}  // namespace engine

#endif  // ENGINE_GLOW_MUSHROOMS_H
