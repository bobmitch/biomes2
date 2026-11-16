#pragma once
// ---------------------------------------------------------------------------
// DDGI probes (ADR-004 L6): the GI backbone of the RT tier. Three camera-
// centred cascades of irradiance probes, 2, 8 and 32 m apart, each tracing
// 128 rays (a quarter of them a frame) through gi_trace_probe and keeping
// octahedral irradiance and depth moments; relocated out of rock and
// switched off where they cannot get out. ddgi.hlsli reads them in
// gi_diffuse for every lit surface; ddgi_update.hlsl is the update;
// ddgi_common.hlsli the layout both share -- keep the constants below equal
// to it.
//
// Needs the RT scene (rt_scene.h): it traces the same TLAS, crowns included.
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace engine {

class Ddgi {
public:
    static const int  kCascades = 3;
    static const int  kDimX = 24, kDimY = 12, kDimZ = 24;
    static const int  kProbes = kDimX * kDimY * kDimZ;   // per cascade
    static const int  kIrr = 8, kVis = 16;
    static const UINT kRays = 128, kPhases = 4;
    static const UINT kSrvIrr = 40, kSrvVis = 41, kSrvProbes = 42, kUavIrr = 43, kUavVis = 44;
    static constexpr float kSpacing[kCascades] = {2.0f, 8.0f, 32.0f};

    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error);
    bool ready() const { return ready_; }

    // Where the cascades sit this frame: whole probes, centred on the camera.
    void begin_frame(const float cam[3]);
    // ShadowConstants' l_ddgi0..3: per cascade (base x, y, z, spacing), then
    // (on, 0, 0, 0). All zero when off, which ddgi.hlsli reads as "no GI".
    void constants(bool on, float out[4][4]) const;

    // What the trace binds from elsewhere: the frame's shadow constants (the
    // light, the sky, the lights' clusters) and the RT scene's buffers.
    struct Inputs {
        D3D12_GPU_VIRTUAL_ADDRESS shadow_cb = 0, tlas = 0;
        D3D12_GPU_VIRTUAL_ADDRESS terrain_vertices = 0, terrain_indices = 0, terrain_materials = 0;
        D3D12_GPU_VIRTUAL_ADDRESS tree_positions = 0, tree_indices = 0, tree_info = 0, tree_attributes = 0;
        float leaf_radiance[3] = {0.0f, 0.0f, 0.0f};   // what a crown's leaves send along a probe ray, nits
        UINT  max_lights = 8;
    };
    // One frame's update: a quarter of each cascade's probes. The textures
    // and the probe buffer end readable by pixel and compute shaders.
    // `slot` is the renderer's frame-in-flight index, for the camera's
    // readback, which is two frames old when it is read.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, UINT slot, const Inputs& in);
    // How much of an open sky's light reaches the camera (cs_ddgi_camera):
    // 1 in the open, ~0 in a sealed cave. Two frames late.
    float camera_sky() const { return camera_sky_; }
    // The irradiance (lux, RGB) the probes give at the camera facing +x, -x,
    // +y, -y, +z, -z, the share of the probes in it, and the fallback's.
    struct CameraIrradiance { float E[6][4]; float open[6][4]; };
    const CameraIrradiance& camera_irradiance() const { return cam_irr_; }

    // Per cascade: probes active, switched off in rock, and not yet traced
    // where they stand now. Reads the probe buffer back; waits on `queue`.
    struct Counts { int active[kCascades] = {}, inactive[kCascades] = {}, idle[kCascades] = {}, pending[kCascades] = {}; };
    // With `radius` > 0, only probes whose grid point lies inside that sphere.
    bool count(ID3D12CommandQueue* queue, Counts& out, std::string& error,
               const float centre[3] = nullptr, float radius = 0.0f);

    UINT64 bytes() const { return bytes_; }
    unsigned long long updates() const { return updates_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    int base_[kCascades][3] = {};
    UINT64 bytes_ = 0;
    unsigned long long updates_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> irr_, vis_, probes_, rays_, cb_[2], cam_, cam_rb_[2];
    bool cam_pending_[2] = {};
    float camera_sky_ = 1.0f;
    CameraIrradiance cam_irr_ = {};
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> trace_pso_, irr_pso_, vis_pso_, relocate_pso_, camera_pso_;
};

}  // namespace engine
