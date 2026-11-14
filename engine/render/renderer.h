// ---------------------------------------------------------------------------
// Device, swapchain, and the frame.
//
// Deliberately not a "graphics abstraction". Everything here is the minimum
// D3D12 the rest of Track B builds on -- descriptor heaps, per-frame command
// allocators, fence-value synchronisation -- and nothing is wrapped until
// there are two callers to justify the wrapper. See ADR-003 for why D3D12.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDERER_H
#define ENGINE_RENDERER_H

#include "engine/meshing/chunk_mesher.h"
#include "engine/streaming/streamer.h"
#include "engine/svdag/brick_store.h"
#include "engine/svdag/brick_pager.h"
#include "engine/field/world_gpu.h"

#include <vector>

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Two frames in flight. The design's readback path wants 2-3 frames of
// latency, so this is the number that will matter later; it is here now
// because getting allocator recycling wrong is invisible until it is not.
static const UINT kFrameCount = 2;

// The design's cap on remeshing per frame. Without it, flying into an unloaded
// region meshes everything at once and the frame that does it is the one the
// player notices.
static const UINT kMeshBudgetPerFrame = 8;
// The cap is now a FLOOR: the budget grows while the GPU's meshing time stays
// under this, up to kMeshBudgetMax, and shrinks back when it does not. A fixed
// 8 used 0.5 ms of a 16.7 ms frame at 15 m/s and still made distant detail
// wait 30 frames. 4 ms is what leaves the 60 Hz frame room for the draw.
static const double kMeshTargetMs = 4.0;   // only with --adaptive-budget

struct ViewConstants {
    float cam_pos[3];    float time;
    float cam_fwd[3];    float aspect;
    float cam_right[3];  float tan_half_fov;
    float cam_up[3];     float pad0;
    // The character capsule, drawn by the raymarch. cap_r <= 0 means none.
    float cap_pos[3];    float cap_r;
    float cap_half;      float pad1[3];
};

// What the frame draws. Raymarch and Mesh render the same field from the same
// camera; the only intended difference is that one marches it and the other
// draws triangles built from it. Any other difference is a meshing bug, which
// is why the raymarch survives rather than being deleted once there is a mesh.
// The raymarch is not streamed -- it is the whole field, always.
// B1 had a third mode that raymarched the field directly. M1 retired it
// with the trivial field it was the only remaining caller of.
enum class Mode { Mesh, Wireframe };

struct FrameStats {
    // Readback: how many height patches came back, how stale they were, and
    // how far the surface they describe is from the field itself.
    unsigned long long patches_total = 0;
    UINT   patches_this_frame = 0;
    size_t collision_patches = 0;
    unsigned long long chunks_classified = 0;
    unsigned long long chunks_needing_mesh = 0;
    unsigned long long mesh_shapes_delivered = 0;
    size_t mesh_backlog = 0;
    size_t edit_bricks = 0;
    size_t edit_bytes = 0;
    UINT   bricks_uploaded = 0;
    size_t chunks_invalidated = 0;
    size_t edit_voxels = 0;
    size_t bricks_resident = 0;
    unsigned long long brick_evictions = 0;
    unsigned long long brick_misses = 0;
    unsigned long long brick_upload_deferrals = 0;
    UINT   readback_latency_frames = 0;
    size_t empty_resident = 0;
    float  height_residual_voxel = 0.0f;
    UINT   peak_slot_vertices = 0;
    UINT   peak_slot_indices = 0;
    float  height_residual_max = 0.0f;
    double height_residual_mean = 0.0;
    unsigned long long height_samples_checked = 0;
    size_t resident = 0, pending = 0, free_slots = 0;
    UINT   vertices = 0, indices = 0, overflowed = 0;
    UINT   meshed_this_frame = 0;
    UINT   mesh_budget = 0;        // what this frame was allowed
    double mesh_ms = 0.0;
};

class Renderer {
public:
    // Returns false and fills `error` rather than throwing: the caller puts it
    // in a message box, which is the only diagnostic a windowed app has before
    // there is anything on screen.
    // debug_layer turns on D3D12 validation regardless of build config and
    // makes drain_debug_messages useful. Validation is the only way to see a
    // state or binding mistake: the symptom otherwise is an empty screen.
    bool init(HWND hwnd, std::string& error, bool debug_layer = false);

    // Writes every validation message produced so far to a file.
    void drain_debug_messages(const char* path);
    void dump_draw_args(const char* path);
    void render(const ViewConstants& view);
    bool capture(const ViewConstants& view, const char* path, std::string& error);
    void resize(UINT width, UINT height);
    void shutdown();

    // Recompiles every shader from disk. The whole of Track B's remaining work
    // is shader editing, so this stays reachable.
    bool reload_shaders(std::string& error);

    // Off for benchmarking: with vsync on, every measurement reads 60 and
    // says nothing about how much headroom there is.
    void set_vsync(bool on) { vsync_ = on; }
    bool tearing_available() const { return tearing_ok_; }
    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    const FrameStats& stats() const { return stats_; }

    const WorldGpu& world() const { return world_; }

    // Sea state: 0 calm, 1 storm. Storms are a planned mechanic, so this is a
    // runtime value and nothing about the surface shape is baked.
    void  set_sea_state(float s) { sea_state_ = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s); }
    float sea_state() const { return sea_state_; }
    void  set_water(bool on) { water_ = on; }
    // LOD transitions: hold the outgoing chunk until its replacements are
    // meshed, then dither-cross-fade over `frames`. See Streamer.
    void  set_lod_hold(bool on) { streamer_.set_lod_hold(on); }
    void  set_lod_fade_frames(int n) { streamer_.set_fade_frames(n); }
    void  set_measure_holes(bool on) { streamer_.set_measure_holes(on); }
    // Surface-first streaming (default on) and the GPU-time budget (default
    // off -- see adaptive_budget_). Both switchable for A/B.
    void  set_prioritise(bool on) { streamer_.set_prioritise(on); }
    void  set_adaptive_budget(bool on) {
        adaptive_budget_ = on;
        if (!on) mesh_budget_ = kMeshBudgetPerFrame;
    }
    const Streamer& streamer() const { return streamer_; }
    // 0 real shading, 1 depth, 2 foam, 3 normal, 4 displacement.
    void  set_water_debug(float m) { water_debug_ = m; }
    float water_debug() const { return water_debug_; }
    bool  water() const { return water_; }

    // Mesh one chunk and compare its density grid against the CPU generator.
    // Returns false only if the GPU work failed; disagreement is reported in
    // the numbers, not as an error.
    bool probe_field(const float origin[3], float voxel, const char* path,
                     std::string& error);

    // What residency costs: how tall the band is over each column, how much
    // of that is terrain and how much is margin, and how many chunks that
    // works out to. Reading only; it changes nothing.
    void band_report(const char* path) const;

    // Is the shading normal the surface's normal? field_normal takes a
    // central difference with a half-voxel arm, which is 0.25 m at LOD 0 and
    // 32 m at LOD 7. This meshes one chunk at each voxel size and compares
    // every stored normal against the CPU gradient taken at a SMALL arm.
    void normal_check(const char* path);

    // The edit overlay. Digging goes through here, and the renderer takes care
    // of uploading what changed and re-meshing what it touched.
    BrickStore& edits() { return edits_; }
    bool dig(float x, float y, float z, float radius, float strength,
             unsigned char material);


    // Persistence. Saving writes only the voxels the player actually edited;
    // loading throws away every resident chunk, because the terrain they were
    // meshed from has just changed underneath them.
    bool save_edits(const char* path, std::string& error) const;
    bool load_edits(const char* path, std::string& error, bool& field_mismatch);

    // Height patches that arrived through the readback ring this frame, and
    // the chunks whose patches are no longer valid. Drained by the caller and
    // handed to physics; the renderer keeps no collision state of its own.
    struct PatchEvent {
        ChunkKey key;
        float    origin[3];
        float    voxel;
        // Columns in this chunk holding more than one surface. Non-zero means
        // a heightfield cannot represent it and it needs a mesh shape.
        UINT     multi_surface_columns;
        std::vector<float> h;
    };
    // A chunk the cheap shape cannot represent, with its actual triangles.
    // Arrives four frames after the chunk was meshed: two for the
    // classification to come back, two more for the geometry.
    struct MeshPatchEvent {
        ChunkKey key;
        std::vector<float>    verts;   // xyz triples, world space
        std::vector<unsigned> idx;     // into verts
    };

    const std::vector<PatchEvent>& patches_added() const { return patches_added_; }
    const std::vector<MeshPatchEvent>& mesh_patches_added() const {
        return mesh_patches_added_;
    }
    const std::vector<ChunkKey>&   patches_removed() const { return patches_removed_; }

    // Max and mean |density| at the vertices of slot 0, in metres. A vertex on
    // the isosurface reads zero; surface nets averages edge crossings rather
    // than solving for the surface, so a small residual is expected and a
    // large one means the mesh is not the field's surface.
    bool check_against_field(float& max_err, float& mean_err, UINT& checked);

private:
    bool create_device(std::string& error, bool debug_layer);
    bool create_swapchain(HWND hwnd, std::string& error);
    bool create_rtvs(std::string& error);
    bool create_depth(std::string& error);
    bool create_mesh_pipeline(std::string& error);
    void record_frame(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void record_scene(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void wait_for_gpu();
    void next_frame();

    ComPtr<ID3D12InfoQueue>           info_queue_;
    ComPtr<IDXGIFactory4>             factory_;
    ComPtr<ID3D12Device>              device_;
    ComPtr<ID3D12CommandQueue>        queue_;
    ComPtr<IDXGISwapChain3>           swapchain_;
    ComPtr<ID3D12DescriptorHeap>      rtv_heap_;
    ComPtr<ID3D12Resource>            targets_[kFrameCount];
    ComPtr<ID3D12CommandAllocator>    allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> cmd_;

    ComPtr<ID3D12DescriptorHeap>   dsv_heap_;
    ComPtr<ID3D12Resource>         depth_;
    ComPtr<ID3D12RootSignature>    mesh_root_;
    ComPtr<ID3D12PipelineState>    mesh_pso_, mesh_wire_pso_;
    ComPtr<ID3D12RootSignature> water_root_;
    ComPtr<ID3D12PipelineState> water_pso_;
    ComPtr<ID3D12Resource>      water_indices_;
    ComPtr<ID3D12Resource>      water_cb_[kFrameCount];
    ComPtr<ID3D12Resource>      fade_buf_[kFrameCount];
    void*                       fade_cpu_[kFrameCount] = {};
    void*                       water_cb_cpu_[kFrameCount] = {};
    D3D12_INDEX_BUFFER_VIEW     water_ibv_ = {};
    UINT                        water_index_count_ = 0;
    bool create_water_pipeline(std::string& error);
    ComPtr<ID3D12CommandSignature> cmd_sig_;

    WorldGpu    world_;
    ChunkMesher mesher_;
    Streamer    streamer_;
    UINT        mesh_budget_ = kMeshBudgetPerFrame;
    // OFF by default, measured: it took 60 m/s detail latency from 3 to 2
    // frames at the 90th percentile and the 99th-percentile frame from 21 to
    // 27 ms, with the GPU's meshing time unchanged -- a CPU cost that grows
    // with chunks per frame, not yet found. --adaptive-budget turns it on.
    bool        adaptive_budget_ = false;
    BrickStore  edits_;
    BrickPager  pager_;
    std::vector<BrickKey> dirty_bricks_;
    std::vector<BrickKey> brick_uploads_;
    std::vector<PatchEvent> patches_added_;
    std::vector<MeshPatchEvent> mesh_patches_added_;
    std::vector<ChunkKey>   patches_removed_;
    FrameStats  stats_;
    Mode        mode_ = Mode::Mesh;
    float       sea_state_ = 0.25f;
    bool        water_ = true;
    float       water_debug_ = 0.0f;

    ComPtr<ID3D12Fence> fence_;
    HANDLE              fence_event_ = nullptr;
    UINT64              fence_values_[kFrameCount] = {0, 0};
    UINT                frame_ = 0;

    UINT rtv_stride_ = 0;
    UINT width_ = 0, height_ = 0;
    // One record per staging buffer: what was copied into it and when, so the
    // consumer knows which chunk each patch belongs to.
    struct RingRecord {
        UINT     count = 0;
        unsigned long long frame = 0;
        ChunkKey keys[kReadbackChunksPerFrame];
        UINT     slot[kReadbackChunksPerFrame];
        float    origin[kReadbackChunksPerFrame][3];
        float    voxel[kReadbackChunksPerFrame];
    };
    RingRecord ring_[kReadbackRing];

    // The same shape again for the mesh path, which is staged a frame later
    // than the heights because it cannot start until the classification has
    // come back.
    struct MeshRingRecord {
        UINT     count = 0;
        unsigned long long frame = 0;
        ChunkKey keys[kMeshChunksPerFrame];
        UINT     slots[kMeshChunksPerFrame];
    };
    MeshRingRecord mesh_ring_[kReadbackRing];
    std::vector<ChunkKey> mesh_wanted_;

    unsigned long long frame_index_ = 0;
    void stage_heights();
    void consume_heights();
    void stage_meshes();
    void consume_meshes();

    bool ready_ = false;
    bool vsync_ = true;
    bool tearing_ok_ = false;
};

}  // namespace engine

#endif  // ENGINE_RENDERER_H
