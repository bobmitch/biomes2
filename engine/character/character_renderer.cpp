#include "engine/character/character_renderer.h"
#include "engine/render/shader_compile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace engine {

namespace {

const char* kCharacterShader = "engine/render/shaders/character.hlsl";

std::string hr_text(const char* what, HRESULT hr) {
    char buf[160];
    std::snprintf(buf, sizeof buf, "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

Microsoft::WRL::ComPtr<ID3D12Resource> buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE type,
                                              D3D12_RESOURCE_STATES state) {
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
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

}  // namespace

bool CharacterRenderer::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
                             ID3D12DescriptorHeap* heap, const std::string& pack, std::string& error) {
    ready_ = false;
    // Null views first, so the terrain's table is whole with no character.
    {
        const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MipLevels = 1;
        sv.Texture2DArray.ArraySize = 1;
        sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateShaderResourceView(nullptr, &sv, {h.ptr + SIZE_T(kSrvA) * inc});
        device->CreateShaderResourceView(nullptr, &sv, {h.ptr + SIZE_T(kSrvB) * inc});
    }
    std::string why;
    if (!model_.load(pack, why)) { note_ = why; return true; }
    if (model_.joints.size() > kMaxJoints) { note_ = pack + ": too many joints"; return true; }

    const UINT nm = UINT(model_.materials.size()), size = model_.tex_size, mips = model_.tex_mips;
    const UINT64 vbytes = model_.vertices.size() * sizeof(CharacterVertex), ibytes = model_.indices.size() * 4;
    vb_ = buffer(device, vbytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ib_ = buffer(device, ibytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!vb_ || !ib_) { error = "character mesh buffers"; return false; }

    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = size;
    td.Height = size;
    td.DepthOrArraySize = UINT16(nm);
    td.MipLevels = UINT16(mips);
    td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    HRESULT hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&tex_a_));
    if (FAILED(hr)) { error = hr_text("character albedo array", hr); return false; }
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    hr = device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&tex_b_));
    if (FAILED(hr)) { error = hr_text("character normal array", hr); return false; }

    // One staging buffer: mesh, then both arrays' footprints.
    const UINT subs = nm * mips;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> nrows(subs);
    std::vector<UINT64> row_bytes(subs);
    UINT64 tex_total = 0;
    device->GetCopyableFootprints(&td, 0, subs, 0, fp.data(), nrows.data(), row_bytes.data(), &tex_total);
    // Placed footprints must start on 512 bytes: both arrays' blocks are.
    tex_total = (tex_total + 511) & ~UINT64(511);
    const UINT64 tex_base = (vbytes + ibytes + 511) & ~UINT64(511);
    const UINT64 total = tex_base + 2 * tex_total;
    ComPtr<ID3D12Resource> up = buffer(device, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!up) { error = "character staging"; return false; }
    uint8_t* base = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&base));
    std::memcpy(base, model_.vertices.data(), size_t(vbytes));
    std::memcpy(base + vbytes, model_.indices.data(), size_t(ibytes));
    for (int t = 0; t < 2; ++t) {
        uint8_t* tb = base + tex_base + UINT64(t) * tex_total;
        for (UINT l = 0; l < nm; ++l) {
            const std::vector<uint8_t>& src = t == 0 ? model_.tex_a[l] : model_.tex_b[l];
            size_t off = 0;
            for (UINT m = 0; m < mips; ++m) {
                const UINT s = m + l * mips;
                const UINT w = (std::max)(size >> m, 1u);
                for (UINT y = 0; y < nrows[s]; ++y)
                    std::memcpy(tb + fp[s].Offset + UINT64(y) * fp[s].Footprint.RowPitch,
                                src.data() + off + size_t(y) * w * 4, size_t(w) * 4);
                off += size_t(w) * w * 4;
            }
        }
    }
    up->Unmap(0, nullptr);
    model_.release_textures();

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) { error = hr_text("character upload setup", hr); return false; }
    cmd->CopyBufferRegion(vb_.Get(), 0, up.Get(), 0, vbytes);
    cmd->CopyBufferRegion(ib_.Get(), 0, up.Get(), vbytes, ibytes);
    ID3D12Resource* dst[2] = {tex_a_.Get(), tex_b_.Get()};
    for (int t = 0; t < 2; ++t)
        for (UINT s = 0; s < subs; ++s) {
            D3D12_TEXTURE_COPY_LOCATION d = {}, sl = {};
            d.pResource = dst[t];
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            d.SubresourceIndex = s;
            sl.pResource = up.Get();
            sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            sl.PlacedFootprint = fp[s];
            sl.PlacedFootprint.Offset += tex_base + UINT64(t) * tex_total;
            cmd->CopyTextureRegion(&d, 0, 0, 0, &sl, nullptr);
        }
    D3D12_RESOURCE_BARRIER b[4] = {};
    ID3D12Resource* res[4] = {vb_.Get(), ib_.Get(), tex_a_.Get(), tex_b_.Get()};
    const D3D12_RESOURCE_STATES after[4] = {
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER, D3D12_RESOURCE_STATE_INDEX_BUFFER,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    for (int k = 0; k < 4; ++k) {
        b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[k].Transition.pResource = res[k];
        b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b[k].Transition.StateAfter = after[k];
        b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    cmd->ResourceBarrier(4, b);
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

    vbv_.BufferLocation = vb_->GetGPUVirtualAddress();
    vbv_.SizeInBytes = UINT(vbytes);
    vbv_.StrideInBytes = sizeof(CharacterVertex);
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv_.SizeInBytes = UINT(ibytes);
    ibv_.Format = DXGI_FORMAT_R32_UINT;

    const UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = mips;
    sv.Texture2DArray.ArraySize = nm;
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    device->CreateShaderResourceView(tex_a_.Get(), &sv, {h.ptr + SIZE_T(kSrvA) * inc});
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    device->CreateShaderResourceView(tex_b_.Get(), &sv, {h.ptr + SIZE_T(kSrvB) * inc});

    for (UINT f = 0; f < kFrames; ++f) {
        bones_[f] = buffer(device, UINT64(kMaxBodies) * kMaxJoints * 48, D3D12_HEAP_TYPE_UPLOAD,
                           D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!bones_[f]) { error = hr_text("character bone buffer (device removed?)", device->GetDeviceRemovedReason()); return false; }
        bones_[f]->Map(0, nullptr, reinterpret_cast<void**>(&bones_cpu_[f]));
    }

    // --- pipelines -----------------------------------------------------------
    ComPtr<ID3DBlob> vs, ps, vss;
    if (!compile_shader(kCharacterShader, "vs_character", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kCharacterShader, "ps_character", "ps_5_1", ps, error)) return false;
    if (!compile_shader(kCharacterShader, "vs_character_shadow", "vs_5_1", vss, error)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[6] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"BLENDWEIGHT", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_UINT, 0, 40, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = mesh_root;
    pd.InputLayout = {layout, 6};
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    // Every material in the kit is double sided; ps_character flips a back
    // face's normal.
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.FrontCounterClockwise = TRUE;   // glTF's winding
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 3;
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (character)", hr); return false; }
    pd.VS = {vss->GetBufferPointer(), vss->GetBufferSize()};
    pd.PS = {nullptr, 0};
    pd.NumRenderTargets = 0;
    pd.RTVFormats[0] = pd.RTVFormats[1] = pd.RTVFormats[2] = DXGI_FORMAT_UNKNOWN;
    pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
    pd.RasterizerState.DepthBiasClamp = 0.002f;
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&shadow_pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (character shadow)", hr); return false; }
    // A local light's faces: the same, with a pixel shader that clears the
    // body round the flame (the carried torch).
    ComPtr<ID3DBlob> vsl, psl;
    if (!compile_shader(kCharacterShader, "vs_character_light_shadow", "vs_5_1", vsl, error)) return false;
    if (!compile_shader(kCharacterShader, "ps_character_light_shadow", "ps_5_1", psl, error)) return false;
    pd.VS = {vsl->GetBufferPointer(), vsl->GetBufferSize()};
    pd.PS = {psl->GetBufferPointer(), psl->GetBufferSize()};
    hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&light_shadow_pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (character light shadow)", hr); return false; }

    char buf[256];
    std::snprintf(buf, sizeof buf, "%s: %zu joints, %zu vertices, %zu triangles, %u materials at %u^2",
                  pack.c_str(), model_.joints.size(), model_.vertices.size(), model_.indices.size() / 3, nm, size);
    note_ = buf;
    ready_ = true;
    return true;
}

void CharacterRenderer::set_pose(const std::vector<float>& bones, bool visible) {
    pose_ = bones;
    visible_ = visible && ready_ && pose_.size() == model_.joints.size() * 12;
    if (!visible_) return;
    // Every joint where this pose puts it: its skinning matrix times its
    // rest position. Their box, padded for flesh past the joints (the top
    // of the head, the fingertips), is the body's bound.
    const size_t nj = model_.joints.size();
    joints_.resize(nj * 3);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t j = 0; j < nj; ++j) {
        const float* m = &pose_[j * 12];
        const float* r = model_.joints[j].rest_p;
        for (int k = 0; k < 3; ++k) {
            const float* row = m + k * 4;
            const float p = row[0] * r[0] + row[1] * r[1] + row[2] * r[2] + row[3];
            joints_[j * 3 + k] = p;
            lo[k] = (std::min)(lo[k], p);
            hi[k] = (std::max)(hi[k], p);
        }
    }
    float r2 = 0.0f;
    for (int k = 0; k < 3; ++k) {
        bound_c_[k] = 0.5f * (lo[k] + hi[k]);
        r2 += (hi[k] - bound_c_[k]) * (hi[k] - bound_c_[k]);
    }
    bound_r_ = std::sqrt(r2) + 0.3f;
    // A new serial when any joint has moved 3 mm from the pose the last
    // serial stood for: what the local lights' cached faces key on.
    bool moved = committed_.size() != joints_.size();
    for (size_t i = 0; !moved && i < nj; ++i) {
        const float dx = joints_[i * 3] - committed_[i * 3], dy = joints_[i * 3 + 1] - committed_[i * 3 + 1],
                    dz = joints_[i * 3 + 2] - committed_[i * 3 + 2];
        moved = dx * dx + dy * dy + dz * dz > 9e-6f;
    }
    if (moved) {
        committed_ = joints_;
        ++pose_serial_;
    }
}

bool CharacterRenderer::bounds(float c[3], float& r) const {
    if (!visible_) return false;
    for (int k = 0; k < 3; ++k) c[k] = bound_c_[k];
    r = bound_r_;
    return true;
}

void CharacterRenderer::set_creatures(const std::vector<float>& bones, const std::vector<float>& tints, size_t n) {
    const size_t per = model_.joints.size() * 12;
    if (n > kMaxBodies - 1) n = kMaxBodies - 1;
    if (!ready_ || bones.size() < n * per || tints.size() < n * 3) n = 0;
    creature_n_ = n;
    creature_bones_.assign(bones.begin(), bones.begin() + std::ptrdiff_t(n * per));
    creature_tints_.assign(tints.begin(), tints.begin() + std::ptrdiff_t(n * 3));
}

bool CharacterRenderer::creature_near(const float p[3], float r) const {
    const size_t per = model_.joints.size() * 12;
    for (size_t i = 0; i < creature_n_; ++i) {
        const float* m = &creature_bones_[i * per];   // joint 0's rows: its translation in [3], [7], [11]
        const float dx = m[3] - p[0], dy = m[7] - p[1], dz = m[11] - p[2];
        if (dx * dx + dy * dy + dz * dz < r * r) return true;
    }
    return false;
}

void CharacterRenderer::prepare(UINT frame) {
    drawn_[frame] = false;
    bodies_[frame] = 0;
    if (!ready_) return;
    const size_t per = model_.joints.size() * 12;
    const size_t block = size_t(kMaxJoints) * 12;
    // Block 0 is the player's, drawn or not; creatures from block 1.
    auto write = [&](UINT b, const float* bones, const float tint[3]) {
        float* dst = bones_cpu_[frame] + size_t(b) * block;
        std::memcpy(dst, bones, per * sizeof(float));
        float* t = dst + size_t(kMaxJoints - 1) * 12;
        t[0] = tint[0]; t[1] = tint[1]; t[2] = tint[2]; t[3] = 0.0f;
    };
    const float white[3] = {1.0f, 1.0f, 1.0f};
    if (visible_) {
        write(0, pose_.data(), white);
        drawn_[frame] = true;
    }
    for (size_t i = 0; i < creature_n_; ++i)
        write(UINT(i + 1), &creature_bones_[i * per], &creature_tints_[i * 3]);
    bodies_[frame] = UINT(creature_n_ + 1);
}

void CharacterRenderer::bind(ID3D12GraphicsCommandList* cmd) {
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv_);
    cmd->IASetIndexBuffer(&ibv_);
}

// Every body this frame: the player's block if it is drawn, then each
// creature's, root SRV t0 moved to the block.
void CharacterRenderer::draw_bodies(ID3D12GraphicsCommandList* cmd, UINT frame) {
    const D3D12_GPU_VIRTUAL_ADDRESS base = bones_[frame]->GetGPUVirtualAddress();
    const UINT64 stride = UINT64(kMaxJoints) * 48;
    for (UINT b = drawn_[frame] ? 0u : 1u; b < bodies_[frame]; ++b) {
        cmd->SetGraphicsRootShaderResourceView(1, base + stride * b);
        cmd->DrawIndexedInstanced(UINT(model_.indices.size()), 1, 0, 0, 0);
    }
}

void CharacterRenderer::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || (!drawn_[frame] && bodies_[frame] <= 1)) return;
    cmd->SetPipelineState(pso_.Get());
    bind(cmd);
    draw_bodies(cmd, frame);
}

void CharacterRenderer::record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || (!drawn_[frame] && bodies_[frame] <= 1)) return;
    cmd->SetPipelineState(shadow_pso_.Get());
    bind(cmd);
    draw_bodies(cmd, frame);
}

void CharacterRenderer::record_light_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, const float light_pos[3],
                                            float clear) {
    if (!ready_ || (!drawn_[frame] && bodies_[frame] <= 1)) return;
    // MeshConstants: cam_pos at 16, far_plane at 19 -- the light and the
    // clear radius for ps_character_light_shadow. The next face sets the
    // whole block again.
    const float c[4] = {light_pos[0], light_pos[1], light_pos[2], clear};
    cmd->SetGraphicsRoot32BitConstants(0, 4, c, 16);
    cmd->SetPipelineState(light_shadow_pso_.Get());
    bind(cmd);
    draw_bodies(cmd, frame);
}

}  // namespace engine
