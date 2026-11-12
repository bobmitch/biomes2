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
enum class Mode { Raymarch, Mesh, Overlay };

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
    UINT   readback_latency_frames = 0;
    float  height_residual_max = 0.0f;
    double height_residual_mean = 0.0;
    unsigned long long height_samples_checked = 0;
    size_t resident = 0, pending = 0, free_slots = 0;
    UINT   vertices = 0, indices = 0, overflowed = 0;
    UINT   meshed_this_frame = 0;
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
    bool create_pipeline(std::string& error);
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
    ComPtr<ID3D12RootSignature>       root_;
    ComPtr<ID3D12PipelineState>       pso_;

    ComPtr<ID3D12DescriptorHeap>   dsv_heap_;
    ComPtr<ID3D12Resource>         depth_;
    ComPtr<ID3D12RootSignature>    mesh_root_;
    ComPtr<ID3D12PipelineState>    mesh_pso_, mesh_wire_pso_;
    ComPtr<ID3D12CommandSignature> cmd_sig_;

    ChunkMesher mesher_;
    Streamer    streamer_;
    std::vector<PatchEvent> patches_added_;
    std::vector<MeshPatchEvent> mesh_patches_added_;
    std::vector<ChunkKey>   patches_removed_;
    FrameStats  stats_;
    Mode        mode_ = Mode::Mesh;

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
