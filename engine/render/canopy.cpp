#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/render/canopy.h"
#include "engine/render/shader_compile.h"
#include "engine/render/shader_heaps.h"

#include <cmath>
#include <cstring>

namespace engine {

namespace {
// Matches cbuffer CanopyBuild in canopy.hlsl.
struct CanopyBuild {
    float origin_xz[2]; float y_ref; UINT tree_count;
    UINT  groups_x;     float pad0[3];
};
static_assert(sizeof(CanopyBuild) == 32, "CanopyBuild must match canopy.hlsl");
struct CanopyTree { float a[4]; float b[4]; };
const UINT kGroupsX = 1024;
}  // namespace

bool CanopyMap::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
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
    // A ROOT UAV must be created with ALLOW_UNORDERED_ACCESS (CLAUDE.md).
    if (!buffer(accum_, D3D12_HEAP_TYPE_DEFAULT, UINT64(kSize) * kSize * 3 * 4, true,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) { error = "canopy accumulators"; return false; }
    for (int f = 0; f < 2; ++f) {
        if (!buffer(tree_up_[f], D3D12_HEAP_TYPE_UPLOAD, UINT64(kMaxTrees) * sizeof(CanopyTree), false,
                    D3D12_RESOURCE_STATE_GENERIC_READ) ||
            !buffer(cb_[f], D3D12_HEAP_TYPE_UPLOAD, 256, false, D3D12_RESOURCE_STATE_GENERIC_READ)) {
            error = "canopy upload";
            return false;
        }
        tree_up_[f]->Map(0, nullptr, &tree_cpu_[f]);
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kSize;
        td.Height = kSize;
        td.DepthOrArraySize = UINT16(kSlices);
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                   nullptr, IID_PPV_ARGS(&tex_)))) {
            error = "canopy texture";
            return false;
        }
    }
    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kSrvSlot) * inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2DArray.MipLevels = 1;
        sd.Texture2DArray.ArraySize = kSlices;
        device->CreateShaderResourceView(tex_.Get(), &sd, h);
        h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(kUavSlot) * inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.ArraySize = kSlices;
        device->CreateUnorderedAccessView(tex_.Get(), nullptr, &ud, h);
    }
    // Root: the build constants, the slice, the trees, the accumulators, the
    // map as a UAV and as an SRV (the blur reads slice k-1 while writing k).
    {
        D3D12_DESCRIPTOR_RANGE ru = {}, rs = {};
        ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ru.NumDescriptors = 1; ru.BaseShaderRegister = 1;
        rs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; rs.NumDescriptors = 1; rs.BaseShaderRegister = 1;
        D3D12_ROOT_PARAMETER p[6] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[1].Constants.ShaderRegister = 1;
        p[1].Constants.Num32BitValues = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].Descriptor.ShaderRegister = 0;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[3].Descriptor.ShaderRegister = 0;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[4].DescriptorTable.NumDescriptorRanges = 1; p[4].DescriptorTable.pDescriptorRanges = &ru;
        p[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[5].DescriptorTable.NumDescriptorRanges = 1; p[5].DescriptorTable.pDescriptorRanges = &rs;
        for (int k = 0; k < 6; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 6;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("canopy root signature: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "canopy CreateRootSignature";
            return false;
        }
    }
    const char* entries[4] = {"cs_canopy_clear", "cs_canopy_splat", "cs_canopy_resolve", "cs_canopy_blur"};
    ComPtr<ID3D12PipelineState>* outs[4] = {&clear_pso_, &splat_pso_, &resolve_pso_, &blur_pso_};
    for (int k = 0; k < 4; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader("engine/render/shaders/canopy.hlsl", entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(outs[k]->GetAddressOf())))) {
            error = "canopy compute pso";
            return false;
        }
    }
    ready_ = true;
    return true;
}

void CanopyMap::set_models(const std::vector<TreeModel>& models, const std::vector<TreeLook>& looks) {
    crowns_.assign(models.size(), CrownVolume());
    for (size_t m = 0; m < models.size(); ++m) {
        const size_t species = m / kTreeVariants;
        if (species < looks.size()) crowns_[m] = crown_volume(models[m], looks[species]);
    }
}

void CanopyMap::record(ID3D12GraphicsCommandList* cmd, UINT frame, const float cam[3],
                       const std::vector<TreeGpuInstance>& trees, unsigned long long version, float origin[3]) {
    if (!ready_) return;
    // The build was ~0.15 ms of a 2.5 ms walking frame and the per-pixel
    // reads too little to measure, so the map is kept until it is due: a
    // 512 m map built 16 m away still reaches 240 m on every side.
    {
        const float dx = cam[0] - centre_[0], dy = cam[1] - centre_[1], dz = cam[2] - centre_[2];
        if (!first_ && version == version_ &&
            dx * dx + dz * dz < kRebuildDistance * kRebuildDistance && std::fabs(dy) < kRebuildDistance) {
            for (int k = 0; k < 3; ++k) origin[k] = origin_[k];
            return;
        }
    }
    version_ = version;
    for (int k = 0; k < 3; ++k) centre_[k] = cam[k];
    ++builds_;
    const UINT f = frame & 1u;
    // The map's min corner, on whole texels so a texel always covers the
    // same ground and nothing crawls as the camera moves.
    const float half = 0.5f * float(kSize) * kTexel;
    origin[0] = std::floor((cam[0] - half) / kTexel) * kTexel;
    origin[1] = std::floor((cam[2] - half) / kTexel) * kTexel;
    origin[2] = cam[1];
    for (int k = 0; k < 3; ++k) origin_[k] = origin[k];

    // The trees whose crowns reach the map, in world metres.
    CanopyTree* out = static_cast<CanopyTree*>(tree_cpu_[f]);
    UINT n = 0;
    const float x1 = origin[0] + 2.0f * half, z1 = origin[1] + 2.0f * half;
    for (size_t i = 0; i < trees.size() && n < kMaxTrees; ++i) {
        const TreeGpuInstance& t = trees[i];
        if (t.mode != 0 || t.model >= crowns_.size()) continue;
        const CrownVolume& c = crowns_[t.model];
        if (c.rh <= 0.0f) continue;
        const float s = t.scale, rh = c.rh * s;
        if (t.pos[0] + rh < origin[0] || t.pos[0] - rh > x1 || t.pos[2] + rh < origin[1] || t.pos[2] - rh > z1) continue;
        // The crown's centre sits on the trunk axis; a tree's tilt is a few
        // degrees and is ignored here (the RT volumes honour it).
        CanopyTree& o = out[n++];
        o.a[0] = t.pos[0]; o.a[1] = t.pos[1] + c.cy * s; o.a[2] = t.pos[2]; o.a[3] = rh;
        o.b[0] = c.rv * s; o.b[1] = c.sigma; o.b[2] = 0.0f; o.b[3] = 0.0f;
    }
    trees_ = n;
    CanopyBuild cb = {};
    cb.origin_xz[0] = origin[0];
    cb.origin_xz[1] = origin[1];
    cb.y_ref = origin[2];
    cb.tree_count = n;
    cb.groups_x = kGroupsX;
    std::memcpy(cb_cpu_[f], &cb, sizeof cb);

    auto tr = [&](D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b, UINT sub) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = tex_.Get();
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = sub;
        cmd->ResourceBarrier(1, &rb);
    };
    auto uav = [&](ID3D12Resource* r) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        rb.UAV.pResource = r;
        cmd->ResourceBarrier(1, &rb);
    };
    const D3D12_RESOURCE_STATES nps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES read = nps | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    const D3D12_RESOURCE_STATES ua = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    if (!first_) tr(read, ua, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
    first_ = false;

    bind_shader_heaps(cmd);
    const UINT inc = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE gu = heap_->GetGPUDescriptorHandleForHeapStart(), gs = gu;
    gu.ptr += UINT64(kUavSlot) * inc;
    gs.ptr += UINT64(kSrvSlot) * inc;
    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRoot32BitConstant(1, 0, 0);
    cmd->SetComputeRootShaderResourceView(2, tree_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(3, accum_->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(4, gu);
    cmd->SetComputeRootDescriptorTable(5, gs);

    cmd->SetPipelineState(clear_pso_.Get());
    cmd->Dispatch((kSize * kSize * 3 + 63) / 64, 1, 1);
    uav(accum_.Get());
    if (n > 0) {
        cmd->SetPipelineState(splat_pso_.Get());
        cmd->Dispatch((std::min)(n, kGroupsX), (n + kGroupsX - 1) / kGroupsX, 1);
        uav(accum_.Get());
    }
    cmd->SetPipelineState(resolve_pso_.Get());
    cmd->Dispatch(kSize / 8, kSize / 8, 1);
    tr(ua, nps, 0);
    cmd->SetPipelineState(blur_pso_.Get());
    for (UINT k = 1; k < kSlices; ++k) {
        cmd->SetComputeRoot32BitConstant(1, k, 0);
        cmd->Dispatch(kSize / 8, kSize / 8, 1);
        tr(ua, nps, k);
    }
    // Every slice is now NON_PIXEL; add PIXEL for the scene's shaders.
    tr(nps, read, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
}

}  // namespace engine
