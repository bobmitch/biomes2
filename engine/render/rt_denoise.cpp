#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/rt_denoise.h"
#include "engine/render/shader_compile.h"

#include <cstdlib>
#include <cstring>

namespace engine {

namespace {
// Matches cbuffer DenoiseConstants in rt_denoise.hlsl.
struct DenoiseConstants {
    float inv_view_proj[16];
    float clip_to_prev[16];
    float cam_pos[3];  UINT width;
    float prev_cam[3]; UINT height;
    float jitter[2];   float prev_jitter[2];
    UINT  history_ok;  UINT frame; UINT sparse; UINT pad0;
};
static_assert(sizeof(DenoiseConstants) == 192, "DenoiseConstants must match rt_denoise.hlsl");
}  // namespace

bool RtDenoise::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT width, UINT height, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    width_ = width ? width : 1;
    height_ = height ? height : 1;
    {
        // The histories and ping-pong buffers are read back as UAVs.
        D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)) ||
            !o.TypedUAVLoadAdditionalFormats) {
            error = "rt denoise: typed UAV loads are not supported";
            return false;
        }
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
            error = "rt denoise constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    if (!create_targets(error)) return false;
    {
        // b0 the frame's constants, b1 the pass's (root constants); t0 the
        // depth at [3]; u0 the visibility at [37]; u1..u6 ours from [72].
        D3D12_DESCRIPTOR_RANGE sr = {}, uv = {}, ur = {};
        sr.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr.NumDescriptors = 1; sr.BaseShaderRegister = 0;
        sr.OffsetInDescriptorsFromTableStart = 3;
        uv.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; uv.NumDescriptors = 1; uv.BaseShaderRegister = 0;
        uv.OffsetInDescriptorsFromTableStart = 37;
        ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ur.NumDescriptors = 7; ur.BaseShaderRegister = 1;
        ur.OffsetInDescriptorsFromTableStart = kUavHist0;
        D3D12_DESCRIPTOR_RANGE ranges[3] = {sr, uv, ur};
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[1].Constants.ShaderRegister = 1;
        p[1].Constants.Num32BitValues = 4;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 3;
        p[2].DescriptorTable.pDescriptorRanges = ranges;
        for (int k = 0; k < 3; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("rt denoise root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "rt denoise CreateRootSignature";
            return false;
        }
    }
    auto pso = [&](const char* entry, ComPtr<ID3D12PipelineState>& out) -> bool {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/rt_denoise.hlsl", entry, "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC d = {};
        d.pRootSignature = root_.Get();
        d.CS.pShaderBytecode = cs->GetBufferPointer();
        d.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&d, IID_PPV_ARGS(&out)))) {
            error = std::string("rt denoise pso ") + entry;
            return false;
        }
        return true;
    };
    if (!pso("cs_den_temporal", temporal_pso_) || !pso("cs_den_atrous", atrous_pso_)) return false;
    ready_ = true;
    return true;
}

bool RtDenoise::create_targets(std::string& error) {
    auto tex = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT fmt) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width_;
        td.Height = height_;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                       nullptr, IID_PPV_ARGS(&out)));
    };
    const DXGI_FORMAT fmt[3] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT};
    ComPtr<ID3D12Resource>* sets[3] = {hist_, dist_, tmp_};
    const UINT inc = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int s = 0; s < 3; ++s) {
        for (int k = 0; k < 2; ++k) {
            if (!tex(sets[s][k], fmt[s])) {
                error = "rt denoise targets";
                return false;
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = fmt[s];
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(kUavHist0 + s * 2 + k) * inc;
            dev_->CreateUnorderedAccessView(sets[s][k].Get(), nullptr, &ud, h);
        }
    }
    if (!tex(raw_, DXGI_FORMAT_R32_FLOAT)) {
        error = "rt denoise sparse trace target";
        return false;
    }
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kUavRaw) * inc;
        dev_->CreateUnorderedAccessView(raw_.Get(), nullptr, &ud, h);
    }
    last_run_ = 0;
    return true;
}

bool RtDenoise::resize(UINT width, UINT height, std::string& error) {
    if (!ready_ || (width == width_ && height == height_)) return true;
    width_ = width ? width : 1;
    height_ = height ? height : 1;
    return create_targets(error);
}

void RtDenoise::record(ID3D12GraphicsCommandList* cmd, UINT frame, const Inputs& in) {
    if (!ready_) return;
    const UINT f = frame & 1u;
    DenoiseConstants dc = {};
    std::memcpy(dc.inv_view_proj, in.inv_view_proj, sizeof dc.inv_view_proj);
    std::memcpy(dc.clip_to_prev, in.clip_to_prev, sizeof dc.clip_to_prev);
    for (int k = 0; k < 3; ++k) { dc.cam_pos[k] = in.cam[k]; dc.prev_cam[k] = in.prev_cam[k]; }
    dc.width = width_;
    dc.height = height_;
    dc.sparse = in.sparse ? 1u : 0u;
    dc.jitter[0] = in.jitter[0]; dc.jitter[1] = in.jitter[1];
    dc.prev_jitter[0] = in.prev_jitter[0]; dc.prev_jitter[1] = in.prev_jitter[1];
    dc.history_ok = in.history_ok ? 1u : 0u;
    dc.frame = parity_;   // the history this frame reads; the a-trous finds this frame's distances from it
    std::memcpy(cb_cpu_[f], &dc, sizeof dc);

    auto uav = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        cmd->ResourceBarrier(1, &b);
    };
    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(2, heap_->GetGPUDescriptorHandleForHeapStart());
    const UINT gx = (width_ + 7) / 8, gy = (height_ + 7) / 8;
    // BIOMES_DEN_DBG=1: the temporal pass alone; 2: each pixel's history
    // length in place of the shadow.
    static const UINT dbg = std::getenv("BIOMES_DEN_DBG") ? UINT(std::atoi(std::getenv("BIOMES_DEN_DBG"))) : 0u;
    UINT pc[4] = {0u, parity_, 0u, dbg};
    cmd->SetComputeRoot32BitConstants(1, 4, pc, 0);
    cmd->SetPipelineState(temporal_pso_.Get());
    cmd->Dispatch(gx, gy, 1);
    uav(nullptr);
    cmd->SetPipelineState(atrous_pso_.Get());
    // BIOMES_DEN_DBG=1: the temporal pass alone (every a-trous step 0,
    // which returns its input) -- with BIOMES_DEN_NOHIST, the spatial alone.
    static const bool temporal_only = dbg != 0u;
    for (UINT k = 0; k < kPasses; ++k) {
        pc[0] = temporal_only ? 0u : 1u << k;   // step
        pc[1] = k & 1u;           // read tmp[0] first: the temporal pass wrote it
        pc[2] = k + 1 == kPasses ? 1u : 0u;
        cmd->SetComputeRoot32BitConstants(1, 4, pc, 0);
        cmd->Dispatch(gx, gy, 1);
        uav(nullptr);
    }
    parity_ ^= 1u;
    last_run_ = frame;
}

}  // namespace engine
