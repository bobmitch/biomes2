#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/rt_check.h"
#include "engine/render/shader_compile.h"

#include <windows.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace engine {
namespace {

using Microsoft::WRL::ComPtr;

const char* sm_name(D3D_SHADER_MODEL m) {
    switch (m) {
    case D3D_SHADER_MODEL_5_1: return "5.1";
    case D3D_SHADER_MODEL_6_0: return "6.0";
    case D3D_SHADER_MODEL_6_1: return "6.1";
    case D3D_SHADER_MODEL_6_2: return "6.2";
    case D3D_SHADER_MODEL_6_3: return "6.3";
    case D3D_SHADER_MODEL_6_4: return "6.4";
    case D3D_SHADER_MODEL_6_5: return "6.5";
    case D3D_SHADER_MODEL_6_6: return "6.6";
    case D3D_SHADER_MODEL_6_7: return "6.7";
    default: return "6.8+";
    }
}

bool buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
            D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes ? bytes : 256;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                  IID_PPV_ARGS(&out)));
}

// The CPU's answer: does the ray at (x, y) hit the triangle, and with which
// barycentrics (D3D's: weight of vertex 1 and vertex 2).
bool cpu_hit(float x, float y, float& b1, float& b2) {
    const float ax = -1, ay = -1, bx = 1, by = -1, cx = 0, cy = 1;
    const float d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
    const float w0 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d;
    const float w1 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d;
    const float w2 = 1.0f - w0 - w1;
    b1 = w1; b2 = w2;
    return w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f;
}

}  // namespace

DeviceCaps query_caps(ID3D12Device* device) {
    DeviceCaps c;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5)))
        c.raytracing = o5.RaytracingTier;
    // Ask from the top down: a runtime that does not know a model rejects it.
    const D3D_SHADER_MODEL ask[] = {D3D_SHADER_MODEL_6_7, D3D_SHADER_MODEL_6_6, D3D_SHADER_MODEL_6_5,
                                    D3D_SHADER_MODEL_6_4, D3D_SHADER_MODEL_6_3, D3D_SHADER_MODEL_6_0};
    for (size_t i = 0; i < sizeof(ask) / sizeof(ask[0]); ++i) {
        D3D12_FEATURE_DATA_SHADER_MODEL sm = {ask[i]};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) {
            c.shader_model = sm.HighestShaderModel;
            break;
        }
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7 = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7)))
        c.mesh_shaders = o7.MeshShaderTier;
    return c;
}

std::string describe(const DeviceCaps& c) {
    char buf[256];
    const char* rt = c.raytracing >= D3D12_RAYTRACING_TIER_1_1 ? "DXR 1.1"
                   : c.raytracing >= D3D12_RAYTRACING_TIER_1_0 ? "DXR 1.0 (no inline RayQuery)"
                   : "no ray tracing";
    std::snprintf(buf, sizeof buf, "%s, shader model %s, mesh shaders %s", rt,
                  sm_name(c.shader_model),
                  c.mesh_shaders != D3D12_MESH_SHADER_TIER_NOT_SUPPORTED ? "yes" : "no");
    return buf;
}

bool run_rt_check(ID3D12Device* device, ID3D12CommandQueue* queue, const DeviceCaps& caps,
                  const char* report_path) {
    FILE* f = std::fopen(report_path, "w");
    if (!f) return false;
    std::fprintf(f, "device: %s\n", describe(caps).c_str());
    Dxc& d = dxc();
    std::fprintf(f, "DXC: %s", d.ok() ? "loaded" : ("NOT AVAILABLE -- " + d.why).c_str());
    if (d.ok()) std::fprintf(f, ", version %llu.%llu", d.version >> 32, d.version & 0xFFFFFFFFull);
    std::fprintf(f, "\n");
    auto fail = [&](const char* why) {
        std::fprintf(f, "\nFAIL: %s\n", why);
        std::fclose(f);
        return false;
    };
    if (caps.raytracing < D3D12_RAYTRACING_TIER_1_1) return fail("the device has no DXR 1.1 (inline RayQuery)");

    ComPtr<ID3D12Device5> dev5;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dev5)))) return fail("no ID3D12Device5");

    // --- the shader, first: a DXC failure is the likeliest one ---
    const double tc = startup_now_ms();
    ComPtr<ID3DBlob> cs;
    std::string err;
    if (!compile_shader("engine/render/shaders/rt_check.hlsl", "cs_rays", "cs_6_5", cs, err)) {
        std::fprintf(f, "\n%s\n", err.c_str());
        return fail("the SM 6.5 shader did not compile");
    }
    std::fprintf(f, "SM 6.5 compute shader with inline RayQuery: compiled, %zu bytes, %.0f ms\n",
                 size_t(cs->GetBufferSize()), startup_now_ms() - tc);

    // --- the triangle, its BLAS and TLAS ---
    const float tri[9] = {-1, -1, 5, 1, -1, 5, 0, 1, 5};
    ComPtr<ID3D12Resource> vb, inst, blas, tlas, scratch_b, scratch_t, out, rb;
    if (!buffer(device, sizeof tri, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, vb)) return fail("vertex buffer");
    void* p = nullptr;
    vb->Map(0, nullptr, &p);
    std::memcpy(p, tri, sizeof tri);
    vb->Unmap(0, nullptr);

    D3D12_RAYTRACING_GEOMETRY_DESC geo = {};
    geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geo.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geo.Triangles.VertexBuffer.StartAddress = vb->GetGPUVirtualAddress();
    geo.Triangles.VertexBuffer.StrideInBytes = 12;
    geo.Triangles.VertexCount = 3;
    geo.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bin = {};
    bin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    bin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    bin.NumDescs = 1;
    bin.pGeometryDescs = &geo;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bpi = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&bin, &bpi);
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!buffer(device, bpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, blas) ||
        !buffer(device, bpi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, scratch_b)) return fail("BLAS buffers");

    D3D12_RAYTRACING_INSTANCE_DESC id = {};
    id.Transform[0][0] = id.Transform[1][1] = id.Transform[2][2] = 1.0f;
    id.InstanceMask = 0xFF;
    id.AccelerationStructure = blas->GetGPUVirtualAddress();
    if (!buffer(device, sizeof id, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_GENERIC_READ, inst)) return fail("instance buffer");
    inst->Map(0, nullptr, &p);
    std::memcpy(p, &id, sizeof id);
    inst->Unmap(0, nullptr);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tin = {};
    tin.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tin.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    tin.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tin.NumDescs = 1;
    tin.InstanceDescs = inst->GetGPUVirtualAddress();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tpi = {};
    dev5->GetRaytracingAccelerationStructurePrebuildInfo(&tin, &tpi);
    if (!buffer(device, tpi.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, tlas) ||
        !buffer(device, tpi.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, scratch_t)) return fail("TLAS buffers");
    if (!buffer(device, 64 * 16, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, out) ||
        !buffer(device, 64 * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST, rb)) return fail("output buffers");

    // --- pipeline: a root SRV for the TLAS, a root UAV for the results ---
    ComPtr<ID3D12RootSignature> root;
    {
        D3D12_ROOT_PARAMETER rp[2] = {};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        rp[0].ShaderVisibility = rp[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = rp;
        ComPtr<ID3DBlob> sig, e2;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &e2)) ||
            FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&root)))) return fail("root signature");
    }
    ComPtr<ID3D12PipelineState> pso;
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(&pso))))
            return fail("CreateComputePipelineState refused the DXIL (unsigned? dxil.dll missing?)");
    }

    // --- record: BLAS, barrier, TLAS, barrier, rays, copy back ---
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList4> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return fail("command list");
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC bd = {};
    bd.Inputs = bin;
    bd.DestAccelerationStructureData = blas->GetGPUVirtualAddress();
    bd.ScratchAccelerationStructureData = scratch_b->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&bd, 0, nullptr);
    D3D12_RESOURCE_BARRIER ub = {};
    ub.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    ub.UAV.pResource = blas.Get();
    cl->ResourceBarrier(1, &ub);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC td = {};
    td.Inputs = tin;
    td.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
    td.ScratchAccelerationStructureData = scratch_t->GetGPUVirtualAddress();
    cl->BuildRaytracingAccelerationStructure(&td, 0, nullptr);
    ub.UAV.pResource = tlas.Get();
    cl->ResourceBarrier(1, &ub);
    cl->SetComputeRootSignature(root.Get());
    cl->SetPipelineState(pso.Get());
    cl->SetComputeRootShaderResourceView(0, tlas->GetGPUVirtualAddress());
    cl->SetComputeRootUnorderedAccessView(1, out->GetGPUVirtualAddress());
    cl->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER tb = {};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource = out.Get();
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &tb);
    cl->CopyBufferRegion(rb.Get(), 0, out.Get(), 0, 64 * 16);
    cl->Close();
    ID3D12CommandList* ls[] = {cl.Get()};
    const double tg = startup_now_ms();
    queue->ExecuteCommandLists(1, ls);
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    if (fence->GetCompletedValue() < 1) return fail("the GPU did not finish in 10 s");
    std::fprintf(f, "BLAS %llu bytes, TLAS %llu bytes; built and traced in %.1f ms\n",
                 static_cast<unsigned long long>(bpi.ResultDataMaxSizeInBytes),
                 static_cast<unsigned long long>(tpi.ResultDataMaxSizeInBytes), startup_now_ms() - tg);

    // --- compare with the CPU ---
    float* r = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&r));
    int want_hits = 0, got_hits = 0, wrong = 0;
    float worst_t = 0.0f, worst_b = 0.0f;
    for (int i = 0; i < 64; ++i) {
        const float x = -1.2f + 2.4f * (float(i % 8) + 0.5f) / 8.0f;
        const float y = -1.2f + 2.4f * (float(i / 8) + 0.5f) / 8.0f;
        float b1 = 0, b2 = 0;
        const bool want = cpu_hit(x, y, b1, b2);
        const bool got = r[i * 4] > 0.5f;
        want_hits += want; got_hits += got;
        if (want != got) { ++wrong; continue; }
        if (got) {
            worst_t = std::fmax(worst_t, std::fabs(r[i * 4 + 1] - 5.0f));
            worst_b = std::fmax(worst_b, std::fmax(std::fabs(r[i * 4 + 2] - b1), std::fabs(r[i * 4 + 3] - b2)));
        }
    }
    rb->Unmap(0, nullptr);
    std::fprintf(f, "64 rays: the CPU expects %d hits, the GPU reports %d; %d disagree\n",
                 want_hits, got_hits, wrong);
    std::fprintf(f, "worst hit distance error %.2e m (t = 5), worst barycentric error %.2e\n",
                 worst_t, worst_b);
    const bool pass = wrong == 0 && want_hits > 0 && worst_t < 1e-4f && worst_b < 1e-4f;
    std::fprintf(f, "\n%s\n", pass ? "PASS: hardware ray tracing works end to end" : "FAIL");
    std::fclose(f);
    return pass;
}

}  // namespace engine
