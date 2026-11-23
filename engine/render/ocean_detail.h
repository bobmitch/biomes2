#pragma once
// ---------------------------------------------------------------------------
// The sea's ripples: two FFT tiles synthesised every frame on the GPU
// (shaders/ocean_fft.hlsl) from the spectrum in ocean_spectrum.h.
//
// The 32 Gerstner trains (water_wave.h) carry the sea down to 4.1 m, and the
// geometry, the swimmer and every height the sea is asked for. Below that,
// the ripples: slopes for the shading -- the glitter, the broken reflections
// -- and the surface's Jacobian for the foam, never geometry and never
// physics, so the swimmer's sea is the same function it was.
//
// The CPU normalises the ripples each frame so that the whole sea's mean
// square slope, Gerstner trains and ripples together, is Cox and Munk's for
// the wind (od_cox_munk). Presentation only: no hash, no save.
// ---------------------------------------------------------------------------
#include "engine/render/ocean_spectrum.h"
#include "engine/render/water_wave.h"

#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace engine {

class OceanDetail {
public:
    static const UINT kUavSlot = 81, kSrvSlot = 82;   // in the renderer's heap

    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error);
    // This frame's ripples: the wind the sea is developed for, its heading,
    // and the Gerstner sea, whose own slope the ripples make up to Cox-Munk.
    void update(float time, float dt, float wind_u, const float wind_dir[2], const WwSea& sea);
    // Records the three passes. The caller has the renderer's heap bound.
    // Leaves the output readable by a pixel shader.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);

    bool ready() const { return ready_; }
    ID3D12Resource* texture() const { return out_.Get(); }
    float tile(int c) const { return od_tile[c]; }
    // The mean square slope (both components) cascade c carries, and the
    // Gerstner trains' and the target's, for the report and the glint.
    float variance(int c) const { return var_[c]; }
    float gerstner_variance() const { return gvar_; }
    float target_variance() const { return target_; }
    float choppy() const { return choppy_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    ComPtr<ID3D12Resource> a_, b_, out_, cb_[2];
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> spectrum_pso_, rows_pso_, cols_pso_;
    ID3D12DescriptorHeap* heap_ = nullptr;
    UINT inc_ = 0;
    bool out_readable_ = false;

    // The normalisation's cache: the spectrum's slope sum per cascade, at
    // the wind it was summed for.
    float sum_u_ = -1.0f, sum_dir_[2] = {0, 0};
    double sum_[OD_CASCADES] = {0, 0};

    float time_ = 0.0f, dt_ = 0.0f, amp_ = 0.0f, foam_decay_ = 0.0f;
    float wind_u_ = 0.0f, wind_dir_[2] = {1, 0};
    float var_[OD_CASCADES] = {0, 0}, gvar_ = 0.0f, target_ = 0.0f;
    float choppy_ = 1.6f;
};

}  // namespace engine
