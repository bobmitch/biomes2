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
#include "engine/render/gpu_profile.h"

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
    float wind = 1.0f;          // a multiplier on the weather's sway
    // The weather (weather.h), set by the renderer every frame: direction x,
    // z; mean speed m/s; gustiness. And the metres the air has moved.
    float wind_v[4] = {0.94f, 0.33f, 4.0f, 0.4f};
    float wind_drift[2] = {0.0f, 0.0f};
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
    // After record_prepass in the same frame, draws colour over the
    // prepass's depth (EQUAL) from the lists it culled.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const TreeFrameInputs& in,
                const TreeDrawSettings& s);
    // The depth prepass: culls, then draws the mesh levels depth only with
    // the coverage test (dither, cut, leaves), so record() shades each
    // visible tree pixel once instead of once per surviving card. Called
    // before the terrain, whose hidden pixels it also saves. A no-op when
    // the prepass is off or the overdraw counter is running.
    void record_prepass(ID3D12GraphicsCommandList* cmd, UINT frame, const TreeFrameInputs& in,
                        const TreeDrawSettings& s);
    void set_prepass(bool on) { prepass_on_ = on; }
    bool prepass() const { return prepass_on_; }
    // Cull against one cascade and draw depth only. The depth target and
    // viewport must already be bound.
    void record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, UINT cascade,
                       const TreeShadowInputs& in, const TreeDrawSettings& s);
    // One face of a local light's shadow (ADR-004 L3): culled against the
    // face, levelled like the sun's first cascade (full detail, real
    // leaves). `face` is the frame's face counter, 0 .. kMaxLightFaces - 1,
    // each with its own constants: twelve for the lights' own redraws and
    // twelve for the faces the player's body moves through.
    static const UINT kMaxLightFaces = 24;
    void record_light_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, UINT face,
                             const TreeShadowInputs& in, const TreeDrawSettings& s);

    // Per-pass GPU timing (gpu_profile.h). While it is enabled the mesh
    // levels draw as separate ExecuteIndirect runs -- every model's LOD 0,
    // then every model's LOD 1 -- so each level gets its own timestamp and
    // pipeline statistics; the impostors are one draw as always.
    void set_profiler(GpuProfiler* p) { prof_ = p; }
    // The scans' sampler for bark (s5): the renderer's, for its current
    // anisotropy (Renderer::scan_sampler). Set before recording.
    void set_scan_sampler(D3D12_GPU_DESCRIPTOR_HANDLE h) { scan_sampler_ = h; }

    // The overdraw instrument (`--gpu-profile`): while on, the CAMERA's pass
    // draws with pixel shaders that count, per pixel, every fragment the tree
    // pass rasterised and every one that survived its coverage and dither
    // tests (and of those, which were leaf cards and which impostors). Two
    // uints a pixel: invoked | survived << 16, leaf | impostor << 16. A UAV
    // write forces late depth, so "invoked" is every rasterised fragment, not
    // what early depth would have let through -- the pipeline statistics'
    // PS invocations are that. Report modes only: the clear and the readback
    // wait on the GPU.
    bool overdraw_begin(ID3D12CommandQueue* queue, UINT width, UINT height, std::string& error);
    bool overdraw_read(ID3D12CommandQueue* queue, std::vector<uint32_t>& out, std::string& error);
    void overdraw_end();

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
    // For the ray-tracing scene (ADR-004 L5): the models' CPU meshes, the
    // species' looks, and this frame's instances.
    const std::vector<TreeModel>& models() const { return models_; }
    const std::vector<TreeLook>& looks() const { return looks_; }
    const std::vector<TreeGpuInstance>& instance_list() const { return cpu_instances_; }

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
                       const void* frame_constants, bool shadow, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb,
                       const char* label);
    void upload_instances(UINT f);
    void write_cb(UINT f, UINT cb_index, const void* frame_constants);
    D3D12_GPU_VIRTUAL_ADDRESS cb_address(UINT f, UINT cb_index) const;
    void cull(ID3D12GraphicsCommandList* cmd, UINT f, D3D12_GPU_VIRTUAL_ADDRESS cb);
    void bind_draw(ID3D12GraphicsCommandList* cmd, UINT f, D3D12_GPU_VIRTUAL_ADDRESS cb,
                   D3D12_GPU_VIRTUAL_ADDRESS shadow_cb);
    void draw_meshes(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, const char* label);
    void draw_impostors(ID3D12GraphicsCommandList* cmd, ID3D12PipelineState* pso, const char* label);
    void release(ID3D12GraphicsCommandList* cmd);
    ComPtr<ID3D12PipelineState> mesh_depth_pso_, mesh_eq_pso_;
    bool prepass_on_ = true, prepassed_ = false;
    GpuProfiler* prof_ = nullptr;
    ComPtr<ID3D12PipelineState> mesh_od_pso_, imp_od_pso_;
    ComPtr<ID3D12Resource> od_buf_, od_zero_, od_rb_;
    UINT od_w_ = 0, od_h_ = 0;
    bool od_on_ = false;
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
    static constexpr UINT kScanSamplerParam = 11;   // the draw root's s5 table
    D3D12_GPU_DESCRIPTOR_HANDLE scan_sampler_ = {};
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
