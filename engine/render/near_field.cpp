#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/near_field.h"
#include "engine/render/shader_compile.h"
#include "engine/render/gpu_profile.h"
#include "engine/field/world_gpu.h"
#include "engine/svdag/brick_store.h"
#include "engine/render/shader_heaps.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace engine {

namespace {
// Matches cbuffer NfConstants in near_field.hlsl.
struct NfConstants {
    int32_t origin[4];
    uint32_t edits, probes;
    float keep_box, keep_rr;   // a frame's blend at the fine memory: a still's base, a probe traced from the CPU's list
    float rot[3][4];   // rows of this update's random rotation of the ray set
    float cloud[4];    // the clouds' shadow map
    uint32_t lights, coarse;   // local lights a near hit is lit by; entries in the coarse list
    float keep_coarse, keep_light;   // the coarse level's blend; probes a moving light reaches
    uint32_t seed, dbg, gate, settle;    // the rays' jitter; BIOMES_NF_DBG; the sealed-air gate; a still's updates so far (~0: play)
    float live[4];                   // the live pass's blends: fine, by a fast light, coarse; -
    uint32_t live2[4];               // fast lights, plans so far, fine and coarse strides
    float fast[8][4];                // fast lights: x, y, z, reach^2
};
static_assert(sizeof(NfConstants) == 288, "NfConstants must match near_field.hlsl");

// A uniformly random rotation (Shoemake's quaternion from three uniforms),
// as matrix rows. Presentation; sin and cos are fine here.
void random_rotation(uint32_t& state, float rows[3][4]) {
    auto next = [&]() {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / 16777216.0f;
    };
    const float u1 = next(), u2 = next() * 6.2831853f, u3 = next() * 6.2831853f;
    const float a = std::sqrt(1.0f - u1), b = std::sqrt(u1);
    const float x = a * std::sin(u2), y = a * std::cos(u2), z = b * std::sin(u3), w = b * std::cos(u3);
    const float m[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
                           {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
                           {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)}};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) rows[r][c] = m[r][c];
        rows[r][3] = 0.0f;
    }
}

const D3D12_RESOURCE_STATES kRead =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

inline int floor_div(int a, int b) { return a >= 0 ? a / b : -(((-a) + b - 1) / b); }
inline int wrap(int a, int n) { const int m = a % n; return m < 0 ? m + n : m; }

inline uint32_t probe_storage(int px, int py, int pz) {
    return uint32_t((wrap(pz, NearField::kPz) * NearField::kPy + wrap(py, NearField::kPy)) * NearField::kPx +
                    wrap(px, NearField::kPx));
}

void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &br);
}

void uav_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    br.UAV.pResource = r;
    cmd->ResourceBarrier(1, &br);
}
}  // namespace

bool NearField::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT cloud_shadow_srv, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    held_.assign(size_t(kBlocks) * 3, INT_MIN);
    dirty_.assign(kBlocks, 0);
    probe_dirty_.assign(kProbes, 0);
    stamp_.assign(kProbes, 0u);
    held_mask_.assign(size_t(kBlocks) * SkyAccess::kMaskWords, 0u);

    D3D12_HEAP_PROPERTIES dh = {};
    dh.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto vol = [&](ComPtr<ID3D12Resource>& out, UINT16 mips) -> bool {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        td.Width = kNx;
        td.Height = kNy;
        td.DepthOrArraySize = kNz;
        td.MipLevels = mips;
        // R32: typed UAV loads of it are guaranteed (the mips read the level
        // above through a UAV), and it filters.
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td,
                                                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                         IID_PPV_ARGS(&out)));
    };
    if (!vol(density_, 1) || !vol(occ_, UINT16(kMips))) { error = "near field volumes"; return false; }
    {
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kNx;
        td.Height = kNz;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&top_)))) {
            error = "near field top";
            return false;
        }
    }

    auto buffer = [&](ComPtr<ID3D12Resource>& out, UINT64 bytes, D3D12_HEAP_TYPE type, bool uav,
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
        return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr,
                                                         IID_PPV_ARGS(&out)));
    };
    // A root UAV: ALLOW_UNORDERED_ACCESS is not optional (CLAUDE.md).
    if (!buffer(probes_, UINT64(kProbes) * kProbeFloat4s * 16, D3D12_HEAP_TYPE_DEFAULT, true,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        error = "near field probes";
        return false;
    }
    if (!buffer(coarse_, UINT64(kCoarseFloat4s) * 16, D3D12_HEAP_TYPE_DEFAULT, true,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
        error = "near field coarse probes";
        return false;
    }
    if (!buffer(aux_, UINT64(kAuxUints) * 4, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !buffer(args_, 256, D3D12_HEAP_TYPE_DEFAULT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) ||
        !buffer(live_rb_[0], 256, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST) ||
        !buffer(live_rb_[1], 256, D3D12_HEAP_TYPE_READBACK, false, D3D12_RESOURCE_STATE_COPY_DEST)) {
        error = "near field live lists";
        return false;
    }
    {
        D3D12_INDIRECT_ARGUMENT_DESC ad = {};
        ad.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sd = {};
        sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &ad;
        if (FAILED(device->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&dispatch_sig_)))) {
            error = "near field dispatch signature";
            return false;
        }
    }
    for (int f = 0; f < 2; ++f) {
        const D3D12_HEAP_TYPE up = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_STATES gr = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (!buffer(cb_[f], 512, up, false, gr) || !buffer(blocks_up_[f], UINT64(kBlocks) * 16, up, false, gr) ||
            !buffer(edits_up_[f], UINT64(kMaxEdits) * 16, up, false, gr) ||
            !buffer(probes_up_[f], UINT64(kProbes) * 4, up, false, gr) ||
            !buffer(coarse_up_[f], UINT64(kCoarse) * 4, up, false, gr) ||
            !buffer(marks_up_[f], UINT64(kBlocks) * 20 * 4, up, false, gr)) {
            error = "near field uploads";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
        blocks_up_[f]->Map(0, nullptr, &blocks_cpu_[f]);
        edits_up_[f]->Map(0, nullptr, &edits_cpu_[f]);
        probes_up_[f]->Map(0, nullptr, &probes_cpu_[f]);
        coarse_up_[f]->Map(0, nullptr, &coarse_cpu_[f]);
        marks_up_[f]->Map(0, nullptr, &marks_cpu_[f]);
    }

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = [&](UINT slot) {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(slot) * inc;
        return h;
    };
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(kProbes) * kProbeFloat4s;
        sd.Buffer.StructureByteStride = 16;
        device->CreateShaderResourceView(probes_.Get(), &sd, cpu(kSrvProbes));
    }
    auto srv3d = [&](ID3D12Resource* r, UINT mips, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture3D.MipLevels = mips;
        device->CreateShaderResourceView(r, &sd, cpu(slot));
    };
    srv3d(occ_.Get(), kMips, kSrvOcc);
    srv3d(density_.Get(), 1, kSrvDensity);
    auto uav3d = [&](ID3D12Resource* r, UINT mip, UINT slot) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        ud.Texture3D.MipSlice = mip;
        ud.Texture3D.FirstWSlice = 0;
        ud.Texture3D.WSize = UINT(kNz >> mip);
        device->CreateUnorderedAccessView(r, nullptr, &ud, cpu(slot));
    };
    uav3d(density_.Get(), 0, kUavDensity);
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(top_.Get(), &sd, cpu(kSrvTop));
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(top_.Get(), nullptr, &ud, cpu(kUavTop));
    }
    for (int m = 0; m < kMips; ++m) uav3d(occ_.Get(), UINT(m), kUavOcc + UINT(m));

    // Root: the update's constants, the world (the field's), the frame's
    // shadow constants (the heightfield's corner), the world's ten buffers,
    // one table (the density and occupancy UAVs, the occupancy and probes'
    // SRVs, the heightfield's maps), the three lists, the probes, the mip.
    {
        D3D12_DESCRIPTOR_RANGE r[8] = {};
        // A ray's hit is lit by the sun's cascades (t8 at [0]), the local
        // lights and their atlas (t20..t24 at [24..28]) and the clouds'
        // shadow (t60).
        r[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[5].NumDescriptors = 1; r[5].BaseShaderRegister = 8;
        r[5].OffsetInDescriptorsFromTableStart = 0;
        r[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[6].NumDescriptors = 5; r[6].BaseShaderRegister = 20;
        r[6].OffsetInDescriptorsFromTableStart = 24;
        r[7].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[7].NumDescriptors = 1; r[7].BaseShaderRegister = 60;
        r[7].OffsetInDescriptorsFromTableStart = cloud_shadow_srv;
        r[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[4].NumDescriptors = 1; r[4].BaseShaderRegister = 7;
        r[4].OffsetInDescriptorsFromTableStart = kUavTop;
        r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[0].NumDescriptors = 1; r[0].BaseShaderRegister = 0;
        r[0].OffsetInDescriptorsFromTableStart = kUavDensity;
        r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; r[1].NumDescriptors = kMips; r[1].BaseShaderRegister = 1;
        r[1].OffsetInDescriptorsFromTableStart = kUavOcc;
        r[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[2].NumDescriptors = 3; r[2].BaseShaderRegister = 26;
        r[2].OffsetInDescriptorsFromTableStart = kSrvProbes;
        r[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r[3].NumDescriptors = 5; r[3].BaseShaderRegister = 38;
        r[3].OffsetInDescriptorsFromTableStart = 48;   // the heightfield GI (height_gi.h kSrvScene)
        D3D12_ROOT_PARAMETER p[24] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[2].Descriptor.ShaderRegister = 2;
        for (int k = 0; k < 10; ++k) {
            p[3 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[3 + k].Descriptor.ShaderRegister = UINT(11 + k);
        }
        p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[13].DescriptorTable.NumDescriptorRanges = 8;
        p[13].DescriptorTable.pDescriptorRanges = r;
        p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[14].Descriptor.ShaderRegister = 2;
        p[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[15].Descriptor.ShaderRegister = 3;
        p[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[16].Descriptor.ShaderRegister = 4;
        p[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[17].Descriptor.ShaderRegister = 6;
        p[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[18].Constants.ShaderRegister = 3;
        p[18].Constants.Num32BitValues = 1;
        // The coarse level (u8) and its list (t5).
        p[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[19].Descriptor.ShaderRegister = 8;
        p[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[20].Descriptor.ShaderRegister = 5;
        // The live lists (u9) and the live passes' dispatch arguments (u10).
        p[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[21].Descriptor.ShaderRegister = 9;
        p[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[22].Descriptor.ShaderRegister = 10;
        // The sky-access marks (t6, sky_access.h).
        p[23].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[23].Descriptor.ShaderRegister = 6;
        for (int k = 0; k < 24; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[3] = {};
        for (int k = 0; k < 2; ++k) {
            ss[k].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[k].MaxLOD = D3D12_FLOAT32_MAX;
            ss[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[0].ShaderRegister = 3;   // g_gi_linear (shadow.hlsli): the heightfield's maps
        ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        ss[1].ShaderRegister = 6;   // g_nf_wrap: the toroidal occupancy
        // s1: g_shadow_cmp (shadow.hlsli), the cascades and the light atlas.
        ss[2].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        ss[2].AddressU = ss[2].AddressV = ss[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        ss[2].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        ss[2].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        ss[2].MaxLOD = D3D12_FLOAT32_MAX;
        ss[2].ShaderRegister = 1;
        ss[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 24;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 3;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("near field root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "near field CreateRootSignature";
            return false;
        }
    }
    const char* entries[14] = {"cs_nf_fill", "cs_nf_edits", "cs_nf_occ", "cs_nf_mip", "cs_nf_probe", "cs_nf_top",
                               "cs_nf_coarse", "cs_nf_probe_live", "cs_nf_coarse_live", "cs_nf_classify",
                               "cs_nf_live", "cs_nf_live_args", "cs_nf_clear", "cs_nf_mark"};
    ComPtr<ID3D12PipelineState>* outs[14] = {&fill_pso_, &edits_pso_, &occ_pso_, &mip_pso_, &probe_pso_, &top_pso_,
                                             &coarse_pso_, &probe_live_pso_, &coarse_live_pso_, &classify_pso_,
                                             &live_pso_, &live_args_pso_, &clear_pso_, &mark_pso_};
    for (int k = 0; k < 14; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/near_field.hlsl", entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = std::string("near field pso ") + entries[k];
            return false;
        }
    }
    return true;
}

void NearField::invalidate_box(const float lo[3], const float hi[3]) {
    if (held_.empty()) return;
    sky_.invalidate_box(lo, hi);
    // The blocks holding the box, by a voxel of margin (the 1 m sample on a
    // block's face belongs to one block only).
    const int b0[3] = {floor_div(int(std::floor(lo[0])) - 1, kBlock), floor_div(int(std::floor(lo[1])) - 1, kBlock),
                       floor_div(int(std::floor(lo[2])) - 1, kBlock)};
    const int b1[3] = {floor_div(int(std::floor(hi[0])) + 1, kBlock), floor_div(int(std::floor(hi[1])) + 1, kBlock),
                       floor_div(int(std::floor(hi[2])) + 1, kBlock)};
    for (int z = b0[2]; z <= b1[2]; ++z)
        for (int y = b0[1]; y <= b1[1]; ++y)
            for (int x = b0[0]; x <= b1[0]; ++x) {
                const int s = (wrap(z, kBz) * kBy + wrap(y, kBy)) * kBx + wrap(x, kBx);
                if (held_[size_t(s) * 3] == x && held_[size_t(s) * 3 + 1] == y && held_[size_t(s) * 3 + 2] == z)
                    dirty_[size_t(s)] = 1;
            }
    // Every probe a cone from could cross the change: a cone's reach in the
    // dark is short, so 12 m round the box.
    const float reach = 12.0f;
    const int p0[3] = {int(std::floor((lo[0] - reach - 1.0f) * 0.5f)), int(std::floor((lo[1] - reach - 1.0f) * 0.5f)),
                       int(std::floor((lo[2] - reach - 1.0f) * 0.5f))};
    const int p1[3] = {int(std::floor((hi[0] + reach - 1.0f) * 0.5f)), int(std::floor((hi[1] + reach - 1.0f) * 0.5f)),
                       int(std::floor((hi[2] + reach - 1.0f) * 0.5f))};
    const int op[3] = {origin_[0] / 2, origin_[1] / 2, origin_[2] / 2};
    for (int z = (std::max)(p0[2], op[2]); z <= (std::min)(p1[2], op[2] + kPz - 1); ++z)
        for (int y = (std::max)(p0[1], op[1]); y <= (std::min)(p1[1], op[1] + kPy - 1); ++y)
            for (int x = (std::max)(p0[0], op[0]); x <= (std::min)(p1[0], op[0] + kPx - 1); ++x) {
                const uint32_t i = probe_storage(x, y, z);
                if (!probe_dirty_[i]) { probe_dirty_[i] = 1; dirty_probes_.push_back(i); }
            }
}

void NearField::invalidate_all() {
    sky_.invalidate_all();
    std::fill(dirty_.begin(), dirty_.end(), uint8_t(1));
}

void NearField::plan(const float cam[3], const BrickStore* edits, bool settle, float dt) {
    // The frame's seconds: given (a simulated clock), else the wall clock's.
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const unsigned long long t = static_cast<unsigned long long>(now.QuadPart);
        if (dt < 0.0f) dt = last_tick_ ? float(double(t - last_tick_) / double(freq.QuadPart)) : 1.0f / 60.0f;
        last_tick_ = t;
        dt = dt < 1e-4f ? 1e-4f : (dt > 0.25f ? 0.25f : dt);
        dt_smooth_ += (dt - dt_smooth_) * 0.05f;
    }
    blocks_.clear();
    edit_list_.clear();
    probe_list_.clear();
    marks_.clear();
    if (held_.empty()) return;
    for (int k = 0; k < 3; ++k) {
        const int half = (k == 1 ? kNy : kNx) / 2;
        origin_[k] = floor_div(int(std::floor(cam[k])), kBlock) * kBlock - half;
    }
    have_origin_ = true;
    const int ob[3] = {origin_[0] / kBlock, origin_[1] / kBlock, origin_[2] / kBlock};
    const float cb[3] = {cam[0] / kBlock, cam[1] / kBlock, cam[2] / kBlock};

    // Every storage block that does not hold the world block it should, or
    // that an edit dirtied: dug ones first, then nearest first.
    std::vector<std::pair<float, int> > want;
    for (int sz = 0; sz < kBz; ++sz)
        for (int sy = 0; sy < kBy; ++sy)
            for (int sx = 0; sx < kBx; ++sx) {
                const int s = (sz * kBy + sy) * kBx + sx;
                const int w[3] = {ob[0] + wrap(sx - ob[0], kBx), ob[1] + wrap(sy - ob[1], kBy),
                                  ob[2] + wrap(sz - ob[2], kBz)};
                const int* h = &held_[size_t(s) * 3];
                if (h[0] == w[0] && h[1] == w[1] && h[2] == w[2] && !dirty_[size_t(s)]) continue;
                const float dx = float(w[0]) + 0.5f - cb[0], dy = float(w[1]) + 0.5f - cb[1],
                            dz = float(w[2]) + 0.5f - cb[2];
                want.push_back(std::make_pair(dirty_[size_t(s)] ? -1.0f : dx * dx + dy * dy + dz * dz, s));
            }
    std::sort(want.begin(), want.end());
    const size_t budget = settle ? want.size() : (std::min)(want.size(), size_t(kBlocksPerFrame));
    std::vector<BrickKey> touched;
    // Sky access: the mask of sealed air for a block, listed for cs_nf_mark
    // when it differs from what the GPU holds (a filled block holds none).
    const int kW = SkyAccess::kMaskWords;
    auto mark_block = [&](int s, const int w[3], bool filled) {
        uint32_t m[SkyAccess::kMaskWords];
        sky_.block_mask(w[0], w[1], w[2], edits, m);
        uint32_t* held = &held_mask_[size_t(s) * kW];
        if (filled) std::fill(held, held + kW, 0u);
        if (std::equal(m, m + kW, held)) return;
        std::copy(m, m + kW, held);
        marks_.push_back(uint32_t(w[0])); marks_.push_back(uint32_t(w[1])); marks_.push_back(uint32_t(w[2]));
        marks_.push_back(0u);
        marks_.insert(marks_.end(), m, m + kW);
    };
    std::vector<uint8_t> filled(size_t(kBlocks), 0);
    for (size_t i = 0; i < budget; ++i) {
        const int s = want[i].second;
        const int sx = s % kBx, sy = (s / kBx) % kBy, sz = s / (kBx * kBy);
        const int w[3] = {ob[0] + wrap(sx - ob[0], kBx), ob[1] + wrap(sy - ob[1], kBy), ob[2] + wrap(sz - ob[2], kBz)};
        int* h = &held_[size_t(s) * 3];
        h[0] = w[0]; h[1] = w[1]; h[2] = w[2];
        dirty_[size_t(s)] = 0;
        blocks_.push_back(w[0]); blocks_.push_back(w[1]); blocks_.push_back(w[2]); blocks_.push_back(0);
        mark_block(s, w, true);
        filled[size_t(s)] = 1;
        // Its own probes are traced again this frame or soon.
        for (int pz = 0; pz < kBlock / 2; ++pz)
            for (int py = 0; py < kBlock / 2; ++py)
                for (int px = 0; px < kBlock / 2; ++px) {
                    const uint32_t pi = probe_storage(w[0] * 4 + px, w[1] * 4 + py, w[2] * 4 + pz);
                    if (!probe_dirty_[pi]) { probe_dirty_[pi] = 1; dirty_probes_.push_back(pi); }
                }
        // The player's edits over it: the 1 m points that hold one, at their
        // absolute value (the edit store's lattice is 0.5 m; point 2w is w).
        if (!edits || edits->brick_count() == 0) continue;
        const float lo[3] = {float(w[0] * kBlock) - 0.5f, float(w[1] * kBlock) - 0.5f, float(w[2] * kBlock) - 0.5f};
        const float hi[3] = {lo[0] + float(kBlock), lo[1] + float(kBlock), lo[2] + float(kBlock)};
        edits->bricks_in_box(lo, hi, touched);
        if (touched.empty()) continue;
        for (int z = 0; z < kBlock; ++z)
            for (int y = 0; y < kBlock; ++y)
                for (int x = 0; x < kBlock; ++x) {
                    const int vx = w[0] * kBlock + x, vy = w[1] * kBlock + y, vz = w[2] * kBlock + z;
                    float d = 0.0f;
                    if (!edits->edited_at(2 * vx, 2 * vy, 2 * vz, d)) continue;
                    if (edit_list_.size() / 4 >= size_t(kMaxEdits)) continue;
                    const uint32_t si = uint32_t((wrap(vz, kNz) * kNy + wrap(vy, kNy)) * kNx + wrap(vx, kNx));
                    float bits;
                    std::memcpy(&bits, &si, 4);
                    edit_list_.push_back(bits);
                    edit_list_.push_back(d);
                    edit_list_.push_back(0.0f);
                    edit_list_.push_back(0.0f);
                }
    }
    // The flood forgot its labels (an edit): every block held is marked
    // again -- after the fills, which marked their own, and never one of
    // them: two entries for one block in the mark pass would race.
    if (sky_.generation() != sky_gen_) {
        sky_gen_ = sky_.generation();
        for (int s = 0; s < kBlocks; ++s) {
            const int* h = &held_[size_t(s) * 3];
            if (h[0] == INT_MIN || filled[size_t(s)]) continue;
            mark_block(s, h, false);
        }
    }
    cam_sealed_ = sky_.sealed_at(cam[0], cam[1], cam[2], edits);
    pending_ = int(want.size() - budget);
    occ_dirty_ = !blocks_.empty();
    ++plans_;
    if (!blocks_.empty()) { ++fill_frames_; blocks_filled_ += blocks_.size() / 4; }

    // The light jumped: start over -- every probe this frame, cleared first,
    // iterated as a still is.
    reset_ = !settle && is_reset(relight_c_);
    if (reset_) {
        settle = true;
        settle_frames_ = 0;
        ++resets_;
    }

    // Probes: the dirty ones first (fresh), then a round robin over all of
    // them -- which in play only places and copies: a probe that traces is
    // the live pass's, every frame (near_field.hlsl cs_nf_live). The round
    // robin finds probes newly under cover (dug ones come dirty) and keeps
    // the copies of the heightfield current. A still lists every probe.
    const size_t pbudget = settle ? size_t(kProbes) : size_t(kProbesPerFrame);
    size_t take = (std::min)(dirty_probes_.size(), pbudget);
    // The top bit: start the probe over rather than blend into its history,
    // which predates the edit or the block that scrolled in.
    const uint32_t stamp = uint32_t(plans_) + 1u;   // this plan's mark: a probe listed once
    for (size_t i = 0; i < take; ++i) {
        probe_list_.push_back(dirty_probes_[i] | 0x80000000u);
        probe_dirty_[dirty_probes_[i]] = 0;
        stamp_[dirty_probes_[i]] = stamp;
    }
    dirty_probes_.erase(dirty_probes_.begin(), dirty_probes_.begin() + std::ptrdiff_t(take));
    // A still blends as play does at 60 Hz (the shader takes the plain mean
    // of its updates until that is the smaller); play, a frame's dt.
    const float step = settle ? 1.0f / 60.0f : dt_smooth_;
    keep_box_ = std::exp(-step / (std::max)(sched_.near_memory, 0.01f));
    keep_light_ = std::exp(-step / (std::max)(sched_.light_memory, 0.01f));
    keep_rr_ = keep_box_;
    keep_live_ = keep_box_;
    keep_live_light_ = keep_light_;
    keep_live_coarse_ = std::exp(-step / (std::max)(sched_.coarse_memory, 0.01f));
    if (settle) {
        probe_list_.clear();
        for (uint32_t i = 0; i < uint32_t(kProbes); ++i) probe_list_.push_back(i);
        std::fill(probe_dirty_.begin(), probe_dirty_.end(), uint8_t(0));
        dirty_probes_.clear();
    } else {
        for (size_t n = 0; n < pbudget; ++n) {
            if (stamp_[cursor_] != stamp) probe_list_.push_back(cursor_);   // not if listed already
            cursor_ = (cursor_ + 1) % uint32_t(kProbes);
        }
    }
    // The coarse level: a round robin too (every probe in a still), which in
    // play places the unplaced and refreshes the open; the covered ones are
    // the live pass's.
    coarse_list_.clear();
    keep_coarse_ = keep_live_coarse_;
    if (settle) {
        for (uint32_t i = 0; i < uint32_t(kCoarse); ++i) coarse_list_.push_back(i);
        // A still is ~17 frames, and a second's memory at 60 Hz (0.983 a
        // frame) kept three quarters of the FIRST frames -- traced while the
        // fine probes its hits bounce off were still empty -- and the
        // stairwell's side tunnel came out 10x too dark. The shader takes the
        // plain mean of the still's updates so far (NfConstants settle).
        ++settle_frames_;
    } else {
        settle_frames_ = 0;
        for (int i = 0; i < kCoarsePerFrame; ++i) {
            coarse_list_.push_back(coarse_cursor_);
            coarse_cursor_ = (coarse_cursor_ + 1) % uint32_t(kCoarse);
        }
    }
    settling_ = settle;
    coarse_traced_ += coarse_list_.size();

    probes_traced_ += probe_list_.size();
    // Ready once the whole volume has been filled and every probe traced
    // since; a jump that leaves a quarter of it stale is not ready again
    // until it has caught up.
    if (pending_ == 0 && dirty_probes_.empty()) ready_ = true;
    else if (pending_ > kBlocks / 4) ready_ = false;
}

void NearField::record(ID3D12GraphicsCommandList* cmd, UINT frame, const WorldGpu& world,
                       D3D12_GPU_VIRTUAL_ADDRESS shadow_cb, const float cloud[4], GpuProfiler* prof) {
    auto mark = [&](const char* name) { if (prof) prof->mark(cmd, name); };
    if (!root_ || (blocks_.empty() && probe_list_.empty() && coarse_list_.empty() && marks_.empty())) return;
    const UINT f = frame & 1u;
    // The live lists' lengths this slot copied two frames ago (its fence has
    // been waited for before the frame's allocator came back).
    if (live_rb_pending_[f]) {
        void* p = nullptr;
        D3D12_RANGE rr = {0, 16};
        if (SUCCEEDED(live_rb_[f]->Map(0, &rr, &p))) {
            const uint32_t* u = static_cast<const uint32_t*>(p);
            live_fine_ = u[0] + u[2];
            live_coarse_ = u[1] + u[3];
            D3D12_RANGE nr = {0, 0};
            live_rb_[f]->Unmap(0, &nr);
        }
        live_rb_pending_[f] = false;
    }
    NfConstants c = {};
    for (int k = 0; k < 3; ++k) c.origin[k] = origin_[k];
    c.edits = uint32_t(edit_list_.size() / 4);
    c.probes = uint32_t(probe_list_.size());
    c.keep_box = keep_box_;
    c.keep_rr = keep_rr_;
    random_rotation(rng_, c.rot);   // unused by the shader now; advances rng_, the jitters' seed
    for (int k = 0; k < 4; ++k) c.cloud[k] = cloud ? cloud[k] : 0.0f;
    // A/B: BIOMES_NF_NOLIGHTS (no local light at a hit), BIOMES_NF_NOCOARSE
    // (no coarse level: every fine ray marches to 48 m, as before it).
    // BIOMES_NF_DBG: 1 a fine ray that took the coarse level returns red,
    // one that found none green; 2 per ray (coarse probes placed, with the
    // bin open, visible) as rgb; 3 never merge (the coarse pass still runs:
    // what the merge costs). View with --texture 0 --tonemap 0 --ev100 1.
    static const bool no_lights = std::getenv("BIOMES_NF_NOLIGHTS") != nullptr;
    static const bool no_coarse = std::getenv("BIOMES_NF_NOCOARSE") != nullptr;
    if (no_coarse) coarse_list_.clear();
    c.lights = no_lights ? 0u : uint32_t(kHitLights);
    c.coarse = uint32_t(coarse_list_.size());
    c.keep_coarse = keep_coarse_;
    c.keep_light = keep_light_;
    c.seed = rng_;
    c.settle = settling_ ? (settle_frames_ - 1u) * uint32_t(kSettleIters) : 0xFFFFFFFFu;
    c.live[0] = keep_live_;
    c.live[1] = keep_live_light_;
    c.live[2] = keep_live_coarse_;
    c.live[3] = 0.0f;
    {
        const size_t nfast = (std::min)(fast_.size() / 4, size_t(8));
        c.live2[0] = uint32_t(nfast);
        for (size_t i = 0; i < nfast; ++i) {
            const float reach = (std::min)(fast_[i * 4 + 3], sched_.light_reach);
            for (int k = 0; k < 3; ++k) c.fast[i][k] = fast_[i * 4 + k];
            c.fast[i][3] = reach * reach;
        }
    }
    c.live2[1] = uint32_t(plans_);
    c.live2[2] = uint32_t((std::max)(sched_.live_stride, 1));
    c.live2[3] = uint32_t((std::max)(sched_.coarse_stride, 1));
    static const char* dbg_env = std::getenv("BIOMES_NF_DBG");
    c.dbg = dbg_env ? uint32_t(std::atoi(dbg_env)) : 0u;
    // BIOMES_NF_NOGATE: probes in sealed air trace even with no light near
    // (near_field.hlsl nf_gated), for A/B.
    static const bool no_gate = std::getenv("BIOMES_NF_NOGATE") != nullptr;
    c.gate = no_gate ? 0u : 1u;
    std::memcpy(cb_cpu_[f], &c, sizeof c);
    if (!blocks_.empty()) std::memcpy(blocks_cpu_[f], blocks_.data(), blocks_.size() * 4);
    if (!edit_list_.empty()) std::memcpy(edits_cpu_[f], edit_list_.data(), edit_list_.size() * 4);
    if (!probe_list_.empty()) std::memcpy(probes_cpu_[f], probe_list_.data(), probe_list_.size() * 4);
    if (!coarse_list_.empty()) std::memcpy(coarse_cpu_[f], coarse_list_.data(), coarse_list_.size() * 4);
    if (!marks_.empty()) std::memcpy(marks_cpu_[f], marks_.data(), marks_.size() * 4);

    bind_shader_heaps(cmd);
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(1, world.world_cb()->GetGPUVirtualAddress());
    cmd->SetComputeRootConstantBufferView(2, shadow_cb);
    ID3D12Resource* wb[10] = {world.cells(), world.cell_offsets(), world.cell_items(), world.archetypes(),
                              world.rivers(), world.river_offsets(), world.river_items(), world.entrances(),
                              world.entrance_offsets(), world.entrance_items()};
    for (int k = 0; k < 10; ++k) cmd->SetComputeRootUnorderedAccessView(3 + UINT(k), wb[k]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(13, heap_->GetGPUDescriptorHandleForHeapStart());
    cmd->SetComputeRootShaderResourceView(14, blocks_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(15, edits_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(16, probes_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(17, probes_->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstant(18, 0u, 0);
    cmd->SetComputeRootUnorderedAccessView(19, coarse_->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(20, coarse_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(21, aux_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(22, args_->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(23, marks_up_[f]->GetGPUVirtualAddress());
    // Sky access's marks: after the fill and the edits (which write plain
    // density), before anything reads it.
    auto apply_marks = [&]() {
        if (marks_.empty()) return;
        cmd->SetPipelineState(mark_pso_.Get());
        cmd->Dispatch(UINT(marks_.size() / 20), 1, 1);
        uav_barrier(cmd, density_.Get());
    };

    if (!blocks_.empty()) {
        if (occ_is_srv_) { transition(cmd, occ_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); occ_is_srv_ = false; }
        cmd->SetPipelineState(fill_pso_.Get());
        cmd->Dispatch(UINT(blocks_.size() / 4), 1, 1);
        uav_barrier(cmd, density_.Get());
        if (!edit_list_.empty()) {
            cmd->SetPipelineState(edits_pso_.Get());
            cmd->Dispatch(UINT((edit_list_.size() / 4 + 63) / 64), 1, 1);
            uav_barrier(cmd, density_.Get());
        }
        apply_marks();
        if (top_is_srv_) { transition(cmd, top_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); top_is_srv_ = false; }
        cmd->SetPipelineState(top_pso_.Get());
        cmd->Dispatch(UINT(kNx / 8), UINT(kNz / 8), 1);
        cmd->SetPipelineState(occ_pso_.Get());
        cmd->Dispatch(UINT(kNx / 8), UINT(kNy / 8), UINT(kNz / 8));
        uav_barrier(cmd, occ_.Get());
        cmd->SetPipelineState(mip_pso_.Get());
        for (int m = 1; m < kMips; ++m) {
            cmd->SetComputeRoot32BitConstant(18, UINT(m), 0);
            cmd->Dispatch(UINT(((kNx >> m) + 3) / 4), UINT(((kNy >> m) + 3) / 4), UINT(((kNz >> m) + 3) / 4));
            uav_barrier(cmd, occ_.Get());
        }
    } else {
        apply_marks();   // re-marks alone change neither occupancy nor the top
    }
    if (!occ_is_srv_) { transition(cmd, occ_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); occ_is_srv_ = true; }
    if (!top_is_srv_) { transition(cmd, top_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); top_is_srv_ = true; }
    if (probes_is_srv_) { transition(cmd, probes_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); probes_is_srv_ = false; }
    mark("near field: fill, occupancy");
    if (reset_) {
        cmd->SetPipelineState(clear_pso_.Get());
        cmd->Dispatch(UINT((kProbes * kProbeFloat4s + 255) / 256), 1, 1);
        uav_barrier(cmd, probes_.Get());
    }
    // A still iterates both levels (kSettleIters); the root constant b3 is
    // the iteration (the mips left it at their last level).
    const int iters = settling_ ? kSettleIters : 1;
    for (int it = 0; it < iters; ++it) {
        cmd->SetComputeRoot32BitConstant(18, UINT(it), 0);
        if (!coarse_list_.empty()) {
            // The coarse level first: the fine rays read it. Its hits read
            // the fine probes (last frame's) for their bounce.
            if (probes_is_srv_) { transition(cmd, probes_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); probes_is_srv_ = false; }
            cmd->SetPipelineState(coarse_pso_.Get());
            cmd->Dispatch(UINT(coarse_list_.size()), 1, 1);
            uav_barrier(cmd, coarse_.Get());
        }
        mark("near field: coarse, listed");
        if (!probe_list_.empty() && settling_) {
            if (probes_is_srv_) { transition(cmd, probes_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); probes_is_srv_ = false; }
            cmd->SetPipelineState(probe_pso_.Get());
            // A group a probe (a thread a ray), in rows of 256 groups: a
            // still's list is longer than one dimension's 65,535.
            cmd->Dispatch(256u, UINT((probe_list_.size() + 255) / 256), 1);
            uav_barrier(cmd, probes_.Get());
        }
        if (settling_) continue;
        // Play: the CPU's list is placed and copied a thread a probe; then
        // every probe that traces (fine and coarse) is listed and traced in
        // its turn, by an indirect dispatch.
        if (!probe_list_.empty()) {
            cmd->SetPipelineState(classify_pso_.Get());
            cmd->Dispatch(UINT((probe_list_.size() + 63) / 64), 1, 1);
            uav_barrier(cmd, aux_.Get());
        }
        mark("near field: classify");
        cmd->SetPipelineState(live_pso_.Get());
        cmd->Dispatch(UINT((kProbes + kCoarse + 255) / 256), 1, 1);
        uav_barrier(cmd, aux_.Get());
        cmd->SetPipelineState(live_args_pso_.Get());
        cmd->Dispatch(1, 1, 1);
        transition(cmd, args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
        mark("near field: live lists");
        // The coarse and fine live passes run side by side, no barrier
        // between: the coarse one is a handful of groups whose 48 m rays are
        // all latency (0.04 ms alone), and the fine rays' merge takes a bin
        // from this frame or the last -- a benign race, the two a frame apart.
        cmd->SetPipelineState(coarse_live_pso_.Get());
        cmd->ExecuteIndirect(dispatch_sig_.Get(), 1, args_.Get(), 12, nullptr, 0);
        cmd->SetPipelineState(probe_live_pso_.Get());
        cmd->ExecuteIndirect(dispatch_sig_.Get(), 1, args_.Get(), 0, nullptr, 0);
        uav_barrier(cmd, probes_.Get());
        uav_barrier(cmd, coarse_.Get());
        transition(cmd, args_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // The counts, for the stats, now and then (both slots: the other
        // was zeroed for next frame).
        if ((plans_ & 31u) == 0u) {
            transition(cmd, aux_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(live_rb_[f].Get(), 0, aux_.Get(), UINT64(2 * kProbes + kCoarse) * 4, 16);
            transition(cmd, aux_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            live_rb_pending_[f] = true;
        }
    }
    if (!probes_is_srv_) { transition(cmd, probes_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); probes_is_srv_ = true; }
}

void NearField::constants(bool on, float out[4]) const {
    out[0] = float(origin_[0]);
    out[1] = float(origin_[1]);
    out[2] = float(origin_[2]);
    // 2: on, and the camera stands in sealed air (sky_access.h sealed_at).
    out[3] = (on && ready_) ? (cam_sealed_ ? 2.0f : 1.0f) : 0.0f;
}

}  // namespace engine
