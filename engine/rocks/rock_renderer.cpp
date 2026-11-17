#include "engine/rocks/rock_renderer.h"
#include "engine/rocks/rock_field.h"
#include "engine/physics/physics_world.h"
#include "engine/render/shader_compile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {

namespace {

const char* kRockShader = "engine/render/shaders/rocks.hlsl";

std::string hr_text(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

// Planes of the frustum from a row-vector view_proj (clip = p * M), as
// (n, d) with dot(n, p) + d >= 0 inside; D3D depth 0..1.
void frustum_planes(const float m[16], float out[6][4]) {
    float c[4][4];
    for (int k = 0; k < 4; ++k) for (int r = 0; r < 4; ++r) c[k][r] = m[r * 4 + k];
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

ComPtr<ID3D12Resource> buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = type;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

}  // namespace

bool RockRenderer::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
                        std::string& error) {
    build_rock_meshes(meshes_);

    // --- the meshes, uploaded once into default-heap buffers ---------------
    const UINT64 vbytes = meshes_.v.size() * sizeof(RockVertex), ibytes = meshes_.idx.size() * 4;
    vb_ = buffer(device, vbytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ib_ = buffer(device, ibytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> up = buffer(device, vbytes + ibytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vb_ || !ib_ || !up) { error = "rock mesh buffers"; return false; }
    {
        uint8_t* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        std::memcpy(p, meshes_.v.data(), size_t(vbytes));
        std::memcpy(p + vbytes, meshes_.idx.data(), size_t(ibytes));
        up->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        ComPtr<ID3D12Fence> fence;
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) { error = hr_text("rock upload setup", hr); return false; }
        cmd->CopyBufferRegion(vb_.Get(), 0, up.Get(), 0, vbytes);
        cmd->CopyBufferRegion(ib_.Get(), 0, up.Get(), vbytes, ibytes);
        D3D12_RESOURCE_BARRIER b[2] = {};
        for (int k = 0; k < 2; ++k) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[k].Transition.pResource = k == 0 ? vb_.Get() : ib_.Get();
            b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b[k].Transition.StateAfter = k == 0 ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
                                                : D3D12_RESOURCE_STATE_INDEX_BUFFER;
            b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmd->ResourceBarrier(2, b);
        cmd->Close();
        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence.Get(), 1);
        if (fence->GetCompletedValue() < 1) {
            HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(1, ev);
            WaitForSingleObject(ev, INFINITE);
            CloseHandle(ev);
        }
    }
    vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
    vbv_.SizeInBytes = UINT(vbytes);
    vbv_.StrideInBytes = sizeof(RockVertex);
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv_.SizeInBytes = UINT(ibytes);
    ibv_.Format = DXGI_FORMAT_R32_UINT;

    for (UINT f = 0; f < kFrames; ++f) {
        inst_[f] = buffer(device, UINT64(kMaxMain + kMaxShadow) * sizeof(RockGpu), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!inst_[f]) { error = "rock instance buffer"; return false; }
        inst_[f]->Map(0, nullptr, reinterpret_cast<void**>(&inst_cpu_[f]));
    }

    // --- pipelines, on the terrain's root signature -------------------------
    ComPtr<ID3DBlob> vs, ps, vss, pst;
    if (!compile_shader(kRockShader, "ps_rock_top", "ps_5_1", pst, error)) return false;
    if (!compile_shader(kRockShader, "vs_rock", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kRockShader, "ps_rock", "ps_5_1", ps, error)) return false;
    if (!compile_shader(kRockShader, "vs_rock_shadow", "vs_5_1", vss, error)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = mesh_root;
    pd.InputLayout = {layout, 2};
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    pd.RasterizerState.FrontCounterClockwise = FALSE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 2;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (rocks)", hr); return false; }
    // From above, for the heightfield GI: one RGBA16F target, no culling
    // (the top-down matrix mirrors one axis), as the terrain's.
    pd.PS = {pst->GetBufferPointer(), pst->GetBufferSize()};
    pd.NumRenderTargets = 1;
    pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&top_pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (rocks top-down)", hr); return false; }
    pd.NumRenderTargets = 2;
    pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    // Under the sun: depth only, both faces (a thin edge seen from the sun
    // must still cast), the terrain caster's slope bias.
    pd.VS = {vss->GetBufferPointer(), vss->GetBufferSize()};
    pd.PS = {nullptr, 0};
    pd.NumRenderTargets = 0;
    pd.RTVFormats[0] = pd.RTVFormats[1] = DXGI_FORMAT_UNKNOWN;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.RasterizerState.DepthBiasClamp = 0.002f;
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadow_pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (rock shadow)", hr); return false; }
    ready_ = true;
    return true;
}

void RockRenderer::prepare(UINT frame, const std::vector<worldgen::RockInstance>& rocks, const float cam[3],
                           const float view_proj[16]) {
    main_[frame].clear();
    shadow_[frame].clear();
    drawn_ = 0;
    triangles_ = 0;
    if (!ready_) return;
    float planes[6][4];
    frustum_planes(view_proj, planes);
    const uint32_t nm = uint32_t(meshes_.models.size());
    // Bucket by (model, level): count, then place.
    std::vector<uint32_t> main_idx, shadow_idx;   // packed: rock index << 8 | model * 2 + lod
    main_idx.reserve(rocks.size());
    for (size_t i = 0; i < rocks.size(); ++i) {
        const worldgen::RockInstance& r = rocks[i];
        const uint32_t m = rock_model(r.kind, r.variant);
        const RockModel& rm = meshes_.models[m];
        const float hh = 0.5f * rm.height * r.size;
        const float c[3] = {r.x, r.y + hh, r.z};
        const float br = r.size * std::sqrt(rm.radius * rm.radius + 0.25f * rm.height * rm.height);
        const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float sreach = r.kind == 0 ? kShadowBoulders : kShadowOutcrops;
        if (d - br < sreach) shadow_idx.push_back(uint32_t(i) << 8 | (m * 2 + 1));
        if (d - br > kDrawDist) continue;
        bool in = true;
        for (int p = 0; p < 6 && in; ++p)
            if (planes[p][0] * c[0] + planes[p][1] * c[1] + planes[p][2] * c[2] + planes[p][3] < -br) in = false;
        if (!in) continue;
        const uint32_t lod = d < 30.0f + 12.0f * r.size ? 0u : 1u;
        main_idx.push_back(uint32_t(i) << 8 | (m * 2 + lod));
    }
    auto fill = [&](std::vector<uint32_t>& idx, std::vector<Group>& groups, UINT base, UINT cap) {
        std::sort(idx.begin(), idx.end(), [](uint32_t a, uint32_t b) { return (a & 0xFFu) < (b & 0xFFu); });
        if (idx.size() > cap) idx.resize(cap);
        RockGpu* out = inst_cpu_[frame] + base;
        for (size_t k = 0; k < idx.size(); ++k) {
            const worldgen::RockInstance& r = rocks[idx[k] >> 8];
            const uint32_t key = idx[k] & 0xFFu;
            if (groups.empty() || groups.back().model * 2 + groups.back().lod != key)
                groups.push_back(Group{key >> 1, key & 1u, uint32_t(k), 0u});
            ++groups.back().count;
            RockGpu g = {};
            g.pos[0] = r.x; g.pos[1] = r.y; g.pos[2] = r.z;
            g.scale = r.size;
            rock_rotation(r, g.rot);
            g.biome = r.biome;
            g.draw_dist = kDrawDist;
            out[k] = g;
        }
    };
    fill(main_idx, main_[frame], 0, kMaxMain);
    fill(shadow_idx, shadow_[frame], kMaxMain, kMaxShadow);
    drawn_ = main_idx.size();
    for (const Group& g : main_[frame]) triangles_ += size_t(g.count) * meshes_.models[g.model].count[g.lod] / 3;
    (void)nm;
}

void RockRenderer::bind_mesh(ID3D12GraphicsCommandList* cmd) {
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv_);
    cmd->IASetIndexBuffer(&ibv_);
}

void RockRenderer::draw_groups(ID3D12GraphicsCommandList* cmd, UINT frame, const std::vector<Group>& groups,
                               UINT base) {
    const D3D12_GPU_VIRTUAL_ADDRESS va = inst_[frame]->GetGPUVirtualAddress();
    for (const Group& g : groups) {
        const RockModel& rm = meshes_.models[g.model];
        cmd->SetGraphicsRootShaderResourceView(1, va + UINT64(base + g.first) * sizeof(RockGpu));
        cmd->DrawIndexedInstanced(rm.count[g.lod], g.count, rm.first[g.lod], 0, 0);
    }
}

void RockRenderer::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || main_[frame].empty()) return;
    cmd->SetPipelineState(pso_.Get());
    bind_mesh(cmd);
    draw_groups(cmd, frame, main_[frame], 0);
}

void RockRenderer::record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || shadow_[frame].empty()) return;
    cmd->SetPipelineState(shadow_pso_.Get());
    bind_mesh(cmd);
    draw_groups(cmd, frame, shadow_[frame], kMaxMain);
}

void RockRenderer::record_top(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || shadow_[frame].empty()) return;
    cmd->SetPipelineState(top_pso_.Get());
    bind_mesh(cmd);
    draw_groups(cmd, frame, shadow_[frame], kMaxMain);
}

void RockRenderer::sync_colliders(PhysicsWorld* physics, const std::vector<worldgen::RockInstance>& rocks,
                                  unsigned long long version, float x, float z) {
    if (!ready_ || !physics) return;
    // Rebuilt until the shapes exist: a first call before physics is up
    // (a still's report) must not leave every later one without them.
    if (phys_ != physics || shapes_built() == 0) {
        phys_ = physics;
        phys_mesh_.clear();
        colliders_.clear();
        for (const RockModel& rm : meshes_.models) {
            std::vector<float> v(size_t(rm.vcount[1]) * 3);
            for (uint32_t i = 0; i < rm.vcount[1]; ++i)
                for (int k = 0; k < 3; ++k) v[size_t(i) * 3 + k] = meshes_.v[rm.vfirst[1] + i].pos[k];
            // Wound the other way from the draw: the draw's triangles are
            // clockwise from outside for D3D, and Jolt takes clockwise as a
            // BACK face -- its rays ignore those, and a ray down a boulder's
            // axis went straight through it to the terrain.
            std::vector<unsigned> idx(rm.count[1]);
            for (uint32_t i = 0; i < rm.count[1]; i += 3) {
                idx[i] = meshes_.idx[rm.first[1] + i] - rm.vfirst[1];
                idx[i + 1] = meshes_.idx[rm.first[1] + i + 2] - rm.vfirst[1];
                idx[i + 2] = meshes_.idx[rm.first[1] + i + 1] - rm.vfirst[1];
            }
            phys_mesh_.push_back(physics->add_prop_mesh(v.data(), rm.vcount[1], idx.data(), rm.count[1]));
        }
    }
    const float dx = x - last_x_, dz = z - last_z_;
    if (dx * dx + dz * dz < 9.0f && version == seen_version_) return;
    last_x_ = x; last_z_ = z;
    seen_version_ = version;
    std::unordered_map<uint32_t, uint32_t> keep;
    for (const worldgen::RockInstance& r : rocks) {
        const float ex = r.x - x, ez = r.z - z;
        const float reach = kColliderRadius + r.size;
        if (ex * ex + ez * ez > reach * reach) continue;
        auto it = colliders_.find(r.id);
        if (it != colliders_.end()) { keep[r.id] = it->second; colliders_.erase(it); continue; }
        const int pm = phys_mesh_[rock_model(r.kind, r.variant)];
        if (pm < 0) continue;
        float q[4];
        rock_rotation(r, q);
        const float pos[3] = {r.x, r.y, r.z};
        const uint32_t h = physics->add_static_prop(pm, pos, q, r.size);
        if (h) keep[r.id] = h;
    }
    for (auto& kv : colliders_) physics->remove_body(kv.second);
    colliders_.swap(keep);
}

}  // namespace engine
