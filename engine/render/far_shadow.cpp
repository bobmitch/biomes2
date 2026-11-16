#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/far_shadow.h"
#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstring>

namespace engine {

constexpr float FarShadow::kTexel;
constexpr float FarShadow::kExtent;

namespace {
// Matches cbuffer FarConstants in far_shadow.hlsl.
struct FarConstants {
    float origin_xz[2]; float y_top; float y_range;
    float key_dir[3];   float extent;
};
static_assert(sizeof(FarConstants) == 32, "FarConstants must match far_shadow.hlsl");
const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}  // namespace

bool FarShadow::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    auto tex = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags,
                   const D3D12_CLEAR_VALUE* clear) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kSize;
        td.Height = kSize;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = flags;
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, kRead, clear, IID_PPV_ARGS(&out)));
    };
    D3D12_CLEAR_VALUE cd = {};
    cd.Format = DXGI_FORMAT_D32_FLOAT;
    cd.DepthStencil.Depth = 1.0f;
    if (!tex(depth_, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, &cd) ||
        !tex(vis_, DXGI_FORMAT_R16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, nullptr)) {
        error = "far shadow targets";
        return false;
    }
    for (int f = 0; f < 2; ++f) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 256;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&cb_[f])))) {
            error = "far shadow constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)))) { error = "far shadow dsv"; return false; }
        D3D12_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depth_.Get(), &dv, dsv_heap_->GetCPUDescriptorHandleForHeapStart());
    }
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(slot) * inc;
        return h;
    };
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        device->CreateShaderResourceView(depth_.Get(), &sd, cpu(kSrvDepth));
        sd.Format = DXGI_FORMAT_R16_FLOAT;
        device->CreateShaderResourceView(vis_.Get(), &sd, cpu(kSrvVis));
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(vis_.Get(), nullptr, &ud, cpu(kUavVis));
    }
    {
        D3D12_DESCRIPTOR_RANGE sr = {}, ur = {};
        sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr.NumDescriptors = 1; sr.BaseShaderRegister = 0;
        sr.OffsetInDescriptorsFromTableStart = kSrvDepth;
        ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ur.NumDescriptors = 1; ur.BaseShaderRegister = 0;
        ur.OffsetInDescriptorsFromTableStart = kUavVis;
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1; p[1].DescriptorTable.pDescriptorRanges = &sr;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1; p[2].DescriptorTable.pDescriptorRanges = &ur;
        for (int k = 0; k < 3; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss = {};
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;   // heights are not interpolated across a missing texel
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &ss;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("far shadow root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "far shadow CreateRootSignature";
            return false;
        }
    }
    ComPtr<ID3DBlob> cs;
    if (!compile_shader("engine/render/shaders/far_shadow.hlsl", "cs_far_march", "cs_5_1", cs, error)) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC cdsc = {};
    cdsc.pRootSignature = root_.Get();
    cdsc.CS.pShaderBytecode = cs->GetBufferPointer();
    cdsc.CS.BytecodeLength = cs->GetBufferSize();
    if (FAILED(device->CreateComputePipelineState(&cdsc, IID_PPV_ARGS(&march_pso_)))) {
        error = "far shadow pso";
        return false;
    }
    ready_ = true;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE FarShadow::dsv() const { return dsv_heap_->GetCPUDescriptorHandleForHeapStart(); }

bool FarShadow::begin(const float cam[3], const float key_dir[3], unsigned long long frame, float vp[16]) {
    if (!ready_) return false;
    const float dx = cam[0] - centre_[0], dz = cam[2] - centre_[2];
    const float dot = key_dir[0] * key_[0] + key_dir[1] * key_[1] + key_dir[2] * key_[2];
    // 256 m of travel, a quarter of a degree of the light, or ten seconds.
    const bool due = !built_ || dx * dx + dz * dz > 256.0f * 256.0f || dot < 0.99999f || frame - last_frame_ >= 600;
    if (!due) return false;
    for (int k = 0; k < 3; ++k) { centre_[k] = cam[k]; key_[k] = key_dir[k]; }
    last_frame_ = frame;
    origin_[0] = std::floor((cam[0] - 0.5f * kExtent) / kTexel) * kTexel;
    origin_[1] = std::floor((cam[2] - 0.5f * kExtent) / kTexel) * kTexel;
    y_top_ = cam[1] + 2000.0f;
    y_range_ = 4000.0f;
    std::memset(vp, 0, sizeof(float) * 16);
    vp[0 * 4 + 0] = 2.0f / kExtent;
    vp[2 * 4 + 1] = 2.0f / kExtent;
    vp[1 * 4 + 2] = -1.0f / y_range_;
    vp[3 * 4 + 0] = -2.0f * origin_[0] / kExtent - 1.0f;
    vp[3 * 4 + 1] = -2.0f * origin_[1] / kExtent - 1.0f;
    vp[3 * 4 + 2] = y_top_ / y_range_;
    vp[3 * 4 + 3] = 1.0f;
    return true;
}

void FarShadow::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    const UINT f = frame & 1u;
    FarConstants fc = {};
    fc.origin_xz[0] = origin_[0];
    fc.origin_xz[1] = origin_[1];
    fc.y_top = y_top_;
    fc.y_range = y_range_;
    for (int k = 0; k < 3; ++k) fc.key_dir[k] = key_[k];
    fc.extent = kExtent;
    std::memcpy(cb_cpu_[f], &fc, sizeof fc);
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rb);
    };
    tr(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
    tr(vis_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    const D3D12_GPU_DESCRIPTOR_HANDLE base = heap_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, base);
    cmd->SetComputeRootDescriptorTable(2, base);
    cmd->SetPipelineState(march_pso_.Get());
    cmd->Dispatch(kSize / 8, kSize / 8, 1);
    tr(vis_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead);
    built_ = true;
    ++rebuilds_;
}

void FarShadow::constants(bool on, float out[4]) const {
    out[0] = origin_[0];
    out[1] = origin_[1];
    out[2] = kExtent;
    out[3] = (on && built_) ? 1.0f : 0.0f;
}

}  // namespace engine
