#pragma once
// ---------------------------------------------------------------------------
// The canopy map (ADR-004 L6): how much sky the crowns overhead leave, for
// the tier with no ray tracing -- and the default everywhere RT's traced
// sky is off.
//
// A 512 m square of 1 m texels round the camera, rebuilt on the GPU every
// frame from the standing trees' crown ellipsoids (crown_volume, the same
// volumes the RT scene traces): each texel holds the crowns' vertical
// optical depth tau, tau-weighted crown bottom and top (relative to the
// camera's height), and the vertical transmittance exp(-k tau). Slices 1..5
// are the same blurred a-trous style to footprints of roughly 1.6, 3, 7, 13
// and 26 m across, because the sky a point sees through a canopy spreads over
// a disc about as wide as the canopy is high above it. canopy.hlsli reads it
// in gi_sky_visibility, so it darkens the SKY term only: the ground's bounce
// and local lights are not touched.
//
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "engine/trees/tree_field.h"
#include "engine/trees/tree_mesh.h"

namespace engine {

class CanopyMap {
public:
    static const UINT  kSize = 512;          // texels a side
    static const UINT  kSlices = 6;          // raw, then five blurs
    static const UINT  kMaxTrees = 65536;
    static constexpr float kTexel = 1.0f;    // metres
    static const UINT  kSrvSlot = 38, kUavSlot = 39;   // in the renderer's heap

    // `heap` is the renderer's shader-visible CBV/SRV/UAV heap.
    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error);
    void set_models(const std::vector<TreeModel>& models, const std::vector<TreeLook>& looks);

    // Builds the map on `cmd` when it is due: the tree list changed
    // (`version`), or the camera moved kRebuildDistance from where it was
    // last built. Otherwise the last map stands, already readable. `origin`
    // receives the map's min corner (x, z) and the height its crown heights
    // are relative to -- the last BUILD's, so the readers always agree with
    // the texture.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const float cam[3],
                const std::vector<TreeGpuInstance>& trees, unsigned long long version, float origin[3]);
    static constexpr float kRebuildDistance = 16.0f;
    unsigned long long builds() const { return builds_; }

    bool ready() const { return ready_; }
    UINT trees_last_frame() const { return trees_; }
    ID3D12Resource* texture() const { return tex_.Get(); }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false, first_ = true;
    unsigned long long version_ = ~0ull, builds_ = 0;
    float centre_[3] = {}, origin_[3] = {};
    UINT trees_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> tex_, accum_, tree_up_[2], cb_[2];
    void* tree_cpu_[2] = {};
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> clear_pso_, splat_pso_, resolve_pso_, blur_pso_;
    std::vector<CrownVolume> crowns_;   // per model
};

}  // namespace engine
