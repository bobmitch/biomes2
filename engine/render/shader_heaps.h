#pragma once
// The shader-visible descriptor heaps every pass binds: the renderer's one
// CBV/SRV/UAV heap, and the scans' sampler heap (Renderer: s5, one sampler
// per anisotropy level, so the Graphics option is a different table
// pointer, live, with no pipeline rebuilt).
//
// SetDescriptorHeaps UNBINDS every heap it is not given. A pass that bound
// only the first would leave the terrain, rock, character and tree
// pipelines with no sampler heap, and this machine has no debug layer to say
// so. So every binding in the engine goes through bind_shader_heaps. The
// UI's renderer, which draws last with a heap of its own, is the one
// exception.

#include <d3d12.h>

namespace engine {

struct ShaderHeaps {
    ID3D12DescriptorHeap* srv = nullptr;
    ID3D12DescriptorHeap* sampler = nullptr;
};

// One instance for the program (an inline function's static is shared by
// every translation unit). The renderer fills it at init.
inline ShaderHeaps& shader_heaps() {
    static ShaderHeaps h;
    return h;
}

inline void bind_shader_heaps(ID3D12GraphicsCommandList* cmd) {
    const ShaderHeaps& h = shader_heaps();
    ID3D12DescriptorHeap* heaps[2] = {h.srv, h.sampler};
    cmd->SetDescriptorHeaps(h.sampler ? 2u : 1u, heaps);
}

}  // namespace engine
