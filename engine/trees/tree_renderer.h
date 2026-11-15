// ---------------------------------------------------------------------------
// Drawing trees: GPU culling, three levels, octahedral impostors.
//
// Per frame: the CPU hands over a flat instance list (TreeField's standing
// trees plus the felled logs and stumps). The GPU culls it against the
// frustum and the draw distance, picks a level per tree with a dithered
// cross-fade at each boundary, and draws everything in two ExecuteIndirect
// calls -- one over every (model, mesh level), one for all impostors.
//
// The impostor atlas is baked once at startup: every model from frames x
// frames directions over the upper hemisphere (hemi-octahedral layout), its
// albedo and normal premultiplied by coverage so the mip chain is a plain
// average and a distant crown keeps its density instead of thinning out.
// ---------------------------------------------------------------------------
#ifndef ENGINE_TREE_RENDERER_H
#define ENGINE_TREE_RENDERER_H

#include "engine/trees/tree_field.h"
#include "engine/trees/tree_mesh.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace engine {

using Microsoft::WRL::ComPtr;

struct TreeDrawSettings {
    float max_dist = 500.0f;    // the draw-distance slider
    float lod0_dist = 45.0f;    // full mesh inside this
    float lod1_dist = 170.0f;   // simplified mesh inside this; impostors beyond
    float wind = 1.0f;
    float debug = 0.0f;         // 1 tints by level
    // Towards the key light -- sun, or moon at night -- which an impostor's
    // shadow card faces. The renderer sets it every frame (ADR-004 L2).
    float sun_dir[3] = {0.465778f, 0.807348f, 0.362271f};
};

struct TreeFrameInputs {
    float view_proj[16];
    float cam_pos[3];
    float cam_fwd[3], cam_right[3], cam_up[3];
    float time;
    float far_plane;
    D3D12_GPU_VIRTUAL_ADDRESS shadow_cb = 0;   // the renderer's shadow constants
};

// One shadow cascade: the light's view, its culling planes, and the light's
// basis -- an impostor casts with its card facing the sun.
struct TreeShadowInputs {
    float view_proj[16];
    float planes[6][4];
    float cam_pos[3];        // the CAMERA, which picks each tree's level
    float light_right[3], light_up[3];
    float time;
    float far_plane;
};

class TreeRenderer {
public:
    // Builds the models, uploads them and bakes the impostors. Uses its own
    // command list and fence, because the mip chain is built on the CPU from
    // a readback of the bake.
    // `heap` is the renderer's shader-visible heap; the atlases' views go at
    // `slot` and `slot + 1`, and the shadow map's is at 0.
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue,
              const std::vector<TreeLook>& looks, ID3D12DescriptorHeap* heap, UINT slot,
              std::string& error);
    void shutdown();

    // The whole instance list. Copied; uploaded lazily per frame in flight.
    void set_instances(const std::vector<TreeGpuInstance>& inst);

    // Culls and draws. Render target, depth, viewport and scissor must
    // already be bound; leaves the graphics root signature changed.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const TreeFrameInputs& in,
                const TreeDrawSettings& s);
    // Cull against one cascade and draw depth only. The depth target and
    // viewport must already be bound.
    void record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, UINT cascade,
                       const TreeShadowInputs& in, const TreeDrawSettings& s);

    // What the models are, for physics and felling.
    const TreeModel& model(uint32_t species, uint32_t variant) const {
        return models_[species * kTreeVariants + (variant % kTreeVariants)];
    }
    const TreeLook& look(uint32_t species) const { return looks_[species]; }
    size_t model_count() const { return models_.size(); }
    bool ready() const { return ready_; }

    // Diagnostics, filled at init.
    size_t vertices() const { return total_vertices_; }
    size_t indices() const { return total_indices_; }
    size_t atlas_bytes() const { return atlas_bytes_; }
    double bake_ms() const { return bake_ms_; }
    size_t instances() const { return cpu_instances_.size(); }

    // Synchronous readback of last frame's per-draw counts and overflows.
    // Report modes only: it waits on the GPU.
    bool read_counts(ID3D12CommandQueue* queue, std::vector<uint32_t>& per_draw,
                     uint32_t& overflow, std::string& error);

private:
    bool create_pipelines(std::string& error);
    bool bake(ID3D12CommandQueue* queue, std::string& error);
    bool run_and_wait(ID3D12CommandQueue* queue, std::string& error);

    ID3D12Device* device_ = nullptr;
    std::vector<TreeLook>  looks_;
    std::vector<TreeModel> models_;
    size_t total_vertices_ = 0, total_indices_ = 0, atlas_bytes_ = 0;
    double bake_ms_ = 0.0;

    ComPtr<ID3D12RootSignature> draw_root_, cull_root_;
    ComPtr<ID3D12PipelineState> mesh_pso_, imp_pso_, bake_pso_;
    ComPtr<ID3D12PipelineState> mesh_shadow_pso_, imp_shadow_pso_;
    void cull_and_draw(ID3D12GraphicsCommandList* cmd, UINT frame, UINT cb_index,
                       const void* frame_constants, bool shadow, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb);
    ComPtr<ID3D12PipelineState> clear_pso_, cull_pso_, args_pso_;
    ComPtr<ID3D12CommandSignature> cmd_sig_;

    ComPtr<ID3D12Resource> vb_, ib_, model_buf_, look_buf_, slot_buf_;
    ComPtr<ID3D12Resource> visible_, counters_, args_, stats_;
    ComPtr<ID3D12Resource> inst_up_[2];
    void* inst_cpu_[2] = {};
    unsigned long long inst_version_[2] = {0, 0};
    ComPtr<ID3D12Resource> frame_cb_[2];
    void* frame_cb_cpu_[2] = {};
    ComPtr<ID3D12Resource> atlas_albedo_, atlas_normal_;
    ID3D12DescriptorHeap* srv_heap_ = nullptr;   // the renderer's
    UINT srv_slot_ = 1;

    // Init-time submission.
    ComPtr<ID3D12CommandAllocator>    init_alloc_;
    ComPtr<ID3D12GraphicsCommandList> init_cmd_;
    ComPtr<ID3D12Fence>               init_fence_;
    UINT64 init_fence_value_ = 0;
    std::vector<ComPtr<ID3D12Resource> > staging_;

    std::vector<TreeGpuInstance> cpu_instances_;
    unsigned long long cpu_version_ = 1;
    UINT quad_first_ = 0, quad_base_ = 0;
    UINT draw_count_ = 0;
    UINT visible_total_ = 0;
    bool ready_ = false;
};

}  // namespace engine

#endif  // ENGINE_TREE_RENDERER_H
