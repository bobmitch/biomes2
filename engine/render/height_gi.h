#pragma once
// ---------------------------------------------------------------------------
// Heightfield GI (ADR-004 L6): the no-RT tier's diffuse GI, and the default.
//
// The world is mostly 2.5D, and the light that matters at a point -- the sky
// the terrain round it leaves, and the light that terrain bounces at it --
// comes as often from off screen as on. So: a 512 m map of 2 m texels round
// the camera, the terrain drawn into it from above (height, albedo), lit
// (sun through the heightfield and the canopy, sky, last rebuild's bounce),
// then gathered per texel along twelve azimuths into L1 spherical harmonics
// of sky visibility and bounce radiance. hfgi.hlsli reads them in
// gi_diffuse. Rebuilt when the camera has moved 8 m, the light has moved,
// or a second has passed (edits, streaming).
//
// What it cannot see: overhangs and caves (a point under the top surface is
// treated as covered), crowns as bouncers, anything past 256 m.
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace engine {

class HeightGi {
public:
    static const UINT  kSize = 256;
    static constexpr float kTexel = 2.0f;
    static constexpr float kExtent = 512.0f;
    // Heap: [48..52] the scene's SRVs (t38..t42: vis, r, g, b SH, height),
    // [53..55] the top-down albedo, depth and radiance for the update,
    // [56..61] the update's UAVs (height, radiance, the four SH).
    static const UINT  kSrvScene = 48, kSrvTop = 53, kUav = 56;

    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error);
    bool ready() const { return ready_; }

    // Whether a rebuild is due; if so, fixes this rebuild's map and returns
    // its top-down view-projection (row-vector, clip = p * M) in `vp`.
    bool begin(const float cam[3], float hours, unsigned long long frame, float vp[16]);
    // The top-down targets the renderer draws the terrain into between
    // begin() and record(): albedo (RGBA16F) and depth.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv() const;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv() const;
    ID3D12Resource* albedo() const { return albedo_.Get(); }
    ID3D12Resource* depth() const { return depth_.Get(); }
    // Lights and gathers the map. The SH and height end readable by pixel
    // and compute shaders; `shadow_cb` is the frame's shadow constants.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb);
    // Every frame: the camera's share of an open sky from the map (read back
    // two frames late; `slot` the frame in flight).
    void record_camera(ID3D12GraphicsCommandList* cmd, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb);
    float camera_sky() const { return camera_sky_; }
    // The eye's share of open sky by geometry alone (sky visibility, no
    // bounce, no light): 1 in the open, 0 sealed in. Dark vision's floor.
    float camera_open() const { return camera_open_; }
    // ShadowConstants' l_hf: the map's min corner x, z; w on. Zero until the
    // first rebuild.
    void constants(bool on, float out[4]) const;
    unsigned long long rebuilds() const { return rebuilds_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false, built_ = false;
    float origin_[2] = {}, centre_[3] = {}, y_top_ = 0.0f, y_range_ = 0.0f, hours_ = -100.0f;
    unsigned long long last_frame_ = 0, rebuilds_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_, rtv_heap_, dsv_heap_;
    ComPtr<ID3D12Resource> albedo_, depth_, height_, rad_, sh_[4], cb_[2];
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> light_pso_, gather_pso_, camera_pso_;
    ComPtr<ID3D12Resource> cam_, cam_rb_[2];
    bool cam_pending_[2] = {};
    float camera_sky_ = 1.0f;
    float camera_open_ = 1.0f;
};

}  // namespace engine
