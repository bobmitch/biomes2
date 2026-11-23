#pragma once
// ---------------------------------------------------------------------------
// Rain (docs/adr-005-clouds-and-rain.md, phase C3; shaders/rain.hlsl):
// streaks round the camera and splashes on whatever is near and facing up,
// drawn onto the lit scene after the sea. Procedural -- no buffers, a drop is
// a hash of its index -- and kept out from under the crowns by the canopy
// map. Presentation only.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>
#include <string>

namespace engine {

class Rain {
public:
    static const unsigned kNearDrops = 40000;   // in a 44 x 24 x 44 m box
    static const unsigned kFarDrops = 60000;    // in a 132 x 36 x 132 m one
    static const unsigned kMaxSplashes = 2500;   // in a 72 m square round the camera, a quarter in view

    struct Frame {
        float view_proj[16];       // this frame, jittered: the scene's
        float inv_view_proj[16];
        float cam_pos[3];
        float time;
        float intensity;           // 0 none .. 1 a downpour
        float drip;                // 0..1 how wet the crowns are: drops under them, after the rain too
        float wind[2];             // m/s, x and z
        float light[3];            // what a drop sends back, nits
        float near_plane, far_plane;
        float canopy[4];           // the canopy map's min corner x, z, reference height; w > 0.5 built
        float hf[4];               // the heightfield GI map's min corner x, z; w > 0.5 built
        unsigned width, height;
        float tan_half_fov;
    };

    // `table` is the heap position the root table starts at, with the depth
    // SRV there, the canopy map `canopy_offset` past it and the heightfield
    // GI's height `hf_offset` past it.
    bool init(ID3D12Device* device, DXGI_FORMAT target, unsigned canopy_offset, unsigned hf_offset,
              std::string& error);
    // Draws onto `rtv` (in RENDER_TARGET). The caller has the renderer's heap
    // bound; `table` is its GPU handle at the depth SRV.
    void record(ID3D12GraphicsCommandList* cmd, unsigned frame, const Frame& f,
                D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_GPU_DESCRIPTOR_HANDLE table);
    bool ready() const { return ready_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool ready_ = false;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> streak_pso_, splash_pso_;
    ComPtr<ID3D12Resource> cb_[2];
    void* cb_cpu_[2] = {};
};

}  // namespace engine
