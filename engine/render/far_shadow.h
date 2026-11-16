#pragma once
// ---------------------------------------------------------------------------
// The far terrain shadow: the key light's shadow past the cascades.
//
// The cascades end at 1.6 km. Past them, a coarse map of the terrain seen
// from above -- 1024 x 1024 texels of 8 m, 8 km round the camera, drawn
// depth-only from the resident clipmap (the coarsest ring reaches ~7 km) --
// is marched towards the key light per texel, with a penumbra that widens
// with distance, into a visibility map. shadow.hlsli blends the last
// cascade into it over that cascade's fade and reads it alone beyond, out to
// ~3.6 km of march, fading over the map's last tenth. Terrain only: trees
// past the draw distance are not drawn anyway. Rebuilt when the camera has
// moved 256 m, the light a quarter of a degree, or ten seconds have passed.
//
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace engine {

class FarShadow {
public:
    static const UINT  kSize = 1024;
    static constexpr float kTexel = 8.0f;
    static constexpr float kExtent = 8192.0f;
    // Heap: [64] the depth (the march reads it), [65] the visibility (t43,
    // the lit tables' seventh range), [66] its UAV.
    static const UINT  kSrvDepth = 64, kSrvVis = 65, kUavVis = 66;

    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error);
    bool ready() const { return ready_; }

    // Due: fixes this rebuild's map and returns its top-down view-projection
    // (row-vector) in `vp`, for the renderer to draw the terrain into dsv().
    bool begin(const float cam[3], const float key_dir[3], unsigned long long frame, float vp[16]);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv() const;
    ID3D12Resource* depth() const { return depth_.Get(); }
    // Marches the map; the visibility ends readable by pixel and compute.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame);
    // ShadowConstants' l_far: the map's min corner x, z, its extent; w on.
    void constants(bool on, float out[4]) const;
    unsigned long long rebuilds() const { return rebuilds_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false, built_ = false;
    float origin_[2] = {}, centre_[3] = {}, key_[3] = {0, 0, 0}, y_top_ = 0.0f, y_range_ = 0.0f;
    unsigned long long last_frame_ = 0, rebuilds_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_, dsv_heap_;
    ComPtr<ID3D12Resource> depth_, vis_, cb_[2];
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> march_pso_;
};

}  // namespace engine
