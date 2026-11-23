#pragma once
// ---------------------------------------------------------------------------
// Volumetric clouds (docs/adr-005-clouds-and-rain.md, phase C1;
// shaders/clouds.hlsl). Owns the noise volumes, the weather map, the
// quarter-resolution march and its history, and hands the post composite a
// resolved (in-scatter, transmittance) and cloud distance at fixed heap slots.
//
// Presentation only: driven by weather.h, never driving it.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace engine {

struct CloudSettings {
    float base_alt = 1500.0f;      // m, the cloud base
    float sigma = 0.1f;            // extinction per metre at density 1
    float base_scale = 6000.0f;    // m, the base noise volume's tile
    float detail_scale = 800.0f;   // m, the detail volume's tile
    float map_extent = 64000.0f;   // m, the weather map's half width
    float shear = 1500.0f;         // m, how far a tall cloud's top leans downwind
    float haze_km = 35.0f;         // e-folding of the fade into the horizon
    float detail_amount = 0.5f;    // how deep the detail erodes the edges
    float drift_factor = 2.5f;     // the wind aloft over the ground's
};

class Clouds {
public:
    // Heap slots: 11 SRVs, 11 UAVs, then the composite's two.
    // Then the cloud-shadow map's SRV (kShadowSrv, the composite's t14, right
    // after the resolved pair) and UAV (kShadowUav, right after the UAVs' u10).
    static const UINT kSrvBase = 83, kUavBase = 94, kOutSrv = 106, kShadowSrv = 108, kShadowUav = 105, kHeapEnd = 109;
    static const UINT kShadowSize = 512;
    static constexpr float kShadowExtent = 6000.0f;   // m, half width round the camera

    // What a frame needs from the renderer and the weather.
    struct Frame {
        float inv_view_proj[16];    // this frame, unjittered
        float prev_view_proj[16];   // last frame's, unjittered
        float cam_pos[3];
        float time;
        float key_dir[3];
        float key_lux_top[3];
        float cam_alt_km;
        float sky_mean[3];
        float ground[3];
        float storm;                // weather.h's dial, 0..1
        float wind_dir[2];
        float drift[2];             // the ground wind's drift; aloft is drift_factor x it
        bool  history_valid;
        float flash[4];             // ADR-005 C5: a lightning flash's point and intensity (cd, 0 none)
    };

    bool load(const char* archetypes_path, std::string& error);
    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, ID3D12Resource* transmittance,
              ID3D12Resource* sky_view, UINT width, UINT height, std::string& error);
    bool resize(UINT width, UINT height, std::string& error);
    // Records the frame's weather map, march and resolve. The caller has the
    // renderer's heap bound and the sky-view table readable. Leaves the
    // resolved textures readable by compute.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const Frame& f);

    bool ready() const { return ready_; }
    const CloudSettings& settings() const { return settings_; }
    // The storm's mean coverage, type and precipitation, as last recorded.
    float coverage() const { return coverage_; }
    bool shadow_ready() const { return shadow_readable_; }
    // The cloud-shadow map (R16F, kRead state between frames), for capture()'s report.
    ID3D12Resource* shadow_texture() const { return shadow_.Get(); }
    // The shadow map's centre (x, z in the cloud-base plane), half extent and
    // the base's altitude, as the composite needs them.
    void shadow_params(float out[4]) const {
        out[0] = shadow_c_[0]; out[1] = shadow_c_[1]; out[2] = kShadowExtent; out[3] = settings_.base_alt;
    }
    float cloud_type() const { return type_; }
    float precipitation() const { return precip_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool make_targets(std::string& error);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu(UINT slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu(UINT slot) const;

    CloudSettings settings_;
    bool ready_ = false, noise_done_ = false, out_readable_ = false;
    ID3D12Device* dev_ = nullptr;
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT inc_ = 0, w_ = 0, h_ = 0;
    ComPtr<ID3D12Resource> base_, detail_, weather_, raw_, raw_dist_, hist_[2], hdist_[2], out_, out_dist_, shadow_;
    float shadow_c_[2] = {0, 0};
    bool shadow_readable_ = false;
    ComPtr<ID3D12Resource> cb_[2];
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> base_pso_, detail_pso_, weather_pso_, march_pso_, temporal_pso_, shadow_pso_;
    unsigned prev_ = 0;   // which history holds last frame's
    unsigned long long frames_ = 0;
    float coverage_ = 0.0f, type_ = 0.0f, precip_ = 0.0f;
};

}  // namespace engine
