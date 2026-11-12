// ---------------------------------------------------------------------------
// B1: device, swapchain, and one fullscreen pass.
//
// Deliberately not a "graphics abstraction". Everything here is the minimum
// D3D12 that B2 will build on -- descriptor heaps, per-frame command
// allocators and fence-value synchronisation -- and nothing is wrapped until
// there are two callers to justify the wrapper. See ADR-003 for why D3D12.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RENDERER_H
#define ENGINE_RENDERER_H

#include "engine/meshing/chunk_mesher.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <string>

namespace engine {

using Microsoft::WRL::ComPtr;

// Two frames in flight. The design's readback path wants 2-3 frames of
// latency, so this is the number that will matter later; it is here now
// because getting allocator recycling wrong is invisible until it is not.
static const UINT kFrameCount = 2;

struct ViewConstants {
    float cam_pos[3];    float time;
    float cam_fwd[3];    float aspect;
    float cam_right[3];  float tan_half_fov;
    float cam_up[3];     float pad0;
};

// What the frame draws. Both render the same field from the same camera; the
// only intended difference is that one marches it and the other draws the
// triangles the mesher built from it. Any other difference is a meshing bug,
// which is the entire reason the raymarch survives into B2 instead of being
// deleted once there is a mesh.
enum class Mode { Raymarch, Mesh, Overlay };

class Renderer {
public:
    // Returns false and fills `error` rather than throwing: the caller puts it
    // in a message box, which is the only diagnostic a windowed app has before
    // there is anything on screen.
    bool init(HWND hwnd, std::string& error);
    void render(const ViewConstants& view);

    void set_mode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    // Meshes the one chunk B2 is about and reports what it cost. Runs once:
    // the trivial field does not change, and remeshing a static chunk every
    // frame would measure the wrong thing.
    bool mesh_chunk(std::string& error);
    // Max and mean |density| at the meshed vertices, in metres. A vertex that
    // sits on the isosurface reads zero; surface nets averages edge crossings
    // rather than solving for the surface, so a small residual is expected and
    // a large one means the mesh is not the field's surface.
    bool check_against_field(float& max_err, float& mean_err, UINT& checked);

    UINT   mesh_vertices() const { return mesh_vertices_; }
    UINT   mesh_indices()  const { return mesh_indices_; }
    double mesh_ms()       const { return mesh_ms_; }

    // Render one frame and write the back buffer to a 24-bit BMP, without
    // presenting or needing the window to be visible. Reads the render target
    // itself rather than the desktop, so it captures what this program drew
    // and nothing else.
    bool capture(const ViewConstants& view, const char* path, std::string& error);
    void resize(UINT width, UINT height);
    void shutdown();

    // Recompiles the shader from disk. Keeping this reachable from B1 means
    // the shader edit loop never has to be rebuilt later, and B2's kernel work
    // is mostly shader editing.
    bool reload_shaders(std::string& error);

private:
    bool create_device(std::string& error);
    bool create_swapchain(HWND hwnd, std::string& error);
    bool create_rtvs(std::string& error);
    bool create_pipeline(std::string& error);
    bool create_mesh_pipeline(std::string& error);
    bool create_depth(std::string& error);
    void record_scene(const ViewConstants& view, D3D12_CPU_DESCRIPTOR_HANDLE rtv);
    void wait_for_gpu();
    void next_frame();

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

    // B2
    ComPtr<ID3D12DescriptorHeap>  dsv_heap_;
    ComPtr<ID3D12Resource>        depth_;
    ComPtr<ID3D12RootSignature>   mesh_root_;
    ComPtr<ID3D12PipelineState>   mesh_pso_, mesh_wire_pso_;
    ComPtr<ID3D12CommandSignature> cmd_sig_;
    ChunkMesher mesher_;
    Mode   mode_ = Mode::Raymarch;
    UINT   mesh_vertices_ = 0, mesh_indices_ = 0;
    double mesh_ms_ = 0.0;
    bool   meshed_ = false;

    ComPtr<ID3D12Fence> fence_;
    HANDLE              fence_event_ = nullptr;
    UINT64              fence_values_[kFrameCount] = {0, 0};
    UINT                frame_ = 0;

    UINT rtv_stride_ = 0;
    UINT width_ = 0, height_ = 0;
    bool ready_ = false;
};

}  // namespace engine

#endif  // ENGINE_RENDERER_H
