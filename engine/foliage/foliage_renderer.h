// ---------------------------------------------------------------------------
// Ground cover: grass, flowers and bushes, generated on the GPU (foliage.hlsl).
//
// The CPU only decides which 16 m tiles of 1 m cells are in range, keeps a
// pool of them, and asks the GPU to bake the ones that arrived -- each
// cell's surface height, slope and blended per-biome ground cover, from the
// same field function the terrain is meshed from. Every frame the GPU turns
// cached cells near the camera into blades, clump cards and bushes and draws
// them through four indirect draws. Nothing is stored, nothing collides.
// ---------------------------------------------------------------------------
#ifndef ENGINE_FOLIAGE_RENDERER_H
#define ENGINE_FOLIAGE_RENDERER_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

using Microsoft::WRL::ComPtr;
class WorldGpu;
class BrickStore;

struct FoliageSettings {
    float blade_range = 60.0f;   // blades out to here
    float card_near = 48.0f;     // cards from here; blades and cards cross-fade between
    float card_far = 150.0f;     // cards out to here
    float max_blades = 96.0f;    // blades per m^2 at grass 1, inside a patch
    float near_radius = 10.0f;   // full density inside this
    float wind = 1.0f;           // a multiplier on the weather's push
    // The weather (weather.h), set by the renderer every frame: direction x,
    // z; mean speed m/s; gustiness. And the metres the air has moved.
    float wind_v[4] = {0.94f, 0.33f, 4.0f, 0.4f};
    float wind_drift[2] = {0.0f, 0.0f};
    float debug = 0.0f;          // 1 tints by kind
    float blade_width = 1.4f;    // a multiplier on every blade's width (--grass-width)
    float tuft = 3.0f;           // blades per placed blade, fanned from one root, 1-3 (--grass-tuft)
};

struct FoliageFrameInputs {
    float view_proj[16];
    float cam_pos[3], cam_right[3], cam_up[3];
    float time;
    float far_plane;
    D3D12_GPU_VIRTUAL_ADDRESS shadow_cb = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE shadow_table = {};
};

class FoliageRenderer {
public:

    bool init(ID3D12Device* device, const WorldGpu* world, const char* archetypes_path,
              std::string& error);
    // Tiles to keep round (x, z). `settle` bakes every missing tile this
    // frame instead of the per-frame budget: for a still.
    void update(float x, float z, const FoliageSettings& s, bool settle);
    // Bake, emit and draw. Render target, depth, viewport and the shared
    // descriptor heap must already be bound.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const FoliageFrameInputs& in,
                const FoliageSettings& s);
    bool ready() const { return ready_; }

    // The player's edits: no ground cover grows where they reach the
    // surface. `invalidate_box` re-bakes the resident tiles under a world
    // box (a dig), next update.
    void set_edits(const BrickStore* e) { edits_ = e; }
    // Columns kept bare by things standing on the ground: called for each
    // tile baked with its 32 x 32 tops (half-metre columns from sx0, sz0),
    // after the edits; a top set past the ground bares it.
    void set_bare_fn(std::function<void(int sx0, int sz0, float* tops)> fn) { bare_fn_ = fn; }
    void invalidate_box(const float lo[3], const float hi[3]);
    void invalidate_all();

    // Diagnostics.
    size_t tiles_resident() const { return tiles_.size(); }
    size_t tiles_queued() const { return queue_.size(); }
    bool   settled() const { return queue_.empty(); }
    // Last frame's instance counts (blades near, far, cards, bushes, overflow);
    // a blocking readback for reports.
    bool read_counts(ID3D12CommandQueue* queue, uint32_t out[5], std::string& error);

private:
    struct TileKey {
        int x, z;
        bool operator==(const TileKey& o) const { return x == o.x && z == o.z; }
    };
    struct TileKeyHash {
        size_t operator()(const TileKey& k) const {
            return std::hash<long long>()((static_cast<long long>(k.x) << 32) ^ (k.z & 0xFFFFFFFFll));
        }
    };

    ID3D12Device*   device_ = nullptr;
    const WorldGpu* world_ = nullptr;
    bool ready_ = false;

    ComPtr<ID3D12RootSignature> compute_root_, draw_root_;
    ComPtr<ID3D12PipelineState> bake_pso_, clear_pso_, emit_pso_, args_pso_;
    ComPtr<ID3D12PipelineState> blade_pso_, card_pso_, bush_pso_;
    ComPtr<ID3D12CommandSignature> cmd_sig_;

    ComPtr<ID3D12Resource> pool_, lists_[4], counters_, args_, ib_, ground_;
    ComPtr<ID3D12Resource> slots_up_[2], bake_up_[2], cb_[2], edit_up_[2];
    void* edit_cpu_[2] = {};
    std::function<void(int, int, float*)> bare_fn_;
    const BrickStore* edits_ = nullptr;
    std::vector<TileKey> rebake_;
    void* slots_cpu_[2] = {};
    void* bake_cpu_[2] = {};
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12Resource> readback_;

    std::unordered_map<TileKey, uint32_t, TileKeyHash> tiles_;   // resident -> slot
    std::vector<uint32_t> free_;
    std::vector<TileKey>  slot_tile_;
    std::vector<uint8_t>  slot_valid_;
    std::deque<TileKey>   queue_;
    std::vector<uint32_t> bake_now_;
    int last_cx_ = 1 << 30, last_cz_ = 1 << 30;
    float last_radius_ = -1.0f;
    uint32_t s_grass_ = 0, s_tree_patch_ = 0;
    float tree_patch_freq_ = 0.0f, tree_patch_edge_ = 0.0f, sea_level_ = 0.0f;
};

}  // namespace engine

#endif  // ENGINE_FOLIAGE_RENDERER_H
