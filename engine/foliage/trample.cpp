#include "engine/foliage/trample.h"

#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstring>

namespace engine {
namespace {

const char* kShader = "engine/render/shaders/trample.hlsl";

// How fast each pair forgets. Fast: a bush shoved aside is back in about a
// third of a second. Slow: grass is still laid over tens of seconds later,
// but well inside the ~20 s it takes a jog to carry the map's edge past a
// trail -- anything that outlived the window would pop.
const float kFastTau = 0.12f;
const float kSlowTau = 18.0f;
// The fade runs in batches: a pass over 4.2 M texels is 34 MB read and
// written, which is more than the whole of the ground cover's frame if it
// runs every one. Every eighth, with the seconds owed, costs an eighth.
const UINT kDecayStride = 8;

const UINT kMaxStamps = 64;
const UINT kSplatGroups = 4;                      // matches cs_splat
const float kSplatSpanM = float(kSplatGroups * 16) * TrampleMap::kTexel;   // 6.4 m
const UINT kCbBlock = 256;                        // a CB view must be 256-aligned

// Matches cbuffer TrampleConstants.
struct TrampleConstants {
    float texel;  float map;  UINT n;  UINT stamps;
    float fast;   float slow; int clear_x; int clear_z;
    int clear_w;  int clear_h; float pad0; float pad1;
};
static_assert(sizeof(TrampleConstants) == 48, "TrampleConstants must match trample.hlsl");

}  // namespace

bool TrampleMap::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT srv_slot, UINT uav_slot,
                      std::string& error) {
    ready_ = false;
    heap_ = heap;
    uav_slot_ = uav_slot;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kN;
        td.Height = kN;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                   IID_PPV_ARGS(&tex_)))) {
            error = "trample map texture";
            return false;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(srv_slot) * inc_;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex_.Get(), &sd, h);
        h = heap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(uav_slot) * inc_;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device->CreateUnorderedAccessView(tex_.Get(), nullptr, &ud, h);
    }

    auto upload = [&](ComPtr<ID3D12Resource>& out, void** cpu, UINT64 bytes) -> bool {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&out)))) return false;
        return SUCCEEDED(out->Map(0, nullptr, cpu));
    };
    for (int f = 0; f < 2; ++f) {
        // Three constant blocks: the splat and the fade share one, and each
        // of the two scrolled-in strips needs its own.
        if (!upload(cb_[f], &cb_cpu_[f], kCbBlock * 3)) { error = "trample cb"; return false; }
        if (!upload(stamps_up_[f], &stamps_cpu_[f], sizeof(Stamp) * kMaxStamps)) {
            error = "trample stamps";
            return false;
        }
    }

    {
        D3D12_DESCRIPTOR_RANGE ru = {};
        ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ru.NumDescriptors = 1;
        ru.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1].Descriptor.ShaderRegister = 0;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ru;
        for (int k = 0; k < 3; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err))) {
            error = std::string("trample root: ") +
                    (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                               IID_PPV_ARGS(&root_)))) {
            error = "trample CreateRootSignature";
            return false;
        }
    }

    const char* entries[3] = {"cs_splat", "cs_decay", "cs_clear"};
    ComPtr<ID3D12PipelineState>* psos[3] = {&splat_pso_, &decay_pso_, &clear_pso_};
    for (int k = 0; k < 3; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kShader, entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(psos[k]->GetAddressOf())))) {
            error = std::string("trample pso ") + entries[k];
            return false;
        }
    }
    ready_ = true;
    return true;
}

void TrampleMap::add(float x0, float z0, float x1, float z1, float radius, float strength) {
    if (!ready_ || strength <= 0.0f || radius <= 0.0f) return;
    if (radius > 2.0f) radius = 2.0f;
    // A capsule longer than the splat's own footprint can hold -- a felled
    // log is ten metres -- is cut into pieces that fit, so the kernel keeps
    // one fixed dispatch per stamp.
    const float room = kSplatSpanM - 2.0f * radius;
    const float dx = x1 - x0, dz = z1 - z0;
    const float len = std::sqrt(dx * dx + dz * dz);
    int pieces = 1;
    if (room > 0.1f && len > room) pieces = int(std::ceil(len / room));
    if (pieces > 16) pieces = 16;   // a jump, not a journey: stamp what fits
    for (int k = 0; k < pieces; ++k) {
        if (stamps_.size() >= kMaxStamps) return;
        const float a = float(k) / float(pieces), b = float(k + 1) / float(pieces);
        Stamp s = {};
        s.x0 = x0 + dx * a; s.z0 = z0 + dz * a;
        s.x1 = x0 + dx * b; s.z1 = z0 + dz * b;
        s.radius = radius;
        s.strength = strength > kMaxLean ? kMaxLean : strength;
        stamps_.push_back(s);
    }
}

void TrampleMap::hold(float x0, float z0, float x1, float z1, float radius, float strength) {
    const size_t first = stamps_.size();
    add(x0, z0, x1, z1, radius, strength);
    held_.insert(held_.end(), stamps_.begin() + first, stamps_.end());
    stamps_.resize(first);
}

void TrampleMap::record(ID3D12GraphicsCommandList* cmd, UINT frame, float cam_x, float cam_z, float dt) {
    if (!ready_) return;
    for (size_t k = 0; k < held_.size() && stamps_.size() < kMaxStamps; ++k) stamps_.push_back(held_[k]);
    const UINT f = frame & 1u;
    ++frames_;

    // Where the map's corner sits now, in world texels. The shader never sees
    // this: it reads frac(world / map_m) through a WRAP sampler, which IS the
    // torus. The corner is only how we know what scrolled in.
    const int nx = int(std::floor(cam_x / kTexel)) - int(kN / 2);
    const int nz = int(std::floor(cam_z / kTexel)) - int(kN / 2);
    int dx = have_org_ ? nx - org_x_ : 0, dz = have_org_ ? nz - org_z_ : 0;
    const bool jumped = !have_org_ || full_clear_ ||
                        std::abs(dx) >= int(kN) / 2 || std::abs(dz) >= int(kN) / 2;

    char* cb = static_cast<char*>(cb_cpu_[f]);
    const D3D12_GPU_VIRTUAL_ADDRESS cb_addr = cb_[f]->GetGPUVirtualAddress();
    TrampleConstants base = {};
    base.texel = kTexel;
    base.map = kMapMetres;
    base.n = kN;

    auto barrier = [&] {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = tex_.Get();
        cmd->ResourceBarrier(1, &b);
    };

    // The map is left readable at the end of the frame; take it back.
    {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex_.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        if (have_org_) cmd->ResourceBarrier(1, &b);
    }

    cmd->SetComputeRootSignature(root_.Get());
    D3D12_GPU_DESCRIPTOR_HANDLE uav = heap_->GetGPUDescriptorHandleForHeapStart();
    uav.ptr += UINT64(uav_slot_) * inc_;
    cmd->SetComputeRootDescriptorTable(2, uav);
    cmd->SetComputeRootShaderResourceView(1, stamps_up_[f]->GetGPUVirtualAddress());

    // --- what scrolled in ---------------------------------------------------
    int blocks = 1;
    auto clear = [&](int x, int z, int w, int h) {
        if (w <= 0 || h <= 0 || blocks > 2) return;
        TrampleConstants c = base;
        c.clear_x = x; c.clear_z = z; c.clear_w = w; c.clear_h = h;
        std::memcpy(cb + kCbBlock * blocks, &c, sizeof c);
        cmd->SetComputeRootConstantBufferView(0, cb_addr + UINT64(kCbBlock) * UINT64(blocks));
        cmd->SetPipelineState(clear_pso_.Get());
        cmd->Dispatch(UINT((w + 15) / 16), UINT((h + 15) / 16), 1);
        ++blocks;
    };
    if (jumped) {
        clear(nx, nz, int(kN), int(kN));
        dx = dz = 0;
    } else {
        // The columns and rows the move brought in, in world texels. The
        // strips are written at their world coordinates and the kernel masks
        // them into the torus, so no wrapping arithmetic is needed here.
        if (dx > 0) clear(nx + int(kN) - dx, nz, dx, int(kN));
        else if (dx < 0) clear(nx, nz, -dx, int(kN));
        if (dz > 0) clear(nx, nz + int(kN) - dz, int(kN), dz);
        else if (dz < 0) clear(nx, nz, int(kN), -dz);
    }
    if (blocks > 1) barrier();
    org_x_ = nx; org_z_ = nz;
    have_org_ = true;
    full_clear_ = false;

    // --- this frame's capsules, then the fade -------------------------------
    TrampleConstants c = base;
    c.stamps = UINT(stamps_.size());
    decay_dt_ += dt < 0.0f ? 0.0f : (dt > 0.5f ? 0.5f : dt);
    const bool fade = (frames_ % kDecayStride) == 0 && decay_dt_ > 0.0f;
    c.fast = fade ? std::exp(-decay_dt_ / kFastTau) : 1.0f;
    c.slow = fade ? std::exp(-decay_dt_ / kSlowTau) : 1.0f;
    std::memcpy(cb, &c, sizeof c);
    cmd->SetComputeRootConstantBufferView(0, cb_addr);

    if (!stamps_.empty()) {
        std::memcpy(stamps_cpu_[f], stamps_.data(), stamps_.size() * sizeof(Stamp));
        cmd->SetPipelineState(splat_pso_.Get());
        cmd->Dispatch(kSplatGroups, kSplatGroups, UINT(stamps_.size()));
        barrier();
    }
    if (fade) {
        cmd->SetPipelineState(decay_pso_.Get());
        cmd->Dispatch(kN / 16, kN / 16, 1);
        decay_dt_ = 0.0f;
        barrier();
    }
    stamps_.clear();

    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex_.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &b);
}

}  // namespace engine
