#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/height_gi.h"
#include "engine/render/near_field.h"   // NearField::kSrvVis
#include "engine/render/shader_compile.h"
#include "engine/render/shader_heaps.h"

#include <cmath>
#include <cstring>

namespace engine {

constexpr float HeightGi::kTexel;
constexpr float HeightGi::kExtent;

namespace {
// Matches cbuffer HfConstants in height_gi.hlsl.
struct HfConstants {
    float origin_xz[2]; float y_top; float y_range;
    UINT  first;        float pad0[3];
};
static_assert(sizeof(HfConstants) == 32, "HfConstants must match height_gi.hlsl");
const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
}  // namespace

bool HeightGi::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    auto tex = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
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
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, clear, IID_PPV_ARGS(&out)));
    };
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_CLEAR_VALUE cz = {};
    cz.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    D3D12_CLEAR_VALUE cd = {};
    cd.Format = DXGI_FORMAT_D32_FLOAT;
    cd.DepthStencil.Depth = 1.0f;
    if (!tex(albedo_, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, kRead, &cz) ||
        !tex(depth_, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, kRead, &cd) ||
        !tex(height_, DXGI_FORMAT_R32G32_FLOAT, uav, kRead, nullptr) ||
        !tex(rad_, DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kRead, nullptr)) {
        error = "heightfield gi targets";
        return false;
    }
    for (int k = 0; k < 4; ++k)
        if (!tex(sh_[k], DXGI_FORMAT_R16G16B16A16_FLOAT, uav, kRead, nullptr)) { error = "heightfield gi sh"; return false; }
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
            error = "heightfield gi constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap_)))) { error = "heightfield gi rtv"; return false; }
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap_)))) { error = "heightfield gi dsv"; return false; }
        device->CreateRenderTargetView(albedo_.Get(), nullptr, rtv_heap_->GetCPUDescriptorHandleForHeapStart());
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
    auto srv = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = fmt;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(r, &sd, cpu(slot));
    };
    auto uavd = [&](ID3D12Resource* r, DXGI_FORMAT fmt, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = fmt;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(r, nullptr, &ud, cpu(slot));
    };
    for (int k = 0; k < 4; ++k) srv(sh_[k].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvScene + UINT(k));
    srv(height_.Get(), DXGI_FORMAT_R32G32_FLOAT, kSrvScene + 4);   // height, curvature (6 m)
    srv(albedo_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvTop + 0);
    srv(depth_.Get(), DXGI_FORMAT_R32_FLOAT, kSrvTop + 1);
    srv(rad_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kSrvTop + 2);
    uavd(height_.Get(), DXGI_FORMAT_R32G32_FLOAT, kUav + 0);
    uavd(rad_.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav + 1);
    for (int k = 0; k < 4; ++k) uavd(sh_[k].Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, kUav + 2 + UINT(k));

    // Root: the update's constants, the frame's shadow constants (the light,
    // the sky, the canopy), the SRVs it reads, the UAVs it writes.
    {
        D3D12_DESCRIPTOR_RANGE sr[5] = {};
        // The near field's probe visibility volume (t30).
        sr[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr[4].NumDescriptors = 1; sr[4].BaseShaderRegister = 30;
        sr[4].OffsetInDescriptorsFromTableStart = NearField::kSrvVis;
        sr[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr[0].NumDescriptors = 3; sr[0].BaseShaderRegister = 0;
        sr[0].OffsetInDescriptorsFromTableStart = kSrvTop;
        sr[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr[1].NumDescriptors = 5; sr[1].BaseShaderRegister = 38;
        sr[1].OffsetInDescriptorsFromTableStart = kSrvScene;
        sr[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr[2].NumDescriptors = 1; sr[2].BaseShaderRegister = 25;
        sr[2].OffsetInDescriptorsFromTableStart = 38;   // the canopy map
        // The near field's probes and occupancy (near_field.h): the camera's
        // sky share comes through hfgi_sample, which reads them under the ground.
        sr[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; sr[3].NumDescriptors = 3; sr[3].BaseShaderRegister = 26;
        sr[3].OffsetInDescriptorsFromTableStart = 40;
        D3D12_DESCRIPTOR_RANGE ur = {};
        ur.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ur.NumDescriptors = 6; ur.BaseShaderRegister = 0;
        ur.OffsetInDescriptorsFromTableStart = kUav;
        D3D12_ROOT_PARAMETER p[5] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 5; p[2].DescriptorTable.pDescriptorRanges = sr;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1; p[3].DescriptorTable.pDescriptorRanges = &ur;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[4].Descriptor.ShaderRegister = 6;   // the camera's share
        for (int k = 0; k < 5; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[2] = {};
        for (int k = 0; k < 2; ++k) {
            ss[k].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[k].AddressU = ss[k].AddressV = ss[k].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[k].MaxLOD = D3D12_FLOAT32_MAX;
            ss[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        ss[0].ShaderRegister = 0;
        ss[1].ShaderRegister = 3;   // hfgi.hlsli samples through s3 (shadow.hlsli g_gi_linear)
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("heightfield gi root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "heightfield gi CreateRootSignature";
            return false;
        }
    }
    {
        auto buf = [&](ComPtr<ID3D12Resource>& out, D3D12_HEAP_TYPE type, bool uavf, D3D12_RESOURCE_STATES st) -> bool {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = type;
            D3D12_RESOURCE_DESC bd = {};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = 256;
            bd.Height = 1;
            bd.DepthOrArraySize = 1;
            bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bd.Flags = uavf ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
            return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
        };
        // A root UAV: ALLOW_UNORDERED_ACCESS is not optional (CLAUDE.md).
        if (!buf(cam_, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
            !buf(cam_rb_[0], D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
            !buf(cam_rb_[1], D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST)) {
            error = "heightfield gi camera";
            return false;
        }
    }
    const char* entries[3] = {"cs_hf_light", "cs_hf_gather", "cs_hf_camera"};
    ComPtr<ID3D12PipelineState>* outs[3] = {&light_pso_, &gather_pso_, &camera_pso_};
    for (int k = 0; k < 3; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/height_gi.hlsl", entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cdsc = {};
        cdsc.pRootSignature = root_.Get();
        cdsc.CS.pShaderBytecode = cs->GetBufferPointer();
        cdsc.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cdsc, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "heightfield gi pso";
            return false;
        }
    }
    ready_ = true;
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE HeightGi::rtv() const { return rtv_heap_->GetCPUDescriptorHandleForHeapStart(); }
D3D12_CPU_DESCRIPTOR_HANDLE HeightGi::dsv() const { return dsv_heap_->GetCPUDescriptorHandleForHeapStart(); }

bool HeightGi::begin(const float cam[3], float hours, unsigned long long frame, float vp[16]) {
    if (!ready_) return false;
    const float dx = cam[0] - centre_[0], dy = cam[1] - centre_[1], dz = cam[2] - centre_[2];
    // 8 m of the camera's travel, a minute of the clock, or a second.
    const bool due = !built_ || dx * dx + dz * dz > 64.0f || std::fabs(dy) > 8.0f ||
                     std::fabs(hours - hours_) > 1.0f / 60.0f || frame - last_frame_ >= 60 || bounce_reset_ > 0;
    if (!due) return false;
    drop_bounce_ = bounce_reset_ == 4;
    if (bounce_reset_ > 0) --bounce_reset_;
    for (int k = 0; k < 3; ++k) centre_[k] = cam[k];
    hours_ = hours;
    last_frame_ = frame;
    origin_[0] = std::floor((cam[0] - 0.5f * kExtent) / kTexel) * kTexel;
    origin_[1] = std::floor((cam[2] - 0.5f * kExtent) / kTexel) * kTexel;
    y_top_ = cam[1] + 600.0f;
    y_range_ = 1200.0f;
    // Looking straight down: x across, +z up the image, depth the drop from
    // y_top. Row-vector: clip = p * M.
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

void HeightGi::record(ID3D12GraphicsCommandList* cmd, UINT frame, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb) {
    const UINT f = frame & 1u;
    HfConstants hc = {};
    hc.origin_xz[0] = origin_[0];
    hc.origin_xz[1] = origin_[1];
    hc.y_top = y_top_;
    hc.y_range = y_range_;
    hc.first = built_ ? (drop_bounce_ ? 2u : 0u) : 1u;   // 2: the last rebuild's sky, none of its bounce
    std::memcpy(cb_cpu_[f], &hc, sizeof hc);
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    // The renderer drew into these (begin_draw left them so).
    tr(albedo_.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, kRead);
    tr(depth_.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, kRead);
    tr(height_.Get(), kRead, ua);
    tr(rad_.Get(), kRead, ua);

    bind_shader_heaps(cmd);
    const D3D12_GPU_DESCRIPTOR_HANDLE base = heap_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, shadow_cb);
    cmd->SetComputeRootDescriptorTable(2, base);
    cmd->SetComputeRootDescriptorTable(3, base);
    cmd->SetComputeRootUnorderedAccessView(4, cam_->GetGPUVirtualAddress());
    cmd->SetPipelineState(light_pso_.Get());
    cmd->Dispatch(kSize / 8, kSize / 8, 1);
    tr(height_.Get(), ua, kRead);
    tr(rad_.Get(), ua, kRead);
    for (int k = 0; k < 4; ++k) tr(sh_[k].Get(), kRead, ua);
    cmd->SetPipelineState(gather_pso_.Get());
    cmd->Dispatch(kSize / 8, kSize / 8, 1);
    for (int k = 0; k < 4; ++k) tr(sh_[k].Get(), ua, kRead);
    built_ = true;
    ++rebuilds_;
}

void HeightGi::record_camera(ID3D12GraphicsCommandList* cmd, UINT slot, D3D12_GPU_VIRTUAL_ADDRESS shadow_cb) {
    if (!ready_ || !built_) return;
    const UINT rs = slot & 1u;
    if (cam_pending_[rs]) {
        float* v = nullptr;
        D3D12_RANGE rr = {0, 16};
        if (SUCCEEDED(cam_rb_[rs]->Map(0, &rr, reinterpret_cast<void**>(&v)))) {
            camera_sky_ = (v[0] >= 0.0f && v[0] <= 1.0f) ? v[0] : 1.0f;
            camera_open_ = (v[1] >= 0.0f && v[1] <= 1.0f) ? v[1] : 1.0f;
            D3D12_RANGE nr = {0, 0};
            cam_rb_[rs]->Unmap(0, &nr);
        }
        cam_pending_[rs] = false;
    }
    bind_shader_heaps(cmd);
    const D3D12_GPU_DESCRIPTOR_HANDLE base = heap_->GetGPUDescriptorHandleForHeapStart();
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[rs]->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, shadow_cb);
    cmd->SetComputeRootDescriptorTable(2, base);
    cmd->SetComputeRootDescriptorTable(3, base);
    cmd->SetComputeRootUnorderedAccessView(4, cam_->GetGPUVirtualAddress());
    cmd->SetPipelineState(camera_pso_.Get());
    cmd->Dispatch(1, 1, 1);
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = cam_.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cmd->ResourceBarrier(1, &b);
    cmd->CopyBufferRegion(cam_rb_[rs].Get(), 0, cam_.Get(), 0, 16);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cmd->ResourceBarrier(1, &b);
    cam_pending_[rs] = true;
}

void HeightGi::constants(bool on, float out[4]) const {
    out[0] = origin_[0];
    out[1] = origin_[1];
    out[2] = 0.0f;
    out[3] = (on && built_) ? 1.0f : 0.0f;
}

}  // namespace engine
