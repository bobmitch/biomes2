#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/near_field.h"
#include "engine/render/shader_compile.h"
#include "engine/field/world_gpu.h"
#include "engine/svdag/brick_store.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <utility>

namespace engine {

namespace {
// Matches cbuffer NfConstants in near_field.hlsl.
struct NfConstants {
    int32_t origin[4];
    uint32_t edits, probes, pad0, pad1;
    float rot[3][4];   // rows of this update's random rotation of the ray set
};
static_assert(sizeof(NfConstants) == 80, "NfConstants must match near_field.hlsl");

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

bool NearField::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    held_.assign(size_t(kBlocks) * 3, INT_MIN);
    dirty_.assign(kBlocks, 0);
    probe_dirty_.assign(kProbes, 0);

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
    for (int f = 0; f < 2; ++f) {
        const D3D12_HEAP_TYPE up = D3D12_HEAP_TYPE_UPLOAD;
        const D3D12_RESOURCE_STATES gr = D3D12_RESOURCE_STATE_GENERIC_READ;
        if (!buffer(cb_[f], 256, up, false, gr) || !buffer(blocks_up_[f], UINT64(kBlocks) * 16, up, false, gr) ||
            !buffer(edits_up_[f], UINT64(kMaxEdits) * 16, up, false, gr) ||
            !buffer(probes_up_[f], UINT64(kProbes) * 4, up, false, gr)) {
            error = "near field uploads";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
        blocks_up_[f]->Map(0, nullptr, &blocks_cpu_[f]);
        edits_up_[f]->Map(0, nullptr, &edits_cpu_[f]);
        probes_up_[f]->Map(0, nullptr, &probes_cpu_[f]);
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
        D3D12_DESCRIPTOR_RANGE r[5] = {};
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
        D3D12_ROOT_PARAMETER p[19] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[2].Descriptor.ShaderRegister = 2;
        for (int k = 0; k < 10; ++k) {
            p[3 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[3 + k].Descriptor.ShaderRegister = UINT(11 + k);
        }
        p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[13].DescriptorTable.NumDescriptorRanges = 5;
        p[13].DescriptorTable.pDescriptorRanges = r;
        p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[14].Descriptor.ShaderRegister = 2;
        p[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[15].Descriptor.ShaderRegister = 3;
        p[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[16].Descriptor.ShaderRegister = 4;
        p[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[17].Descriptor.ShaderRegister = 6;
        p[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[18].Constants.ShaderRegister = 3;
        p[18].Constants.Num32BitValues = 1;
        for (int k = 0; k < 19; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[2] = {};
        for (int k = 0; k < 2; ++k) {
            ss[k].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[k].MaxLOD = D3D12_FLOAT32_MAX;
            ss[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        ss[0].AddressU = ss[0].AddressV = ss[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss[0].ShaderRegister = 3;   // g_gi_linear (shadow.hlsli): the heightfield's maps
        ss[1].AddressU = ss[1].AddressV = ss[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        ss[1].ShaderRegister = 6;   // g_nf_wrap: the toroidal occupancy
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 19;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 2;
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
    const char* entries[6] = {"cs_nf_fill", "cs_nf_edits", "cs_nf_occ", "cs_nf_mip", "cs_nf_probe", "cs_nf_top"};
    ComPtr<ID3D12PipelineState>* outs[6] = {&fill_pso_, &edits_pso_, &occ_pso_, &mip_pso_, &probe_pso_, &top_pso_};
    for (int k = 0; k < 6; ++k) {
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
    std::fill(dirty_.begin(), dirty_.end(), uint8_t(1));
}

void NearField::plan(const float cam[3], const BrickStore* edits, bool settle) {
    blocks_.clear();
    edit_list_.clear();
    probe_list_.clear();
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
    for (size_t i = 0; i < budget; ++i) {
        const int s = want[i].second;
        const int sx = s % kBx, sy = (s / kBx) % kBy, sz = s / (kBx * kBy);
        const int w[3] = {ob[0] + wrap(sx - ob[0], kBx), ob[1] + wrap(sy - ob[1], kBy), ob[2] + wrap(sz - ob[2], kBz)};
        int* h = &held_[size_t(s) * 3];
        h[0] = w[0]; h[1] = w[1]; h[2] = w[2];
        dirty_[size_t(s)] = 0;
        blocks_.push_back(w[0]); blocks_.push_back(w[1]); blocks_.push_back(w[2]); blocks_.push_back(0);
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
    pending_ = int(want.size() - budget);
    occ_dirty_ = !blocks_.empty();
    ++plans_;
    if (!blocks_.empty()) { ++fill_frames_; blocks_filled_ += blocks_.size() / 4; }

    // Probes: the dirty ones first, then a round robin over all of them so
    // the light follows the hour and the heightfield's rebuilds.
    const size_t pbudget = settle ? size_t(kProbes) : size_t(kProbesPerFrame);
    size_t take = (std::min)(dirty_probes_.size(), pbudget);
    // The top bit: start the probe over rather than blend into its history,
    // which predates the edit or the block that scrolled in.
    for (size_t i = 0; i < take; ++i) {
        probe_list_.push_back(dirty_probes_[i] | 0x80000000u);
        probe_dirty_[dirty_probes_[i]] = 0;
    }
    dirty_probes_.erase(dirty_probes_.begin(), dirty_probes_.begin() + std::ptrdiff_t(take));
    if (settle) {
        probe_list_.clear();
        for (uint32_t i = 0; i < uint32_t(kProbes); ++i) probe_list_.push_back(i);   // blended: a still runs 16 frames
        std::fill(probe_dirty_.begin(), probe_dirty_.end(), uint8_t(0));
        dirty_probes_.clear();
    } else {
        while (probe_list_.size() < pbudget) {
            probe_list_.push_back(cursor_);
            cursor_ = (cursor_ + 1) % uint32_t(kProbes);
        }
    }

    probes_traced_ += probe_list_.size();
    // Ready once the whole volume has been filled and every probe traced
    // since; a jump that leaves a quarter of it stale is not ready again
    // until it has caught up.
    if (pending_ == 0 && dirty_probes_.empty()) ready_ = true;
    else if (pending_ > kBlocks / 4) ready_ = false;
}

void NearField::record(ID3D12GraphicsCommandList* cmd, UINT frame, const WorldGpu& world,
                       D3D12_GPU_VIRTUAL_ADDRESS shadow_cb) {
    if (!root_ || (blocks_.empty() && probe_list_.empty())) return;
    const UINT f = frame & 1u;
    NfConstants c = {};
    for (int k = 0; k < 3; ++k) c.origin[k] = origin_[k];
    c.edits = uint32_t(edit_list_.size() / 4);
    c.probes = uint32_t(probe_list_.size());
    random_rotation(rng_, c.rot);
    std::memcpy(cb_cpu_[f], &c, sizeof c);
    if (!blocks_.empty()) std::memcpy(blocks_cpu_[f], blocks_.data(), blocks_.size() * 4);
    if (!edit_list_.empty()) std::memcpy(edits_cpu_[f], edit_list_.data(), edit_list_.size() * 4);
    if (!probe_list_.empty()) std::memcpy(probes_cpu_[f], probe_list_.data(), probe_list_.size() * 4);

    ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
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
    }
    if (!occ_is_srv_) { transition(cmd, occ_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); occ_is_srv_ = true; }
    if (!top_is_srv_) { transition(cmd, top_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); top_is_srv_ = true; }
    if (!probe_list_.empty()) {
        if (probes_is_srv_) { transition(cmd, probes_.Get(), kRead, D3D12_RESOURCE_STATE_UNORDERED_ACCESS); probes_is_srv_ = false; }
        cmd->SetPipelineState(probe_pso_.Get());
        cmd->Dispatch(UINT((probe_list_.size() + 63) / 64), 1, 1);
    }
    if (!probes_is_srv_) { transition(cmd, probes_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kRead); probes_is_srv_ = true; }
}

void NearField::constants(bool on, float out[4]) const {
    out[0] = float(origin_[0]);
    out[1] = float(origin_[1]);
    out[2] = float(origin_[2]);
    out[3] = (on && ready_) ? 1.0f : 0.0f;
}

}  // namespace engine
