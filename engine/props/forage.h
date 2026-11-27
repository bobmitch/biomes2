// ---------------------------------------------------------------------------
// Things to forage: layer 10's plants (worldgen/world.h, forage_at_cell)
// near the player -- streamed, drawn, picked, and grown back.
//
// ZONES, NOT A WORLD LIST. Valheim keeps its world in 64 m zones: a zone's
// contents are generated when a player first comes near, and what changed
// is kept as per-object records (its ZDOs). Here the generation half is
// free -- a plant is a pure function of its cell, so a zone can be rebuilt
// from the seed whenever it is needed and thrown away when it is not -- and
// the record half is only what the player CHANGED: picked plants, by id,
// with the game time they were picked (`picked_`). So: 64 m tiles round the
// player, generated on a worker as they come into reach (RockField's shape)
// and dropped when they leave it, over a sparse map of picks that is the
// only thing saved. A BVH would buy nothing: every query is "near the
// player", which the tile grid answers by construction, and the set changes
// as the player walks, which a BVH would have to rebuild.
//
// REGROWTH is by the game clock: a plant bears again `regrow_days` game days
// after it was picked (archetypes.json forage species; strawberries 3). No
// timer runs anywhere: ripe(id) compares the clock with the pick's time.
//
// DRAWING: procedural meshes made here (a rosette of trifoliate leaves on
// stalks, berries at its rim, flowers while it grows back), instanced on the
// terrain's root signature after the rocks, as the mushrooms are; lit by
// the sun with its shadow, the sky, the bounce and the local lights. They
// cast no shadow. The grass is cleared from under each plant (the foliage
// bake's bare mask, `clear_tops`), or 50 cm of meadow hides them.
// ---------------------------------------------------------------------------
#ifndef ENGINE_FORAGE_H
#define ENGINE_FORAGE_H

#include "worldgen/world.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

struct ForageGpu {
    float pos[3];
    float scale;
    float rot[4];      // quaternion
    float tint[3];     // the leaves' colour, linear
    float fade;        // draw distance, flat metres (the dithered fade)
};
static_assert(sizeof(ForageGpu) == 48, "ForageInst in forage.hlsl");

class Forage {
public:
    static const UINT kFrames = 2;
    static const UINT kMaxDrawn = 8192;      // instances a frame, all parts
    static constexpr float kDrawDist = 90.0f;     // metres, flat
    static constexpr float kTileM = 64.0f;
    static constexpr float kStreamRadius = 150.0f;
    static constexpr float kClearRadius = 0.45f;   // grass kept off round a plant's foot, metres

    // What a species gives and how it grows back (the engine's half of its
    // archetypes.json entry), and how it looks.
    struct Species {
        std::string name;
        std::string item;              // an id in game/data/items.json
        int   yield_min = 1, yield_max = 1;
        float regrow_days = 3.0f;
        float size = 1.0f;             // metres across, roughly
        float leaf[3] = {0.20f, 0.38f, 0.12f};    // sRGB as typed
        float berry[3] = {0.80f, 0.08f, 0.10f};
        float flower[3] = {0.96f, 0.95f, 0.90f};
    };

    struct Hit {
        uint32_t id = 0;
        int species = -1;
        float pos[3] = {0, 0, 0};
        float dist = 0.0f;             // along the ray
    };

    Forage();
    ~Forage();

    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
              const char* archetypes_path, std::string& error);
    void shutdown();
    bool ready() const { return ready_; }
    // The layer's world. Starts the worker; drops tiles and picks.
    void set_world(const worldgen::SeedWorld* w, const worldgen::WorldParams* wp,
                   const worldgen::ArchetypeTable* table);

    // Tiles out to kStreamRadius round (x, z); `settle` waits for them (a
    // still, a test). True when the resident set changed. The boxes of new
    // plants are handed to `on_new` (the foliage re-bakes under them).
    using BoxFn = std::function<void(const float lo[3], const float hi[3])>;
    bool update(float x, float z, bool settle, const BoxFn& on_new = BoxFn());

    // This frame's instances (frustum, draw distance), ripe or not by `now`
    // (game seconds).
    void prepare(UINT frame, const float cam[3], const float view_proj[16], double now);
    // The terrain's root signature, constants, shadow CBV and table bound.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);

    // The ripe plant the ray from `eye` along `dir` is on, within `reach` of
    // `hand` and in front of `occluder` metres of ray.
    bool aim(const float eye[3], const float dir[3], const float hand[3], float reach, float occluder,
             double now, Hit& out) const;
    // Picks a ripe plant: how many of its species' item it gives (0: not
    // there, or not ripe).
    int pick(uint32_t id, double now);
    bool ripe(uint32_t id, double now) const;
    // When a picked plant bears again (game seconds); 0 if it is ripe.
    double ripe_at(uint32_t id) const;

    // For the foliage bake: tops[hz * 32 + hx] over the 0.5 m columns from
    // (sx0, sz0) (half-metre units) set past any ground where a plant stands.
    void clear_tops(int sx0, int sz0, float* tops) const;

    const Species& species(int s) const { return species_[size_t(s)]; }
    int species_count() const { return int(species_.size()); }
    const std::vector<worldgen::ForageInstance>& plants() const { return plants_; }
    bool nearest(const float p[3], bool ripe_only, double now, worldgen::ForageInstance& out) const;

    // The picks: ids and game times, binary, under the world's field id.
    bool save(const char* path, uint64_t field_id, std::string& error) const;
    bool load(const char* path, uint64_t field_id, std::string& error);
    void forget_picks() { picked_.clear(); }

    size_t tiles() const { return tiles_.size(); }
    size_t drawn() const { return drawn_; }
    size_t picked_count() const { return picked_.size(); }
    bool settled() const { return settled_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    struct TileKey {
        int x, z;
        bool operator==(const TileKey& o) const { return x == o.x && z == o.z; }
    };
    struct TileKeyHash {
        size_t operator()(const TileKey& k) const {
            return std::hash<long long>()((static_cast<long long>(k.x) << 32) ^ (k.z & 0xFFFFFFFFll));
        }
    };
    // kind: 0 leaf, 1 stalk, 2 berry, 3 calyx, 4 petal, 5 a flower's middle.
    struct Vertex { float pos[3]; float nrm[3]; float uv[2]; float kind; float col[3]; };
    struct Model { uint32_t first, count; };
    enum Part { kLeaves = 0, kBerries = 1, kFlowers = 2, kParts = 3 };
    static const int kVariants = 3;

    bool load_config(const char* path, std::string& error);
    void build_models(const Species& sp, std::vector<Vertex>& v, std::vector<uint32_t>& idx);
    void worker();
    void rebuild();
    float pick_height(const worldgen::ForageInstance& f) const;
    float scale_of(const worldgen::ForageInstance& f) const;

    bool ready_ = false;
    std::vector<Species> species_;
    const worldgen::SeedWorld* world_ = nullptr;
    const worldgen::WorldParams* wp_ = nullptr;
    const worldgen::ArchetypeTable* table_ = nullptr;

    std::unordered_map<TileKey, std::vector<worldgen::ForageInstance>, TileKeyHash> tiles_;
    std::vector<worldgen::ForageInstance> plants_;
    std::unordered_map<uint32_t, double> picked_;   // id -> game seconds picked
    int last_cx_ = 1 << 30, last_cz_ = 1 << 30;
    bool settled_ = false;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<TileKey> queue_;
    std::unordered_set<TileKey, TileKeyHash> in_flight_;
    std::vector<std::pair<TileKey, std::vector<worldgen::ForageInstance> > > done_;
    std::vector<std::thread> threads_;
    bool stop_ = false;

    // Per species, kVariants x kParts models.
    std::vector<Model> models_;
    ComPtr<ID3D12Resource> vb_, ib_, inst_[kFrames];
    ForageGpu* inst_cpu_[kFrames] = {};
    D3D12_VERTEX_BUFFER_VIEW vbv_ = {};
    D3D12_INDEX_BUFFER_VIEW ibv_ = {};
    ComPtr<ID3D12PipelineState> pso_;
    struct Group { uint32_t model, first, count; };
    std::vector<Group> groups_[kFrames];
    size_t drawn_ = 0;
};

}  // namespace engine

#endif
