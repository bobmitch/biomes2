// ---------------------------------------------------------------------------
// What the device can do for ADR-004's ray-traced lighting, and a proof that
// it does it: --check-rt builds a one-triangle BLAS and TLAS, casts 64 rays at
// it from an SM 6.5 compute shader with inline RayQuery, and compares every
// result with the CPU's answer.
// ---------------------------------------------------------------------------
#ifndef ENGINE_RT_CHECK_H
#define ENGINE_RT_CHECK_H

#include <d3d12.h>
#include <string>

namespace engine {

struct DeviceCaps {
    D3D12_RAYTRACING_TIER raytracing = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D_SHADER_MODEL      shader_model = D3D_SHADER_MODEL_5_1;
    D3D12_MESH_SHADER_TIER mesh_shaders = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    std::string adapter;
};

DeviceCaps query_caps(ID3D12Device* device);
std::string describe(const DeviceCaps& c);

// Writes `report_path`; true if every ray agreed with the CPU.
bool run_rt_check(ID3D12Device* device, ID3D12CommandQueue* queue, const DeviceCaps& caps,
                  const char* report_path);

}  // namespace engine

#endif  // ENGINE_RT_CHECK_H
