// ---------------------------------------------------------------------------
// Foam's fine structure: a tileable texture of BUBBLES, baked once.
//
// Up close sea foam is a froth of holes of many sizes in a white film. The
// sea's shader reads this at three scales (0.5 m, 12 cm and 3 cm cells; see
// water.hlsl) to break its foam up below the lace's half metre. A texture
// rather than the same pattern evaluated per pixel: the procedural version
// searched nine lattice points an octave, 27 a pixel, and cost +0.4 ms of the
// sea pass at 1440p wherever foam filled the view -- and the mip chain is the
// filtering that fades each octave into its mean as it shrinks under a pixel,
// for free and without aliasing.
//
// 512 x 512 texels, 16 x 16 cells (32 texels a cell), RGBA8 with a full mip
// chain. Each cell holds one jittered point, the centre of a hole of its own
// radius (0.45-0.9 of a cell). b = min over points of |p - q| / r -- the
// minimum of distance over radius, so b is continuous where holes of
// different sizes straddle a cell border:
//   R  saturate(b): 0 at a hole's centre, 1 in the film between holes
//   GB the gradient of b (cells^-1), / 2.5, biased to 0.5: the foam's relief
//   A  saturate(b / 2): whether a texel is in a hole (b < 1) or the film
// mean() is R's average over the tile, which is what the smallest mip holds;
// the shader fades each octave into it, so the foam's coverage does not move
// as detail comes and goes.
// ---------------------------------------------------------------------------
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace engine {

class FoamPattern {
public:
    static constexpr int kSize = 512;
    static constexpr int kCells = 16;

    // Bakes, uploads (one wait) and writes the SRV at `heap`[slot].
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12DescriptorHeap* heap, UINT slot,
              std::string& error);
    float mean() const { return mean_; }
    double bake_ms() const { return bake_ms_; }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> tex_;
    float mean_ = 0.6f;
    double bake_ms_ = 0.0;
};

}  // namespace engine
