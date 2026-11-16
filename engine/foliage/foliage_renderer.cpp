#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/foliage/foliage_renderer.h"
#include "engine/field/world_gpu.h"
#include "engine/render/shader_compile.h"
#include "worldgen/assign/json_lite.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine {
namespace {

const char* kShader = "engine/render/shaders/foliage.hlsl";
const UINT kPool = 768;              // tile slots
const UINT kTileCells = 256;         // 16 x 16 cells of 1 m
const UINT kCellBytes = 24;
const UINT kBladeBytes = 32;
const UINT kCaps[4] = {400000u, 400000u, 160000u, 40000u};
const UINT kBakePerFrame = 24;
const float kTileM = 16.0f;

// Matches cbuffer FoliageFrame: 272 bytes.
struct FoliageFrame {
    float view_proj[16];
    float cam_pos[3];   float time;
    float frustum[6][4];
    float cam_right[3]; float blade_range;
    float cam_up[3];    float card_near;
    float card_far;     float max_blades;
    UINT  slot_count;   UINT  bake_count;
    UINT  s_tree_patch; float tree_patch_freq;
    float tree_patch_edge; float sea_level;
    float wind;         float far_plane;
    UINT  s_grass;      float grass_freq;
    float near_radius;  float debug;
    float pad[2];
};
static_assert(sizeof(FoliageFrame) == 272, "FoliageFrame must match foliage.hlsl");

std::string hr_text(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

bool make_buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
                 D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out, std::string& error,
                 const char* what) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes ? bytes : 16;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                    IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
             D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &rb);
}

void uav_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    rb.UAV.pResource = r;
    cmd->ResourceBarrier(1, &rb);
}

void frustum_planes(const float m[16], float out[6][4]) {
    float c[4][4];
    for (int cc = 0; cc < 4; ++cc) for (int rr = 0; rr < 4; ++rr) c[cc][rr] = m[rr * 4 + cc];
    for (int k = 0; k < 4; ++k) {
        out[0][k] = c[3][k] + c[0][k];
        out[1][k] = c[3][k] - c[0][k];
        out[2][k] = c[3][k] + c[1][k];
        out[3][k] = c[3][k] - c[1][k];
        out[4][k] = c[2][k];
        out[5][k] = c[3][k] - c[2][k];
    }
    for (int p = 0; p < 6; ++p) {
        const float l = std::sqrt(out[p][0] * out[p][0] + out[p][1] * out[p][1] + out[p][2] * out[p][2]);
        if (l > 0.0f) for (int k = 0; k < 4; ++k) out[p][k] /= l;
    }
}

}  // namespace

bool FoliageRenderer::init(ID3D12Device* device, const WorldGpu* world, const char* archetypes_path,
                           std::string& error) {
    device_ = device;
    world_ = world;
    const worldgen::ArchetypeTable& table = world->table();

    // --- per-biome ground cover, 4 float4 per biome ---
    std::vector<float> ground(table.size() * 16, 0.0f);
    try {
        const jsonlite::Value root = jsonlite::parse_file(archetypes_path);
        const jsonlite::Array& bs = root.array("biomes");
        for (size_t b = 0; b < bs.size() && b < table.size(); ++b) {
            float* g = &ground[b * 16];
            g[14] = table[b].tree_density;
            g[15] = table[b].tree_cover;
            if (!bs[b].has("ground")) continue;
            const jsonlite::Value& gr = bs[b].at("ground");
            const jsonlite::Array& h = gr.array("height");
            const jsonlite::Array& c = gr.array("color");
            const jsonlite::Array& dc = gr.array("dry_color");
            g[0] = float(gr.number("grass"));
            g[1] = float(gr.number("cover"));
            g[2] = float(h[0].num); g[3] = float(h[1].num);
            for (int k = 0; k < 3; ++k) { g[4 + k] = float(c[k].num); g[8 + k] = float(dc[k].num); }
            g[7] = float(gr.number("dry"));
            g[11] = float(gr.number("narrow"));
            g[12] = float(gr.number("flowers"));
            g[13] = float(gr.number("bushes"));
        }
    } catch (const std::exception& e) {
        error = std::string("foliage ground data: ") + e.what();
        return false;
    }
    s_tree_patch_ = world->world().s_tree_patch;
    // The grass patch noise's own seed: presentation, so derived rather than
    // a new worldgen feature id.
    s_grass_ = s_tree_patch_ ^ 0x6A09E667u;
    tree_patch_freq_ = table.trees.patch_freq;
    tree_patch_edge_ = table.trees.patch_edge;
    sea_level_ = table.sea_level;

    // --- root signatures ---
    {
        // Compute: frame CB, world CB, the ten world buffers (u11..u20, as the
        // mesher and the sea bind them), then this pass's own. 44 DWORDs.
        D3D12_ROOT_PARAMETER p[22] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 1;
        for (int k = 0; k < 10; ++k) {
            p[2 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[2 + k].Descriptor.ShaderRegister = 11 + k;
        }
        p[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV; p[12].Descriptor.ShaderRegister = 0;
        p[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[13].Descriptor.ShaderRegister = 0;
        p[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[14].Descriptor.ShaderRegister = 1;
        p[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[15].Descriptor.ShaderRegister = 2;
        for (int k = 0; k < 6; ++k) {
            p[16 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[16 + k].Descriptor.ShaderRegister = 1 + k;
        }
        for (int k = 0; k < 22; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 22;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            error = std::string("foliage compute root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&compute_root_));
        if (FAILED(hr)) { error = hr_text("foliage compute root", hr); return false; }
    }
    {
        // Draw: frame CB, shadow CB, the instance list, the shadow map, the
        // segment count. 8 DWORDs.
        // The shadow table: the sun's map at heap [0], and since ADR-004 L3
        // the local lights' buffers and atlas at [24..28] (t20..t24).
        D3D12_DESCRIPTOR_RANGE sr[3] = {};
        sr[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr[0].NumDescriptors = 1;
        sr[0].BaseShaderRegister = 8;
        sr[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr[1].NumDescriptors = 5;
        sr[1].BaseShaderRegister = 20;
        sr[1].OffsetInDescriptorsFromTableStart = 24;
        // ADR-004 L6: the canopy map at [38] (t25).
        sr[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        sr[2].NumDescriptors = 1;
        sr[2].BaseShaderRegister = 25;
        sr[2].OffsetInDescriptorsFromTableStart = 38;
        D3D12_ROOT_PARAMETER p[5] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[1].Descriptor.ShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].Descriptor.ShaderRegister = 4;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[3].DescriptorTable.NumDescriptorRanges = 3;
        p[3].DescriptorTable.pDescriptorRanges = sr;
        p[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[4].Constants.ShaderRegister = 3;
        p[4].Constants.Num32BitValues = 1;
        for (int k = 0; k < 5; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC cmp = {};
        cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cmp.AddressU = cmp.AddressV = cmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        cmp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        cmp.MaxLOD = D3D12_FLOAT32_MAX;
        cmp.ShaderRegister = 1;
        cmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 5;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers = &cmp;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            error = std::string("foliage draw root: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&draw_root_));
        if (FAILED(hr)) { error = hr_text("foliage draw root", hr); return false; }

        D3D12_INDIRECT_ARGUMENT_DESC a = {};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC csd = {};
        csd.ByteStride = 20;
        csd.NumArgumentDescs = 1;
        csd.pArgumentDescs = &a;
        hr = device_->CreateCommandSignature(&csd, nullptr, IID_PPV_ARGS(&cmd_sig_));
        if (FAILED(hr)) { error = hr_text("foliage command signature", hr); return false; }
    }

    // --- pipelines ---
    const char* cs_e[4] = {"cs_bake", "cs_clear", "cs_emit", "cs_args"};
    ComPtr<ID3D12PipelineState>* cs_o[4] = {&bake_pso_, &clear_pso_, &emit_pso_, &args_pso_};
    for (int k = 0; k < 4; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kShader, cs_e[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = compute_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        const HRESULT hr = device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(cs_o[k]->GetAddressOf()));
        if (FAILED(hr)) { error = hr_text("foliage compute pso", hr); return false; }
    }
    auto gfx = [&](const char* vs_e, const char* ps_e, ComPtr<ID3D12PipelineState>& out) -> bool {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile_shader(kShader, vs_e, "vs_5_1", vs, error)) return false;
        if (!compile_shader(kShader, ps_e, "ps_5_1", ps, error)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = draw_root_.Get();
        pd.VS.pShaderBytecode = vs->GetBufferPointer();
        pd.VS.BytecodeLength = vs->GetBufferSize();
        pd.PS.pShaderBytecode = ps->GetBufferPointer();
        pd.PS.BytecodeLength = ps->GetBufferSize();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;   // blades and cards from both sides
        pd.RasterizerState.DepthClipEnable = TRUE;
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        // The renderer's two HDR targets: direct and ambient (scene_out.hlsli).
        pd.NumRenderTargets = 2;
        pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out));
        if (FAILED(hr)) { error = hr_text("foliage graphics pso", hr); return false; }
        return true;
    };
    if (!gfx("vs_blade", "ps_blade", blade_pso_)) return false;
    if (!gfx("vs_card", "ps_card", card_pso_)) return false;
    if (!gfx("vs_bush", "ps_bush", bush_pso_)) return false;

    // --- buffers ---
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!make_buffer(device_, UINT64(kPool) * kTileCells * kCellBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pool_, error, "foliage pool")) return false;
    for (int l = 0; l < 4; ++l) {
        if (!make_buffer(device_, UINT64(kCaps[l]) * kBladeBytes, D3D12_HEAP_TYPE_DEFAULT, uav,
                         D3D12_RESOURCE_STATE_UNORDERED_ACCESS, lists_[l], error, "foliage list")) return false;
    }
    if (!make_buffer(device_, 64, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     counters_, error, "foliage counters")) return false;
    if (!make_buffer(device_, 4 * 20, D3D12_HEAP_TYPE_DEFAULT, uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     args_, error, "foliage args")) return false;
    if (!make_buffer(device_, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, readback_, error, "foliage readback")) return false;

    // The shapes' indices, LOCAL to each shape: SV_VertexID is the index
    // itself (base vertex 0 -- see CLAUDE.md on BaseVertexLocation), and the
    // vertex shaders build every shape from that number alone.
    std::vector<uint32_t> idx;
    auto blade = [&](uint32_t S) {
        for (uint32_t r = 0; r + 1 < S; ++r) {
            const uint32_t a = 2 * r;
            const uint32_t q[6] = {a, a + 1, a + 2, a + 1, a + 3, a + 2};
            idx.insert(idx.end(), q, q + 6);
        }
        const uint32_t a = 2 * (S - 1);
        idx.push_back(a); idx.push_back(a + 1); idx.push_back(2 * S);
    };
    blade(5);   // 27, at 0
    blade(2);   // 9, at 27
    for (uint32_t q = 0; q < 2; ++q) {   // 12, at 36
        const uint32_t b = q * 4;
        const uint32_t t[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        idx.insert(idx.end(), t, t + 6);
    }
    for (uint32_t c = 0; c < 14; ++c) {   // 84, at 48
        const uint32_t b = c * 4;
        const uint32_t t[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
        idx.insert(idx.end(), t, t + 6);
    }
    if (idx.size() != 132) { error = "foliage index layout disagrees with foliage.hlsl"; return false; }
    if (!make_buffer(device_, idx.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_GENERIC_READ, ib_, error, "foliage indices")) return false;
    {
        void* p = nullptr;
        ib_->Map(0, nullptr, &p);
        std::memcpy(p, idx.data(), idx.size() * 4);
        ib_->Unmap(0, nullptr);
    }
    if (!make_buffer(device_, ground.size() * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_GENERIC_READ, ground_, error, "foliage ground")) return false;
    {
        void* p = nullptr;
        ground_->Map(0, nullptr, &p);
        std::memcpy(p, ground.data(), ground.size() * 4);
        ground_->Unmap(0, nullptr);
    }
    for (int f = 0; f < 2; ++f) {
        if (!make_buffer(device_, UINT64(kPool) * 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, slots_up_[f], error, "foliage slots")) return false;
        slots_up_[f]->Map(0, nullptr, &slots_cpu_[f]);
        std::memset(slots_cpu_[f], 0, kPool * 16);
        if (!make_buffer(device_, UINT64(kPool) * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, bake_up_[f], error, "foliage bake list")) return false;
        bake_up_[f]->Map(0, nullptr, &bake_cpu_[f]);
        if (!make_buffer(device_, 512, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, cb_[f], error, "foliage cb")) return false;
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }

    slot_tile_.assign(kPool, TileKey{0, 0});
    slot_valid_.assign(kPool, 0);
    for (UINT s = kPool; s-- > 0;) free_.push_back(s);
    ready_ = true;
    return true;
}

void FoliageRenderer::update(float x, float z, const FoliageSettings& s, bool settle) {
    if (!ready_) return;
    const float radius = s.card_far + 1.5f * kTileM;
    const int cx = int(std::floor(x / kTileM)), cz = int(std::floor(z / kTileM));
    if (cx != last_cx_ || cz != last_cz_ || radius != last_radius_) {
        last_cx_ = cx; last_cz_ = cz; last_radius_ = radius;
        // Evict what is well outside, then queue what is missing, nearest first.
        for (auto it = tiles_.begin(); it != tiles_.end();) {
            const float tx = (it->first.x + 0.5f) * kTileM - x, tz = (it->first.z + 0.5f) * kTileM - z;
            if (std::sqrt(tx * tx + tz * tz) > radius + 2.0f * kTileM) {
                slot_valid_[it->second] = 0;
                free_.push_back(it->second);
                it = tiles_.erase(it);
            } else {
                ++it;
            }
        }
        const int reach = int(std::ceil(radius / kTileM)) + 1;
        std::vector<std::pair<float, TileKey> > want;
        for (int j = cz - reach; j <= cz + reach; ++j) {
            for (int i = cx - reach; i <= cx + reach; ++i) {
                const float tx = (i + 0.5f) * kTileM - x, tz = (j + 0.5f) * kTileM - z;
                const float d = std::sqrt(tx * tx + tz * tz) - 0.7072f * kTileM;
                if (d > radius) continue;
                TileKey k; k.x = i; k.z = j;
                if (tiles_.count(k)) continue;
                want.push_back(std::make_pair(d, k));
            }
        }
        std::sort(want.begin(), want.end(),
                  [](const std::pair<float, TileKey>& a, const std::pair<float, TileKey>& b) { return a.first < b.first; });
        queue_.clear();
        for (size_t i = 0; i < want.size(); ++i) queue_.push_back(want[i].second);
    }
    // Hand out slots to this frame's bakes.
    bake_now_.clear();
    const size_t budget = settle ? kPool : kBakePerFrame;
    while (!queue_.empty() && bake_now_.size() < budget && !free_.empty()) {
        const TileKey k = queue_.front();
        queue_.pop_front();
        if (tiles_.count(k)) continue;
        const uint32_t slot = free_.back();
        free_.pop_back();
        tiles_[k] = slot;
        slot_tile_[slot] = k;
        slot_valid_[slot] = 1;   // baked before this frame's emit reads it
        bake_now_.push_back(slot);
    }
}

void FoliageRenderer::record(ID3D12GraphicsCommandList* cmd, UINT frame, const FoliageFrameInputs& in,
                             const FoliageSettings& s) {
    if (!ready_) return;
    const UINT f = frame & 1u;
    {
        int* st = static_cast<int*>(slots_cpu_[f]);
        for (UINT k = 0; k < kPool; ++k) {
            st[k * 4 + 0] = slot_tile_[k].x;
            st[k * 4 + 1] = slot_tile_[k].z;
            st[k * 4 + 2] = slot_valid_[k];
            st[k * 4 + 3] = 0;
        }
        if (!bake_now_.empty()) std::memcpy(bake_cpu_[f], bake_now_.data(), bake_now_.size() * 4);
    }
    FoliageFrame fr = {};
    std::memcpy(fr.view_proj, in.view_proj, sizeof fr.view_proj);
    for (int k = 0; k < 3; ++k) {
        fr.cam_pos[k] = in.cam_pos[k];
        fr.cam_right[k] = in.cam_right[k];
        fr.cam_up[k] = in.cam_up[k];
    }
    fr.time = in.time;
    frustum_planes(in.view_proj, fr.frustum);
    fr.blade_range = s.blade_range;
    fr.card_near = s.card_near;
    fr.card_far = s.card_far;
    fr.max_blades = s.max_blades;
    fr.slot_count = kPool;
    fr.bake_count = static_cast<UINT>(bake_now_.size());
    fr.s_tree_patch = s_tree_patch_;
    fr.tree_patch_freq = tree_patch_freq_;
    fr.tree_patch_edge = tree_patch_edge_;
    fr.sea_level = sea_level_;
    fr.wind = s.wind;
    fr.far_plane = in.far_plane;
    fr.s_grass = s_grass_;
    fr.grass_freq = 1.0f / 45.0f;
    fr.near_radius = s.near_radius;
    fr.debug = s.debug;
    std::memcpy(cb_cpu_[f], &fr, sizeof fr);
    const D3D12_GPU_VIRTUAL_ADDRESS cb = cb_[f]->GetGPUVirtualAddress();

    // --- compute: bake new tiles, then emit this frame's instances ---
    cmd->SetComputeRootSignature(compute_root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb);
    cmd->SetComputeRootConstantBufferView(1, world_->world_cb()->GetGPUVirtualAddress());
    ID3D12Resource* wb[10] = {
        world_->cells(), world_->cell_offsets(), world_->cell_items(), world_->archetypes(),
        world_->rivers(), world_->river_offsets(), world_->river_items(), world_->entrances(),
        world_->entrance_offsets(), world_->entrance_items()};
    for (int k = 0; k < 10; ++k) cmd->SetComputeRootUnorderedAccessView(2 + k, wb[k]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(12, pool_->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(13, slots_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(14, bake_up_[f]->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(15, ground_->GetGPUVirtualAddress());
    for (int l = 0; l < 4; ++l) cmd->SetComputeRootUnorderedAccessView(16 + l, lists_[l]->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(20, counters_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(21, args_->GetGPUVirtualAddress());
    if (!bake_now_.empty()) {
        cmd->SetPipelineState(bake_pso_.Get());
        cmd->Dispatch(static_cast<UINT>(bake_now_.size()), 1, 1);
        uav_barrier(cmd, pool_.Get());
    }
    cmd->SetPipelineState(clear_pso_.Get());
    cmd->Dispatch(1, 1, 1);
    uav_barrier(cmd, counters_.Get());
    cmd->SetPipelineState(emit_pso_.Get());
    cmd->Dispatch(kPool, 1, 1);
    uav_barrier(cmd, counters_.Get());
    cmd->SetPipelineState(args_pso_.Get());
    cmd->Dispatch(1, 1, 1);
    barrier(cmd, args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    for (int l = 0; l < 4; ++l)
        barrier(cmd, lists_[l].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- draw ---
    cmd->SetGraphicsRootSignature(draw_root_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb);
    cmd->SetGraphicsRootConstantBufferView(1, in.shadow_cb ? in.shadow_cb : cb);
    cmd->SetGraphicsRootDescriptorTable(3, in.shadow_table);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv.SizeInBytes = 132 * 4;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    cmd->IASetIndexBuffer(&ibv);
    ID3D12PipelineState* pso[4] = {blade_pso_.Get(), blade_pso_.Get(), card_pso_.Get(), bush_pso_.Get()};
    const UINT segs[4] = {5, 2, 0, 0};
    for (int l = 0; l < 4; ++l) {
        cmd->SetPipelineState(pso[l]);
        cmd->SetGraphicsRootShaderResourceView(2, lists_[l]->GetGPUVirtualAddress());
        cmd->SetGraphicsRoot32BitConstant(4, segs[l], 0);
        cmd->ExecuteIndirect(cmd_sig_.Get(), 1, args_.Get(), UINT64(l) * 20, nullptr, 0);
    }
    barrier(cmd, args_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (int l = 0; l < 4; ++l)
        barrier(cmd, lists_[l].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    bake_now_.clear();
}

bool FoliageRenderer::read_counts(ID3D12CommandQueue* queue, uint32_t out[5], std::string& error) {
    if (!ready_) return false;
    ComPtr<ID3D12CommandAllocator> al;
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al.Get(), nullptr, IID_PPV_ARGS(&cl))) ||
        FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        error = "foliage readback setup";
        return false;
    }
    barrier(cl.Get(), counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl->CopyBufferRegion(readback_.Get(), 0, counters_.Get(), 0, 20);
    barrier(cl.Get(), counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cl->Close();
    ID3D12CommandList* ls[] = {cl.Get()};
    queue->ExecuteCommandLists(1, ls);
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, INFINITE);
    CloseHandle(ev);
    uint32_t* p = nullptr;
    readback_->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (int k = 0; k < 5; ++k) out[k] = p[k];
    readback_->Unmap(0, nullptr);
    return true;
}

}  // namespace engine
