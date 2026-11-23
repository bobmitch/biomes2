#include "engine/render/ocean_detail.h"
#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstring>

namespace engine {

namespace {
// Matches cbuffer FftConstants in ocean_fft.hlsl.
struct FftConstants {
    float time;  float choppy;  float amp;  float foam_decay;
    float wind_dir[2];  float wind_u;  unsigned seed;
};
static_assert(sizeof(FftConstants) == 32, "FftConstants must match ocean_fft.hlsl");

const char* kFftShader = "engine/render/shaders/ocean_fft.hlsl";
}  // namespace

bool OceanDetail::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    heap_ = heap;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&out)));
    };
    // Three complex fields per cascade. ROOT UAVs: ALLOW_UNORDERED_ACCESS is
    // not optional (CLAUDE.md, the M1 hang).
    const UINT64 field_bytes = UINT64(OD_N) * OD_N * 3 * OD_CASCADES * 8;
    if (!buffer(a_, D3D12_HEAP_TYPE_DEFAULT, field_bytes, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !buffer(b_, D3D12_HEAP_TYPE_DEFAULT, field_bytes, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        error = "ocean detail fields";
        return false;
    }
    for (int f = 0; f < 2; ++f) {
        if (!buffer(cb_[f], D3D12_HEAP_TYPE_UPLOAD, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ)) {
            error = "ocean detail constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = OD_N;
        td.Height = OD_N;
        td.DepthOrArraySize = OD_CASCADES;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&out_)))) {
            error = "ocean detail texture";
            return false;
        }
        out_readable_ = false;
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kSrvSlot) * inc_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = OD_CASCADES;
        device->CreateShaderResourceView(out_.Get(), &sd, h);
        h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kUavSlot) * inc_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.ArraySize = OD_CASCADES;
        device->CreateUnorderedAccessView(out_.Get(), nullptr, &ud, h);
    }
    {
        D3D12_DESCRIPTOR_RANGE ru = {};
        ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ru.NumDescriptors = 1;
        ru.BaseShaderRegister = 2;
        D3D12_ROOT_PARAMETER p[4] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[1].Descriptor.ShaderRegister = 0;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p[2].Descriptor.ShaderRegister = 1;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 1;
        p[3].DescriptorTable.pDescriptorRanges = &ru;
        for (int k = 0; k < 4; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 4;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = "ocean detail root signature";
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "ocean detail CreateRootSignature";
            return false;
        }
    }
    const char* entries[3] = {"cs_spectrum", "cs_rows", "cs_cols"};
    ComPtr<ID3D12PipelineState>* psos[3] = {&spectrum_pso_, &rows_pso_, &cols_pso_};
    for (int k = 0; k < 3; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kFftShader, entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(psos[k]->GetAddressOf())))) {
            error = std::string("ocean detail pso ") + entries[k];
            return false;
        }
    }
    ready_ = true;
    return true;
}

void OceanDetail::update(float time, float dt, float wind_u, const float wind_dir[2], const WwSea& sea) {
    time_ = time;
    dt_ = dt < 0.0f ? 0.0f : (dt > 0.1f ? 0.1f : dt);
    wind_u_ = wind_u > 1.0f ? wind_u : 1.0f;
    wind_dir_[0] = wind_dir[0];
    wind_dir_[1] = wind_dir[1];
    // Foam left by a breaking crest lasts a couple of seconds.
    foam_decay_ = std::exp(-dt_ / 1.8f);

    // The spectrum's slope sum per cascade, sum k^2 (P(k) + P(-k)) over the
    // grid -- the variance the IFFT of h0 = xi sqrt(P / 2) produces with
    // E|xi|^2 = 2 -- re-summed only when the wind has moved by 2% or half a
    // degree, which at the weather's pace is every few seconds.
    const float du = std::fabs(wind_u_ - sum_u_) / wind_u_;
    const float dd = wind_dir_[0] * sum_dir_[0] + wind_dir_[1] * sum_dir_[1];
    if (sum_u_ < 0.0f || du > 0.02f || dd < 0.99996f) {
        // Only where the band is: the 31 m tile's ripples all sit within 64
        // texels of k = 0 (k up to OD_K_SPLIT), every texel summed; the 7.7 m
        // tile's spectrum is smooth out there, so every second texel, x4.
        // ~33k evaluations rather than 131k, well under a millisecond.
        for (int c = 0; c < OD_CASCADES; ++c) {
            double s = 0.0;
            const int stride = c == 0 ? 1 : 2;
            const int reach = c == 0 ? 64 : OD_N / 2;
            for (int my = -reach; my < reach; my += stride) {
                const float kz = 6.2831853f / od_tile[c] * float(my);
                for (int mx = -reach; mx < reach; mx += stride) {
                    const float kx = 6.2831853f / od_tile[c] * float(mx);
                    const double k2 = double(kx) * kx + double(kz) * kz;
                    if (k2 <= 0.0) continue;
                    s += k2 * (double(od_phillips(c, kx, kz, wind_u_, wind_dir_[0], wind_dir_[1])) +
                               double(od_phillips(c, -kx, -kz, wind_u_, wind_dir_[0], wind_dir_[1])));
                }
            }
            sum_[c] = s * double(stride * stride);
        }
        sum_u_ = wind_u_;
        sum_dir_[0] = wind_dir_[0];
        sum_dir_[1] = wind_dir_[1];
    }

    // What the Gerstner trains already carry: a train of amplitude a and
    // wavenumber k has mean square slope (k a)^2 / 2.
    double g = 0.0;
    for (int i = 0; i < WW_WAVES; ++i) {
        const double k = 6.283185307 / ww_wavelength(i);
        g += 0.5 * (k * sea.amp[i]) * (k * sea.amp[i]);
    }
    gvar_ = float(g);
    const float cm = od_cox_munk(wind_u_);
    // At least 30% of Cox-Munk in ripples: a sea whose trains already carry
    // the whole slope still has capillary ripples, which is where glitter is.
    target_ = cm - gvar_ > 0.3f * cm ? cm - gvar_ : 0.3f * cm;
    const double total = sum_[0] + sum_[1];
    const double a2 = total > 0.0 ? double(target_) / total : 0.0;
    amp_ = float(std::sqrt(a2));
    for (int c = 0; c < OD_CASCADES; ++c) var_[c] = float(a2 * sum_[c]);
}

void OceanDetail::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_) return;
    FftConstants fc = {};
    fc.time = time_;
    fc.choppy = choppy_;
    fc.amp = amp_;
    fc.foam_decay = foam_decay_;
    fc.wind_dir[0] = wind_dir_[0];
    fc.wind_dir[1] = wind_dir_[1];
    fc.wind_u = wind_u_;
    fc.seed = 0x5EA5u;
    std::memcpy(cb_cpu_[frame & 1u], &fc, sizeof fc);

    auto uavb = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        cmd->ResourceBarrier(1, &b);
    };
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (out_readable_) tr(out_.Get(), ps, ua);

    D3D12_GPU_DESCRIPTOR_HANDLE t = heap_->GetGPUDescriptorHandleForHeapStart();
    t.ptr += UINT64(kUavSlot) * inc_;
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[frame & 1u]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(1, a_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(2, b_->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(3, t);
    cmd->SetPipelineState(spectrum_pso_.Get());
    cmd->Dispatch(OD_N / 8, OD_N / 8, OD_CASCADES);
    uavb(a_.Get());
    cmd->SetPipelineState(rows_pso_.Get());
    cmd->Dispatch(OD_N, OD_CASCADES, 1);
    uavb(b_.Get());
    cmd->SetPipelineState(cols_pso_.Get());
    cmd->Dispatch(OD_N, OD_CASCADES, 1);
    tr(out_.Get(), ua, ps);
    out_readable_ = true;
}

}  // namespace engine
