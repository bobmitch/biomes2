#pragma once
// ---------------------------------------------------------------------------
// The ray-traced key-light shadow's denoiser (shaders/rt_denoise.hlsl).
//
// rt_shadow.hlsl traces ONE ray a pixel a frame towards a random point on
// the sun's disk. Left to TAA, that is a penumbra made of grain: TAA was
// built for sub-pixel jitter, clips its history to each pixel's 3x3
// neighbourhood, and a 1-sample shadow's neighbourhood is 0 and 1 side by
// side, so moving or not, the grain comes through. This filters the shadow
// on its own first: temporal accumulation with variance (SVGF's), anti-lag
// from a fast second history, then four edge-aware a-trous passes, written
// back into the visibility the composite reads. `--no-rt-denoise` (F8 in
// the window) for A/B.
//
// SPARSE TERRAIN, an option (`--rt-sparse-terrain`, Shift+F8): the
// trace takes terrain at one pixel of each 2x2 quad a frame, rotating, and
// everything else -- trees, trunks, grass, rocks, water -- at every pixel
// every frame. A skipped pixel carries its history; one with none is filled
// by the a-trous from its traced neighbours. Ground shadows are soft and
// mostly still; a crown's are neither, and tracing whole frames at half
// resolution (one ray per quad for everything, upsampled) BOILED on trees.
//
// Targets: two RGBA16F histories, two R32F distances, two RG16F ping-pong
// buffers and the sparse trace (R32F) -- ~35 MB at 1280 x 720, ~140 MB at
// 2560 x 1440. Heap [72..78], UAVs only.
//
// Presentation only; nothing here is upstream of the golden hash.
// ---------------------------------------------------------------------------
#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace engine {

class RtDenoise {
public:
    static const UINT kUavHist0 = 72;   // [72..73] histories, [74..75] distances, [76..77] ping-pong
    static const UINT kUavRaw = 78;     // the sparse trace (rt_shadow.hlsl's u2)
    static const UINT kHeapEnd = 79;
    static const UINT kPasses = 4;      // a-trous steps 1, 2, 4, 8

    bool init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT width, UINT height, std::string& error);
    // Recreates the targets at a new size; the history starts over.
    bool resize(UINT width, UINT height, std::string& error);
    bool ready() const { return ready_; }

    struct Inputs {
        float inv_view_proj[16];   // this frame's jittered
        float clip_to_prev[16];    // TAA's reprojection, this frame's unjittered clip to last frame's
        float cam[3], prev_cam[3];
        float jitter[2], prev_jitter[2];
        bool history_ok;           // false after a jump, a resize, or a frame without it
        bool sparse;               // the trace wrote the raw target, -1 where it skipped terrain
    };
    // Between the trace (rt_vis UAV at heap [37], in UAV state) and the
    // composite; depth at heap [3] readable by compute.
    void record(ID3D12GraphicsCommandList* cmd, UINT frame, const Inputs& in);
    // The last frame it ran: a gap means the history is stale.
    unsigned long long last_run() const { return last_run_; }

private:
    template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
    bool create_targets(std::string& error);
    bool ready_ = false;
    UINT width_ = 0, height_ = 0, parity_ = 0;
    unsigned long long last_run_ = 0;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    ComPtr<ID3D12Resource> hist_[2], dist_[2], tmp_[2], raw_, cb_[2];
    void* cb_cpu_[2] = {};
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> temporal_pso_, atrous_pso_;
};

}  // namespace engine
