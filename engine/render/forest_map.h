#pragma once
// ---------------------------------------------------------------------------
// Where the forests are, as a map over the whole disc: the far forest's
// source (the canopy shell, mesh.hlsl). Past the trees' draw distance the
// terrain is lifted and shaded as canopy wherever this says trees stand.
//
// Baked once per seed on the CPU from the SAME terms layer 8 plants a tree
// with, short of the per-cell rolls: the biome blend sharpened as the trees
// sharpen it, each biome's density x its patch noise, the species' altitude
// fit (so the treeline is the trees' treeline), the shore and the slope
// limit. Its expectation, not its draw -- so clearings, forest edges and
// biome borders sit where the real trees will be when the camera arrives.
// Rivers are left out: a channel is narrower than a texel.
//
// Per texel (1024^2 over 20 km, ~20 m):
//   A  cover (expected canopy fraction), canopy height in metres, conifer
//      share, 0
//   B  leaf albedo, linear rgb, 0
// Heap [70] A, [71] B (t47, t48 in the terrain's table).
//
// Presentation only: nothing here reaches the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace engine {

class WorldGpu;
struct TreeLook;

class ForestMap {
public:
    static const int kSize = 1024;
    static const UINT kSrvA = 70, kSrvB = 71;

    bool build(const WorldGpu& world, const std::vector<TreeLook>& looks, ID3D12Device* device,
               ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap, std::string& error);
    bool ready() const { return ready_; }
    float extent() const { return extent_; }   // spans [-extent/2, extent/2] in x and z
    double bake_ms() const { return bake_ms_; }
    float forested_share() const { return forested_; }   // share of land texels with cover > 0.05

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    float extent_ = 20000.0f;
    double bake_ms_ = 0.0;
    float forested_ = 0.0f;
    ComPtr<ID3D12Resource> tex_[2];
};

}  // namespace engine
