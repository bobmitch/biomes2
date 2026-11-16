#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/ddgi.h"
#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstring>

namespace engine {

constexpr float Ddgi::kSpacing[Ddgi::kCascades];

namespace {
// Matches cbuffer DdgiConstants in ddgi_update.hlsl.
struct DdgiConstants {
    float ray_rot[3][4];
    UINT  phase;  UINT frame;  float hysteresis;  UINT max_lights;
    float leaf[4];
};
static_assert(sizeof(DdgiConstants) == 80, "DdgiConstants must match ddgi_update.hlsl");
struct DdgiProbeCpu { float offset[4]; int coord[4]; };
static_assert(sizeof(DdgiProbeCpu) == 32, "DdgiProbe is 32 bytes (ddgi_common.hlsli)");

uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }
}  // namespace

bool Ddgi::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    auto texture = [&](ComPtr<ID3D12Resource>& out, int interior, DXGI_FORMAT fmt) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = UINT64(kDimX * kDimY * (interior + 2));
        td.Height = UINT(kDimZ * (interior + 2));
        td.DepthOrArraySize = UINT16(kCascades);
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_RESOURCE_ALLOCATION_INFO ai = device->GetResourceAllocationInfo(0, 1, &td);
        bytes_ += ai.SizeInBytes;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, read, nullptr, IID_PPV_ARGS(&out)));
    };
    auto buffer = [&](ComPtr<ID3D12Resource>& out, D3D12_HEAP_TYPE type, UINT64 bytes, bool uav,
                      D3D12_RESOURCE_STATES st) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = type;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        if (type == D3D12_HEAP_TYPE_DEFAULT) bytes_ += bytes;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
    };
    if (!texture(irr_, kIrr, DXGI_FORMAT_R16G16B16A16_FLOAT) || !texture(vis_, kVis, DXGI_FORMAT_R16G16_FLOAT)) {
        error = "ddgi atlases";
        return false;
    }
    // A committed default-heap buffer starts zeroed: every slot's coordinate
    // (0, 0, 0) with zero updates, which the readers take as "not traced".
    // Root UAVs must have ALLOW_UNORDERED_ACCESS (CLAUDE.md).
    if (!buffer(probes_, D3D12_HEAP_TYPE_DEFAULT, UINT64(kCascades) * kProbes * sizeof(DdgiProbeCpu), true, read) ||
        !buffer(rays_, D3D12_HEAP_TYPE_DEFAULT, UINT64(kCascades) * (kProbes / kPhases) * kRays * 16, true,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        error = "ddgi buffers";
        return false;
    }
    for (int f = 0; f < 2; ++f) {
        if (!buffer(cb_[f], D3D12_HEAP_TYPE_UPLOAD, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ) ||
            !buffer(cam_rb_[f], D3D12_HEAP_TYPE_READBACK, 256, false, D3D12_RESOURCE_STATE_COPY_DEST)) {
            error = "ddgi constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    if (!buffer(cam_, D3D12_HEAP_TYPE_DEFAULT, 256, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        error = "ddgi camera";
        return false;
    }

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(slot) * inc;
        return h;
    };
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = kCascades;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateShaderResourceView(irr_.Get(), &sd, cpu(kSrvIrr));
        sd.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateShaderResourceView(vis_.Get(), &sd, cpu(kSrvVis));
        D3D12_SHADER_RESOURCE_VIEW_DESC bd = {};
        bd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        bd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        bd.Format = DXGI_FORMAT_UNKNOWN;
        bd.Buffer.NumElements = UINT(kCascades * kProbes);
        bd.Buffer.StructureByteStride = sizeof(DdgiProbeCpu);
        device->CreateShaderResourceView(probes_.Get(), &bd, cpu(kSrvProbes));
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.ArraySize = kCascades;
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        device->CreateUnorderedAccessView(irr_.Get(), nullptr, &ud, cpu(kUavIrr));
        ud.Format = DXGI_FORMAT_R16G16_FLOAT;
        device->CreateUnorderedAccessView(vis_.Get(), nullptr, &ud, cpu(kUavVis));
    }

    // Root: the update's constants, the frame's shadow-and-light constants,
    // the TLAS and the RT scene's buffers, the lit pipelines' table (sun map,
    // local lights, canopy, these probes), the rays, the probes, the atlases
    // as UAVs. Samplers: the light atlas's comparison, the probes' linear.
    {
        D3D12_DESCRIPTOR_RANGE lit[6] = {};
        lit[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[0].NumDescriptors = 1; lit[0].BaseShaderRegister = 8;
        lit[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[1].NumDescriptors = 5; lit[1].BaseShaderRegister = 20;
        lit[1].OffsetInDescriptorsFromTableStart = 24;
        lit[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[2].NumDescriptors = 1; lit[2].BaseShaderRegister = 25;
        lit[2].OffsetInDescriptorsFromTableStart = 38;
        lit[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[3].NumDescriptors = 3; lit[3].BaseShaderRegister = 26;
        lit[3].OffsetInDescriptorsFromTableStart = 40;
        lit[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[4].NumDescriptors = 1; lit[4].BaseShaderRegister = 29;
        lit[4].OffsetInDescriptorsFromTableStart = 45;
        lit[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; lit[5].NumDescriptors = 5; lit[5].BaseShaderRegister = 38;
        lit[5].OffsetInDescriptorsFromTableStart = 48;
        D3D12_DESCRIPTOR_RANGE uav = {};
        uav.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uav.NumDescriptors = 2; uav.BaseShaderRegister = 2;
        D3D12_ROOT_PARAMETER p[15] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 2;
        for (int k = 0; k < 8; ++k) {
            p[2 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[2 + k].Descriptor.ShaderRegister = 30 + k;
        }
        p[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[10].DescriptorTable.NumDescriptorRanges = 6; p[10].DescriptorTable.pDescriptorRanges = lit;
        p[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[11].Descriptor.ShaderRegister = 0;
        p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[12].Descriptor.ShaderRegister = 1;
        p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[13].DescriptorTable.NumDescriptorRanges = 1; p[13].DescriptorTable.pDescriptorRanges = &uav;
        p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[14].Descriptor.ShaderRegister = 4;
        for (int k = 0; k < 15; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[2] = {};
        ss[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        ss[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        ss[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ss[0].MaxLOD = D3D12_FLOAT32_MAX;
        ss[0].ShaderRegister = 1;
        ss[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ss[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[1].MaxLOD = D3D12_FLOAT32_MAX;
        ss[1].ShaderRegister = 3;
        ss[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 15;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("ddgi root signature: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "ddgi CreateRootSignature";
            return false;
        }
    }
    const char* entries[5] = {"cs_ddgi_trace", "cs_ddgi_blend_irr", "cs_ddgi_blend_vis", "cs_ddgi_relocate",
                              "cs_ddgi_camera"};
    ComPtr<ID3D12PipelineState>* outs[5] = {&trace_pso_, &irr_pso_, &vis_pso_, &relocate_pso_, &camera_pso_};
    for (int k = 0; k < 5; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/ddgi_update.hlsl", entries[k], "cs_6_5", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "ddgi compute pso";
            return false;
        }
    }
    ready_ = true;
    return true;
}

bool Ddgi::count(ID3D12CommandQueue* queue, Counts& out, std::string& error, const float centre[3], float radius) {
    out = Counts();
    if (!ready_) return false;
    const UINT64 bytes = UINT64(kCascades) * kProbes * sizeof(DdgiProbeCpu);
    ComPtr<ID3D12Resource> rb;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&rb))) ||
        FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
        FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        error = "ddgi count readback";
        return false;
    }
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = probes_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = read;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    cl->CopyBufferRegion(rb.Get(), 0, probes_.Get(), 0, bytes);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = read;
    cl->ResourceBarrier(1, &b);
    cl->Close();
    ID3D12CommandList* lists[] = {cl.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
    const DdgiProbeCpu* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(const_cast<DdgiProbeCpu**>(&p))))) {
        error = "ddgi count map";
        return false;
    }
    const int dims[3] = {kDimX, kDimY, kDimZ};
    for (int c = 0; c < kCascades; ++c) {
        for (int i = 0; i < kProbes; ++i) {
            const DdgiProbeCpu& q = p[c * kProbes + i];
            // The world coordinate this slot should hold under the current base.
            const int s[3] = {i % kDimX, (i / kDimX) % kDimY, i / (kDimX * kDimY)};
            bool match = q.coord[3] >= 1;
            for (int k = 0; k < 3 && match; ++k) {
                const int rel = ((s[k] - base_[c][k]) % dims[k] + dims[k]) % dims[k];
                match = q.coord[k] == base_[c][k] + rel;
            }
            if (centre && radius > 0.0f) {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const int rel = ((s[k] - base_[c][k]) % dims[k] + dims[k]) % dims[k];
                    const float p = float(base_[c][k] + rel) * kSpacing[c] - centre[k];
                    d2 += p * p;
                }
                if (d2 > radius * radius) continue;
            }
            if (!match) ++out.pending[c];
            else if (q.offset[3] > 1.5f) ++out.idle[c];
            else if (q.offset[3] > 0.5f) ++out.inactive[c];
            else ++out.active[c];
        }
    }
    rb->Unmap(0, nullptr);
    return true;
}

void Ddgi::begin_frame(const float cam[3]) {
    const int dims[3] = {kDimX, kDimY, kDimZ};
    for (int c = 0; c < kCascades; ++c) {
        for (int k = 0; k < 3; ++k)
            base_[c][k] = int(std::floor(cam[k] / kSpacing[c])) - dims[k] / 2;
    }
}

void Ddgi::constants(bool on, float out[4][4]) const {
    std::memset(out, 0, sizeof(float) * 16);
    if (!on || !ready_) return;
    for (int c = 0; c < kCascades; ++c) {
        for (int k = 0; k < 3; ++k) out[c][k] = float(base_[c][k]);
        out[c][3] = kSpacing[c];
    }
    out[3][0] = 1.0f;
}

void Ddgi::record(ID3D12GraphicsCommandList* cmd, UINT frame, UINT slot, const Inputs& in) {
    if (!ready_) return;
    const UINT f = frame & 1u;
    const UINT rs = slot & 1u;
    // The camera's sky share this slot copied out two frames ago: landed.
    if (cam_pending_[rs]) {
        float* v = nullptr;
        D3D12_RANGE rr = {0, 13 * 16};
        if (SUCCEEDED(cam_rb_[rs]->Map(0, &rr, reinterpret_cast<void**>(&v)))) {
            camera_sky_ = (v[0] >= 0.0f && v[0] <= 1.0f) ? v[0] : 1.0f;
            std::memcpy(cam_irr_.E, v + 4, sizeof cam_irr_.E);
            std::memcpy(cam_irr_.open, v + 28, sizeof cam_irr_.open);
            D3D12_RANGE nr = {0, 0};
            cam_rb_[rs]->Unmap(0, &nr);
        }
        cam_pending_[rs] = false;
    }
    DdgiConstants dc = {};
    {
        // A uniformly random rotation, from a random unit quaternion (Shoemake).
        const uint32_t h = hash32(frame * 0x9E3779B9u + 0x51u);
        const float u1 = unit(h), u2 = unit(hash32(h + 1u)), u3 = unit(hash32(h + 2u));
        const float s1 = std::sqrt(1.0f - u1), s2 = std::sqrt(u1);
        const float x = s1 * std::sin(6.2831853f * u2), y = s1 * std::cos(6.2831853f * u2);
        const float z = s2 * std::sin(6.2831853f * u3), w = s2 * std::cos(6.2831853f * u3);
        const float R[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                               {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                               {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) dc.ray_rot[r][c] = R[r][c];
    }
    dc.phase = frame % kPhases;
    dc.frame = frame;
    // Each probe is updated every fourth frame; 0.92 a time is a few seconds
    // to follow a change of light, and keeps 128 rays from being noise.
    dc.hysteresis = 0.92f;
    dc.max_lights = in.max_lights;
    for (int k = 0; k < 3; ++k) dc.leaf[k] = in.leaf_radiance[k];
    std::memcpy(cb_cpu_[f], &dc, sizeof dc);

    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rb);
    };
    auto uav = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = r;
        cmd->ResourceBarrier(1, &rb);
    };

    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    const UINT inc = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE table = heap_->GetGPUDescriptorHandleForHeapStart(), uavs = table;
    uavs.ptr += UINT64(kUavIrr) * inc;
    const D3D12_GPU_VIRTUAL_ADDRESS any = rays_->GetGPUVirtualAddress();
    auto va = [&](D3D12_GPU_VIRTUAL_ADDRESS a) { return a ? a : any; };
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, in.shadow_cb);
    cmd->SetComputeRootShaderResourceView(2, in.tlas);
    cmd->SetComputeRootShaderResourceView(3, va(in.terrain_vertices));
    cmd->SetComputeRootShaderResourceView(4, va(in.terrain_indices));
    cmd->SetComputeRootShaderResourceView(5, va(in.terrain_materials));
    cmd->SetComputeRootShaderResourceView(6, va(in.tree_positions));
    cmd->SetComputeRootShaderResourceView(7, va(in.tree_indices));
    cmd->SetComputeRootShaderResourceView(8, va(in.tree_info));
    cmd->SetComputeRootShaderResourceView(9, va(in.tree_attributes));
    cmd->SetComputeRootDescriptorTable(10, table);
    cmd->SetComputeRootUnorderedAccessView(11, rays_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, probes_->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(13, uavs);
    cmd->SetComputeRootUnorderedAccessView(14, cam_->GetGPUVirtualAddress());

    const UINT per_phase = UINT(kProbes) / kPhases;
    cmd->SetPipelineState(trace_pso_.Get());
    cmd->Dispatch(per_phase, kCascades, 1);
    uav(rays_.Get());
    tr(irr_.Get(), read, ua);
    tr(vis_.Get(), read, ua);
    tr(probes_.Get(), read, ua);
    cmd->SetPipelineState(irr_pso_.Get());
    cmd->Dispatch(per_phase, kCascades, 1);
    cmd->SetPipelineState(vis_pso_.Get());
    cmd->Dispatch(per_phase, kCascades, 1);
    // The blends read each probe's coordinate; relocation rewrites it.
    uav(probes_.Get());
    cmd->SetPipelineState(relocate_pso_.Get());
    cmd->Dispatch((per_phase + 63) / 64, kCascades, 1);
    tr(irr_.Get(), ua, read);
    tr(vis_.Get(), ua, read);
    tr(probes_.Get(), ua, read);
    cmd->SetPipelineState(camera_pso_.Get());
    cmd->Dispatch(1, 1, 1);
    tr(cam_.Get(), ua, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyBufferRegion(cam_rb_[rs].Get(), 0, cam_.Get(), 0, 13 * 16);
    tr(cam_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, ua);
    cam_pending_[rs] = true;
    updates_ += UINT64(per_phase) * kCascades;
}

}  // namespace engine
