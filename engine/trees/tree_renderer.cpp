#include "engine/trees/tree_renderer.h"
#include "engine/render/shader_compile.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace engine {
namespace {

const char* kTreeShader = "engine/render/shaders/trees.hlsl";
const char* kCullShader = "engine/render/shaders/tree_cull.hlsl";

// Impostor atlas: kFrames x kFrames views of kFramePx each, per model.
const UINT kFrames = 8;
const UINT kFramePx = 80;
const UINT kAtlas = kFrames * kFramePx;   // 640
const UINT kAtlasMips = 5;                // 640 .. 40; a frame is 5 px at the last

// Visible-list capacity per draw. LOD 0 is the nearest 45 m, LOD 1 out to
// 170 m; a single model in a monoculture is the worst case for either.
const UINT kCapLod0 = 1024;
const UINT kCapLod1 = 4096;
const UINT kCapImp = 98304;
const UINT kMaxInstances = 131072;        // 20 bits of index in a visible entry

// Matches cbuffer TreeFrame in trees.hlsl and tree_cull.hlsl: 320 bytes.
struct TreeFrame {
    float view_proj[16];
    float cam_pos[3];   float time;
    float sun_dir[3];   float max_dist;
    float cam_right[3]; float lod0_dist;
    float cam_up[3];    float lod1_dist;
    float cam_fwd[3];   float far_plane;
    float frustum[6][4];
    UINT  instance_count;
    UINT  model_count;
    UINT  frames;
    UINT  quad_first;
    UINT  quad_base;
    float wind;
    float cut_height;
    float debug;
    float bake_dir[3];   float bake_r;
    float bake_right[3]; float bake_centre_y;
    float bake_up[3];    float shadow_pass;
};
static_assert(sizeof(TreeFrame) == 320, "TreeFrame must match the HLSL cbuffer");
const UINT kCbStride = 512;
// Per frame in flight: the camera's constants, then one per shadow cascade.
const UINT kCbPerFrame = 4;

struct ModelInfoGpu {
    float radius, top, bound, slice;
    UINT  first0, count0, base0;
    UINT  first1, count1, base1;
    UINT  species, pad;
};
static_assert(sizeof(ModelInfoGpu) == 48, "ModelInfo must match the HLSL struct");

struct LookGpu {
    float bark[3]; float leaf_kind;
    float leaf[3]; float pad;
};

std::string hr_text(const char* what, HRESULT hr) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: 0x%08lX", what, static_cast<unsigned long>(hr));
    return buf;
}

bool make_buffer(ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap,
                 D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                 ComPtr<ID3D12Resource>& out, std::string& error, const char* what) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes ? bytes : 16;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state,
                                                    nullptr, IID_PPV_ARGS(&out));
    if (FAILED(hr)) { error = hr_text(what, hr); return false; }
    return true;
}

void barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES a,
             D3D12_RESOURCE_STATES b, UINT sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    rb.Transition.pResource = r;
    rb.Transition.StateBefore = a;
    rb.Transition.StateAfter = b;
    rb.Transition.Subresource = sub;
    cmd->ResourceBarrier(1, &rb);
}

void uav_barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r) {
    D3D12_RESOURCE_BARRIER rb = {};
    rb.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    rb.UAV.pResource = r;
    cmd->ResourceBarrier(1, &rb);
}

// Hemi-octahedral direction for frame (i, j); identical to the shader's.
void hemi_oct_decode(float u, float v, float out[3]) {
    const float ex = u * 2.0f - 1.0f, ey = v * 2.0f - 1.0f;
    const float x = 0.5f * (ex + ey), z = 0.5f * (ex - ey);
    const float y = 1.0f - std::fabs(x) - std::fabs(z);
    const float l = std::sqrt(x * x + y * y + z * z);
    out[0] = x / l; out[1] = y / l; out[2] = z / l;
}

void frame_basis(const float d[3], float right[3], float up[3]) {
    float w[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(d[1]) > 0.999f) { w[1] = 0.0f; w[2] = 1.0f; }
    // right = normalize(cross(w, d))
    float r[3] = {w[1] * d[2] - w[2] * d[1], w[2] * d[0] - w[0] * d[2], w[0] * d[1] - w[1] * d[0]};
    const float l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (int k = 0; k < 3; ++k) right[k] = r[k] / l;
    // up = cross(d, right)
    up[0] = d[1] * right[2] - d[2] * right[1];
    up[1] = d[2] * right[0] - d[0] * right[2];
    up[2] = d[0] * right[1] - d[1] * right[0];
}

// Planes of the frustum from a row-vector view_proj (clip = p * M), as
// (n, d) with dot(n, p) + d >= 0 inside. D3D depth runs 0..1, so the near
// plane is the third column alone.
void frustum_planes(const float m[16], float out[6][4]) {
    auto col = [&](int c, float v[4]) { for (int r = 0; r < 4; ++r) v[r] = m[r * 4 + c]; };
    float c0[4], c1[4], c2[4], c3[4];
    col(0, c0); col(1, c1); col(2, c2); col(3, c3);
    for (int k = 0; k < 4; ++k) {
        out[0][k] = c3[k] + c0[k];
        out[1][k] = c3[k] - c0[k];
        out[2][k] = c3[k] + c1[k];
        out[3][k] = c3[k] - c1[k];
        out[4][k] = c2[k];
        out[5][k] = c3[k] - c2[k];
    }
    for (int p = 0; p < 6; ++p) {
        const float l = std::sqrt(out[p][0] * out[p][0] + out[p][1] * out[p][1] + out[p][2] * out[p][2]);
        if (l > 0.0f) for (int k = 0; k < 4; ++k) out[p][k] /= l;
    }
}

}  // namespace

bool TreeRenderer::run_and_wait(ID3D12CommandQueue* queue, std::string& error) {
    HRESULT hr = init_cmd_->Close();
    if (FAILED(hr)) { error = hr_text("tree init Close", hr); return false; }
    ID3D12CommandList* lists[] = {init_cmd_.Get()};
    queue->ExecuteCommandLists(1, lists);
    ++init_fence_value_;
    queue->Signal(init_fence_.Get(), init_fence_value_);
    if (init_fence_->GetCompletedValue() < init_fence_value_) {
        HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        init_fence_->SetEventOnCompletion(init_fence_value_, ev);
        WaitForSingleObject(ev, INFINITE);
        CloseHandle(ev);
    }
    init_alloc_->Reset();
    init_cmd_->Reset(init_alloc_.Get(), nullptr);
    return true;
}

bool TreeRenderer::create_pipelines(std::string& error) {
    // --- draw root signature: 1 + 2 * 6 + 1 = 14 DWORDs of the 64 ---
    {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 5;
        D3D12_DESCRIPTOR_RANGE srange = {};
        srange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srange.NumDescriptors = 1;
        srange.BaseShaderRegister = 8;
        D3D12_ROOT_PARAMETER p[10] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[0].Constants.ShaderRegister = 1;
        p[0].Constants.Num32BitValues = 1;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].Descriptor.ShaderRegister = 0;
        for (int k = 0; k < 5; ++k) {
            p[2 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            p[2 + k].Descriptor.ShaderRegister = k;
        }
        p[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[7].DescriptorTable.NumDescriptorRanges = 1;
        p[7].DescriptorTable.pDescriptorRanges = &range;
        p[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[8].Descriptor.ShaderRegister = 2;
        p[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[9].DescriptorTable.NumDescriptorRanges = 1;
        p[9].DescriptorTable.pDescriptorRanges = &srange;
        for (int k = 0; k < 10; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC ssa[2] = {};
        D3D12_STATIC_SAMPLER_DESC& ss = ssa[0];
        ss.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        ss.AddressU = ss.AddressV = ss.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        ss.MaxLOD = D3D12_FLOAT32_MAX;
        ss.ShaderRegister = 0;
        ss.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC& cmp = ssa[1];
        cmp.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        cmp.AddressU = cmp.AddressV = cmp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        cmp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        cmp.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        cmp.MaxLOD = D3D12_FLOAT32_MAX;
        cmp.ShaderRegister = 1;
        cmp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 10;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 2;
        rsd.pStaticSamplers = ssa;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) {
            error = std::string("tree root signature: ") +
                    (err ? static_cast<const char*>(err->GetBufferPointer()) : "");
            return false;
        }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&draw_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (trees)", hr); return false; }

        // One root constant (where the draw's visible list starts), then the
        // draw. 24 bytes per draw.
        D3D12_INDIRECT_ARGUMENT_DESC a[2] = {};
        a[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        a[0].Constant.RootParameterIndex = 0;
        a[0].Constant.DestOffsetIn32BitValues = 0;
        a[0].Constant.Num32BitValuesToSet = 1;
        a[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        D3D12_COMMAND_SIGNATURE_DESC csd = {};
        csd.ByteStride = 24;
        csd.NumArgumentDescs = 2;
        csd.pArgumentDescs = a;
        hr = device_->CreateCommandSignature(&csd, draw_root_.Get(), IID_PPV_ARGS(&cmd_sig_));
        if (FAILED(hr)) { error = hr_text("CreateCommandSignature (trees)", hr); return false; }
    }
    // --- cull root signature: 2 * 8 = 16 DWORDs ---
    {
        D3D12_ROOT_PARAMETER p[8] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV; p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[1].Descriptor.ShaderRegister = 1;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[2].Descriptor.ShaderRegister = 3;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV; p[3].Descriptor.ShaderRegister = 7;
        for (int k = 0; k < 4; ++k) {
            p[4 + k].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            p[4 + k].Descriptor.ShaderRegister = k;
        }
        for (int k = 0; k < 8; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 8;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        HRESULT hr = D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err);
        if (FAILED(hr)) { error = "tree cull root signature"; return false; }
        hr = device_->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          IID_PPV_ARGS(&cull_root_));
        if (FAILED(hr)) { error = hr_text("CreateRootSignature (tree cull)", hr); return false; }
    }

    // --- compute ---
    const char* cs_entry[3] = {"cs_clear", "cs_cull", "cs_args"};
    ComPtr<ID3D12PipelineState>* cs_out[3] = {&clear_pso_, &cull_pso_, &args_pso_};
    for (int k = 0; k < 3; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kCullShader, cs_entry[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = cull_root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        const HRESULT hr = device_->CreateComputePipelineState(&cd, IID_PPV_ARGS(cs_out[k]->GetAddressOf()));
        if (FAILED(hr)) { error = hr_text("CreateComputePipelineState (trees)", hr); return false; }
    }

    // --- graphics ---
    auto make_pso = [&](const char* vs_e, const char* ps_e, UINT rts,
                        ComPtr<ID3D12PipelineState>& out, bool shadow = false) -> bool {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile_shader(kTreeShader, vs_e, "vs_5_1", vs, error)) return false;
        if (!compile_shader(kTreeShader, ps_e, "ps_5_1", ps, error)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = draw_root_.Get();
        pd.VS.pShaderBytecode = vs->GetBufferPointer();
        pd.VS.BytecodeLength = vs->GetBufferSize();
        pd.PS.pShaderBytecode = ps->GetBufferPointer();
        pd.PS.BytecodeLength = ps->GetBufferSize();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // Leaf cards are seen from both sides; bark tubes' back faces are
        // behind their front faces and lose the depth test.
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
            pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = rts;
        for (UINT i = 0; i < rts; ++i) pd.RTVFormats[i] = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count = 1;
        if (shadow) {
            pd.RasterizerState.SlopeScaledDepthBias = 2.0f;
            pd.RasterizerState.DepthBiasClamp = 0.002f;
        }
        const HRESULT hr = device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out));
        if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (trees)", hr); return false; }
        return true;
    };
    if (!make_pso("vs_mesh", "ps_mesh", 1, mesh_pso_)) return false;
    if (!make_pso("vs_impostor", "ps_impostor", 1, imp_pso_)) return false;
    if (!make_pso("vs_bake", "ps_bake", 2, bake_pso_)) return false;
    if (!make_pso("vs_mesh", "ps_mesh_shadow", 0, mesh_shadow_pso_, true)) return false;
    if (!make_pso("vs_impostor", "ps_impostor_shadow", 0, imp_shadow_pso_, true)) return false;
    return true;
}

bool TreeRenderer::init(ID3D12Device* device, ID3D12CommandQueue* queue,
                        const std::vector<TreeLook>& looks, ID3D12DescriptorHeap* heap, UINT slot,
                        std::string& error) {
    device_ = device;
    looks_ = looks;
    srv_heap_ = heap;
    srv_slot_ = slot;
    if (looks_.empty()) { ready_ = false; return true; }

    HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&init_alloc_));
    if (FAILED(hr)) { error = hr_text("tree allocator", hr); return false; }
    hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, init_alloc_.Get(), nullptr,
                                    IID_PPV_ARGS(&init_cmd_));
    if (FAILED(hr)) { error = hr_text("tree command list", hr); return false; }
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&init_fence_));
    if (FAILED(hr)) { error = hr_text("tree fence", hr); return false; }

    if (!create_pipelines(error)) return false;

    // --- models into one vertex and one index buffer ---
    double tg0 = startup_now_ms();
    std::vector<TreeVertex> verts;
    std::vector<uint32_t> idx;
    std::vector<ModelInfoGpu> infos;
    const uint32_t ns = static_cast<uint32_t>(looks_.size());
    for (uint32_t s = 0; s < ns; ++s) {
        for (uint32_t v = 0; v < kTreeVariants; ++v) {
            models_.push_back(build_tree_model(looks_[s], s, v));
            const TreeModel& m = models_.back();
            ModelInfoGpu mi = {};
            mi.radius = m.radius;
            mi.top = m.top;
            mi.bound = (std::max)(m.radius, 0.5f * m.top) * 1.08f;
            mi.slice = float(models_.size() - 1);
            mi.species = s;
            UINT* f[2] = {&mi.first0, &mi.first1};
            UINT* c[2] = {&mi.count0, &mi.count1};
            UINT* b[2] = {&mi.base0, &mi.base1};
            // Indices are made ABSOLUTE here and every draw passes base vertex
            // 0. On this driver SV_VertexID does not include BaseVertexLocation,
            // so with per-model bases every model read model 0's vertices
            // through its own indices: LOD 0 trunks happened to line up (all
            // species build a 10-sided trunk first) and hid it, LOD 1 trunks
            // twisted, caps joined to tubes drew lines to the middle of the
            // screen, and every impostor was baked from an oak. mesh.hlsl's
            // terrain never met it because its indices were absolute already.
            for (int l = 0; l < 2; ++l) {
                *f[l] = static_cast<UINT>(idx.size());
                *c[l] = static_cast<UINT>(m.lod[l].idx.size());
                *b[l] = 0;
                const uint32_t vb = static_cast<uint32_t>(verts.size());
                verts.insert(verts.end(), m.lod[l].v.begin(), m.lod[l].v.end());
                for (size_t q = 0; q < m.lod[l].idx.size(); ++q) idx.push_back(vb + m.lod[l].idx[q]);
            }
            infos.push_back(mi);
        }
    }
    // The impostor quad: four placeholder vertices on a multiple of four, so
    // SV_VertexID & 3 is the corner whatever the driver does with bases.
    while (verts.size() % 4) verts.push_back(TreeVertex());
    quad_base_ = static_cast<UINT>(verts.size());
    for (int k = 0; k < 4; ++k) verts.push_back(TreeVertex());
    quad_first_ = static_cast<UINT>(idx.size());
    const uint32_t q[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; ++k) idx.push_back(quad_base_ + q[k]);
    total_vertices_ = verts.size();
    total_indices_ = idx.size();
    {
        // The vertex shader drops caps and cards by collapsing every vertex
        // of them to one point. A triangle mixing kinds would be stretched
        // to that point instead: a long line to the middle of the screen.
        size_t mixed = 0, bad_index = 0;
        for (size_t mi = 0; mi < models_.size(); ++mi) {
            for (int l = 0; l < 2; ++l) {
                const TreeMesh& tm = models_[mi].lod[l];
                for (size_t t = 0; t + 2 < tm.idx.size(); t += 3) {
                    const uint32_t a = tm.idx[t], b2 = tm.idx[t + 1], c = tm.idx[t + 2];
                    if (a >= tm.v.size() || b2 >= tm.v.size() || c >= tm.v.size()) { ++bad_index; continue; }
                    if (tm.v[a].kind != tm.v[b2].kind || tm.v[a].kind != tm.v[c].kind) ++mixed;
                }
            }
        }
        char line[160];
        std::snprintf(line, sizeof line, "trees: %zu triangles mixing vertex kinds, %zu out-of-range indices",
                      mixed, bad_index);
        startup_log().push_back(std::make_pair(std::string(line), 0.0));
    }
    {
        char line[160];
        std::snprintf(line, sizeof line, "trees: %zu models, %zu vertices, %zu indices, generated in",
                      models_.size(), total_vertices_, total_indices_);
        startup_log().push_back(std::make_pair(std::string(line), startup_now_ms() - tg0));
    }

    std::vector<LookGpu> lk(ns);
    for (uint32_t s = 0; s < ns; ++s) {
        for (int k = 0; k < 3; ++k) { lk[s].bark[k] = looks_[s].bark[k]; lk[s].leaf[k] = looks_[s].leaf[k]; }
        // Narrow leaves for the weeping form; the shader reads this, the
        // generator chose the form.
        lk[s].leaf_kind = looks_[s].form == TreeForm::Weeping ? 1.0f : 0.0f;
    }

    // Draw slots: per model LOD 0 and LOD 1, then the impostor draw.
    const UINT nm = static_cast<UINT>(models_.size());
    draw_count_ = nm * 2 + 1;
    std::vector<UINT> slots(size_t(draw_count_) * 2);
    UINT off = 0;
    for (UINT d = 0; d < draw_count_; ++d) {
        const UINT cap = d == nm * 2 ? kCapImp : ((d & 1) == 0 ? kCapLod0 : kCapLod1);
        slots[d * 2] = off;
        slots[d * 2 + 1] = cap;
        off += cap;
    }
    visible_total_ = off;

    // --- buffers ---
    const D3D12_RESOURCE_FLAGS uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    auto upload_static = [&](const void* data, size_t bytes, ComPtr<ID3D12Resource>& dst,
                             const char* what) -> bool {
        if (!make_buffer(device_, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST, dst, error, what)) return false;
        ComPtr<ID3D12Resource> st;
        if (!make_buffer(device_, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, st, error, what)) return false;
        void* p = nullptr;
        st->Map(0, nullptr, &p);
        std::memcpy(p, data, bytes);
        st->Unmap(0, nullptr);
        init_cmd_->CopyBufferRegion(dst.Get(), 0, st.Get(), 0, bytes);
        barrier(init_cmd_.Get(), dst.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        staging_.push_back(st);
        return true;
    };
    if (!upload_static(verts.data(), verts.size() * sizeof(TreeVertex), vb_, "tree vertices")) return false;
    if (!upload_static(idx.data(), idx.size() * 4, ib_, "tree indices")) return false;
    if (!upload_static(infos.data(), infos.size() * sizeof(ModelInfoGpu), model_buf_, "tree models")) return false;
    if (!upload_static(lk.data(), lk.size() * sizeof(LookGpu), look_buf_, "tree looks")) return false;
    if (!upload_static(slots.data(), slots.size() * 4, slot_buf_, "tree slots")) return false;
    // The index buffer is read by the input assembler, not a shader.
    barrier(init_cmd_.Get(), ib_.Get(),
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_INDEX_BUFFER);

    // Root UAVs: ALLOW_UNORDERED_ACCESS, or the GPU hangs (CLAUDE.md).
    if (!make_buffer(device_, UINT64(visible_total_) * 4, D3D12_HEAP_TYPE_DEFAULT, uav,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, visible_, error, "tree visible")) return false;
    if (!make_buffer(device_, UINT64(draw_count_) * 4, D3D12_HEAP_TYPE_DEFAULT, uav,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, counters_, error, "tree counters")) return false;
    if (!make_buffer(device_, UINT64(draw_count_) * 24, D3D12_HEAP_TYPE_DEFAULT, uav,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, args_, error, "tree args")) return false;
    if (!make_buffer(device_, 256, D3D12_HEAP_TYPE_DEFAULT, uav,
                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS, stats_, error, "tree stats")) return false;
    for (int f = 0; f < 2; ++f) {
        if (!make_buffer(device_, UINT64(kMaxInstances) * sizeof(TreeGpuInstance), D3D12_HEAP_TYPE_UPLOAD,
                         D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, inst_up_[f],
                         error, "tree instances")) return false;
        inst_up_[f]->Map(0, nullptr, &inst_cpu_[f]);
        if (!make_buffer(device_, kCbStride * kCbPerFrame, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, frame_cb_[f], error, "tree frame cb")) return false;
        frame_cb_[f]->Map(0, nullptr, &frame_cb_cpu_[f]);
    }

    // --- the impostor atlas ---
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = kAtlas;
        td.Height = kAtlas;
        td.DepthOrArraySize = static_cast<UINT16>(nm);
        td.MipLevels = kAtlasMips;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ComPtr<ID3D12Resource>* t[2] = {&atlas_albedo_, &atlas_normal_};
        for (int k = 0; k < 2; ++k) {
            hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                  D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                                  IID_PPV_ARGS(t[k]->GetAddressOf()));
            if (FAILED(hr)) { error = hr_text("tree atlas", hr); return false; }
        }
        atlas_bytes_ = size_t(kAtlas) * kAtlas * 4 * nm * 2 * 4 / 3;

        const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += SIZE_T(srv_slot_) * inc;
        for (int k = 0; k < 2; ++k) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2DArray.MipLevels = kAtlasMips;
            sd.Texture2DArray.ArraySize = nm;
            device_->CreateShaderResourceView(t[k]->Get(), &sd, h);
            h.ptr += inc;
        }
    }

    tg0 = startup_now_ms();
    if (!run_and_wait(queue, error)) return false;
    startup_log().push_back(std::make_pair(std::string("trees: upload"), startup_now_ms() - tg0));
    tg0 = startup_now_ms();
    if (!bake(queue, error)) return false;
    startup_log().push_back(std::make_pair(std::string("trees: impostor bake + mips"), startup_now_ms() - tg0));
    staging_.clear();
    ready_ = true;
    return true;
}

bool TreeRenderer::bake(ID3D12CommandQueue* queue, std::string& error) {
    LARGE_INTEGER f0, t0, t1;
    QueryPerformanceFrequency(&f0);
    QueryPerformanceCounter(&t0);
    const UINT nm = static_cast<UINT>(models_.size());

    // RTVs for mip 0 of every slice, and one depth buffer the bake reuses.
    ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = nm * 2;
    HRESULT hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap));
    if (FAILED(hr)) { error = hr_text("bake rtv heap", hr); return false; }
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 1;
    hr = device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsv_heap));
    if (FAILED(hr)) { error = hr_text("bake dsv heap", hr); return false; }
    const UINT rinc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT m = 0; m < nm; ++m) {
        for (int k = 0; k < 2; ++k) {
            D3D12_RENDER_TARGET_VIEW_DESC rv = {};
            rv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rv.Texture2DArray.FirstArraySlice = m;
            rv.Texture2DArray.ArraySize = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            h.ptr += SIZE_T(m * 2 + k) * rinc;
            device_->CreateRenderTargetView(k == 0 ? atlas_albedo_.Get() : atlas_normal_.Get(), &rv, h);
        }
    }
    ComPtr<ID3D12Resource> depth;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kAtlas; rd.Height = kAtlas; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_D32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D32_FLOAT;
        cv.DepthStencil.Depth = 1.0f;
        hr = device_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depth));
        if (FAILED(hr)) { error = hr_text("bake depth", hr); return false; }
        device_->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    // One constant block per (model, frame).
    const UINT draws = nm * kFrames * kFrames;
    ComPtr<ID3D12Resource> cbs;
    if (!make_buffer(device_, UINT64(draws) * kCbStride, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_GENERIC_READ, cbs, error, "bake cbs")) return false;
    unsigned char* cbp = nullptr;
    cbs->Map(0, nullptr, reinterpret_cast<void**>(&cbp));

    ID3D12GraphicsCommandList* cmd = init_cmd_.Get();
    cmd->SetGraphicsRootSignature(draw_root_.Get());
    cmd->SetPipelineState(bake_pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv.SizeInBytes = static_cast<UINT>(total_indices_ * 4);
    ibv.Format = DXGI_FORMAT_R32_UINT;
    cmd->IASetIndexBuffer(&ibv);
    cmd->SetGraphicsRoot32BitConstant(0, 0, 0);
    cmd->SetGraphicsRootShaderResourceView(2, vb_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(3, vb_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(4, vb_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(5, model_buf_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(6, look_buf_->GetGPUVirtualAddress());
    ID3D12DescriptorHeap* heaps[] = {srv_heap_};
    cmd->SetDescriptorHeaps(1, heaps);
    {
        const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        cmd->SetGraphicsRootDescriptorTable(9, g);
        g.ptr += UINT64(srv_slot_) * inc;
        cmd->SetGraphicsRootDescriptorTable(7, g);
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dsv_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_RECT sc = {0, 0, LONG(kAtlas), LONG(kAtlas)};
    cmd->RSSetScissorRects(1, &sc);

    UINT di = 0;
    for (UINT m = 0; m < nm; ++m) {
        D3D12_CPU_DESCRIPTOR_HANDLE rt[2];
        for (int k = 0; k < 2; ++k) {
            rt[k] = rtv_heap->GetCPUDescriptorHandleForHeapStart();
            rt[k].ptr += SIZE_T(m * 2 + k) * rinc;
        }
        cmd->OMSetRenderTargets(2, rt, FALSE, &dsv);
        const float zero[4] = {0, 0, 0, 0};
        cmd->ClearRenderTargetView(rt[0], zero, 0, nullptr);
        cmd->ClearRenderTargetView(rt[1], zero, 0, nullptr);
        cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        const TreeModel& tm = models_[m];
        const float bound = (std::max)(tm.radius, 0.5f * tm.top) * 1.08f;
        for (UINT j = 0; j < kFrames; ++j) {
            for (UINT i = 0; i < kFrames; ++i, ++di) {
                TreeFrame tf = {};
                tf.instance_count = m;   // vs_bake reads the model from here
                float d[3], r[3], u[3];
                hemi_oct_decode((i + 0.5f) / kFrames, (j + 0.5f) / kFrames, d);
                frame_basis(d, r, u);
                for (int k = 0; k < 3; ++k) { tf.bake_dir[k] = d[k]; tf.bake_right[k] = r[k]; tf.bake_up[k] = u[k]; }
                tf.bake_r = bound;
                tf.bake_centre_y = 0.5f * tm.top;
                std::memcpy(cbp + size_t(di) * kCbStride, &tf, sizeof tf);
                D3D12_VIEWPORT vp = {};
                vp.TopLeftX = float(i * kFramePx);
                vp.TopLeftY = float(j * kFramePx);
                vp.Width = float(kFramePx);
                vp.Height = float(kFramePx);
                vp.MaxDepth = 1.0f;
                cmd->RSSetViewports(1, &vp);
                cmd->SetGraphicsRootConstantBufferView(1, cbs->GetGPUVirtualAddress() + UINT64(di) * kCbStride);
                cmd->SetGraphicsRootConstantBufferView(8, cbs->GetGPUVirtualAddress());
                // LOD 0 of this model.
                // LOD 0 of this model; indices are absolute (see init).
                size_t f = 0;
                for (UINT q = 0; q < m; ++q) f += models_[q].lod[0].idx.size() + models_[q].lod[1].idx.size();
                cmd->DrawIndexedInstanced(UINT(tm.lod[0].idx.size()), 1, UINT(f), 0, 0);
            }
        }
    }
    // The bake's mip 0 to a readback buffer.
    D3D12_RESOURCE_DESC ad = atlas_albedo_->GetDesc();
    const UINT subs = kAtlasMips * nm;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(subs);
    std::vector<UINT> rows(subs);
    std::vector<UINT64> rowbytes(subs);
    UINT64 total = 0;
    device_->GetCopyableFootprints(&ad, 0, subs, 0, fp.data(), rows.data(), rowbytes.data(), &total);
    ComPtr<ID3D12Resource> rb[2], up[2];
    ID3D12Resource* atl[2] = {atlas_albedo_.Get(), atlas_normal_.Get()};
    for (int k = 0; k < 2; ++k) {
        if (!make_buffer(device_, total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_COPY_DEST, rb[k], error, "atlas readback")) return false;
        barrier(cmd, atl[k], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT m = 0; m < nm; ++m) {
            const UINT sub = m * kAtlasMips;   // mip 0 of slice m
            D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
            src.pResource = atl[k];
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = sub;
            dst.pResource = rb[k].Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint = fp[sub];
            cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        barrier(cmd, atl[k], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    }
    if (!run_and_wait(queue, error)) return false;
    cbs->Unmap(0, nullptr);

    // The mip chain, on the CPU: a 2x2 average of premultiplied values. A
    // frame's edges are multiples of 2^4 texels, so no average ever reaches
    // across into the next frame's view.
    //
    // Built in ordinary memory and copied to the upload heap in one go. The
    // first version averaged each level by READING the level above back out
    // of the upload buffer, which is write-combined memory: uncached, and
    // the whole bake took 20 s instead of a fraction of one.
    std::vector<unsigned char> work(static_cast<size_t>(total));
    for (int k = 0; k < 2; ++k) {
        if (!make_buffer(device_, total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                         D3D12_RESOURCE_STATE_GENERIC_READ, up[k], error, "atlas upload")) return false;
        unsigned char* src = nullptr;
        unsigned char* upload = nullptr;
        unsigned char* dst = work.data();
        rb[k]->Map(0, nullptr, reinterpret_cast<void**>(&src));
        up[k]->Map(0, nullptr, reinterpret_cast<void**>(&upload));
        for (UINT m = 0; m < nm; ++m) {
            const UINT s0 = m * kAtlasMips;
            for (UINT y = 0; y < kAtlas; ++y) {
                std::memcpy(dst + fp[s0].Offset + y * fp[s0].Footprint.RowPitch,
                            src + fp[s0].Offset + y * fp[s0].Footprint.RowPitch, kAtlas * 4);
            }
            for (UINT mip = 1; mip < kAtlasMips; ++mip) {
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& a = fp[s0 + mip - 1];
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& b = fp[s0 + mip];
                const UINT w = b.Footprint.Width, h = b.Footprint.Height;
                for (UINT y = 0; y < h; ++y) {
                    for (UINT x = 0; x < w; ++x) {
                        for (int c = 0; c < 4; ++c) {
                            const unsigned char* p = dst + a.Offset;
                            const UINT pitch = a.Footprint.RowPitch;
                            const int sum = p[(2 * y) * pitch + (2 * x) * 4 + c] +
                                            p[(2 * y) * pitch + (2 * x + 1) * 4 + c] +
                                            p[(2 * y + 1) * pitch + (2 * x) * 4 + c] +
                                            p[(2 * y + 1) * pitch + (2 * x + 1) * 4 + c];
                            dst[b.Offset + y * b.Footprint.RowPitch + x * 4 + c] =
                                static_cast<unsigned char>((sum + 2) / 4);
                        }
                    }
                }
            }
        }
        std::memcpy(upload, work.data(), work.size());
        rb[k]->Unmap(0, nullptr);
        up[k]->Unmap(0, nullptr);
        for (UINT s = 0; s < subs; ++s) {
            D3D12_TEXTURE_COPY_LOCATION src2 = {}, dst2 = {};
            src2.pResource = up[k].Get();
            src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src2.PlacedFootprint = fp[s];
            dst2.pResource = atl[k];
            dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst2.SubresourceIndex = s;
            init_cmd_->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);
        }
        barrier(init_cmd_.Get(), atl[k], D3D12_RESOURCE_STATE_COPY_DEST,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (!run_and_wait(queue, error)) return false;
    QueryPerformanceCounter(&t1);
    bake_ms_ = 1000.0 * double(t1.QuadPart - t0.QuadPart) / double(f0.QuadPart);
    return true;
}

void TreeRenderer::set_instances(const std::vector<TreeGpuInstance>& inst) {
    cpu_instances_ = inst;
    if (cpu_instances_.size() > kMaxInstances) cpu_instances_.resize(kMaxInstances);
    ++cpu_version_;
}

namespace {
void fill_common(TreeFrame& tf, UINT count, UINT models, UINT quad_first, UINT quad_base,
                 const TreeDrawSettings& s) {
    tf.sun_dir[0] = 0.45f; tf.sun_dir[1] = 0.78f; tf.sun_dir[2] = 0.35f;
    tf.max_dist = s.max_dist;
    tf.lod0_dist = (std::min)(s.lod0_dist, s.max_dist);
    tf.lod1_dist = (std::min)(s.lod1_dist, s.max_dist);
    tf.instance_count = count;
    tf.model_count = models;
    tf.frames = kFrames;
    tf.quad_first = quad_first;
    tf.quad_base = quad_base;
    tf.wind = s.wind;
    tf.cut_height = kCutHeight;
    tf.debug = s.debug;
}
}  // namespace

void TreeRenderer::record(ID3D12GraphicsCommandList* cmd, UINT frame, const TreeFrameInputs& in,
                          const TreeDrawSettings& s) {
    if (!ready_) return;
    TreeFrame tf = {};
    std::memcpy(tf.view_proj, in.view_proj, sizeof tf.view_proj);
    for (int k = 0; k < 3; ++k) {
        tf.cam_pos[k] = in.cam_pos[k];
        tf.cam_right[k] = in.cam_right[k];
        tf.cam_up[k] = in.cam_up[k];
        tf.cam_fwd[k] = in.cam_fwd[k];
    }
    tf.time = in.time;
    tf.far_plane = in.far_plane;
    frustum_planes(in.view_proj, tf.frustum);
    fill_common(tf, static_cast<UINT>(cpu_instances_.size()), static_cast<UINT>(models_.size()),
                quad_first_, quad_base_, s);
    tf.shadow_pass = 0.0f;
    cull_and_draw(cmd, frame, 0, &tf, false, in.shadow_cb);
}

void TreeRenderer::record_shadow(ID3D12GraphicsCommandList* cmd, UINT frame, UINT cascade,
                                 const TreeShadowInputs& in, const TreeDrawSettings& s) {
    if (!ready_) return;
    TreeFrame tf = {};
    std::memcpy(tf.view_proj, in.view_proj, sizeof tf.view_proj);
    std::memcpy(tf.frustum, in.planes, sizeof tf.frustum);
    for (int k = 0; k < 3; ++k) {
        // The camera picks the level; the light's basis turns impostor cards
        // to face the sun.
        tf.cam_pos[k] = in.cam_pos[k];
        tf.cam_right[k] = in.light_right[k];
        tf.cam_up[k] = in.light_up[k];
    }
    tf.time = in.time;
    tf.far_plane = in.far_plane;
    fill_common(tf, static_cast<UINT>(cpu_instances_.size()), static_cast<UINT>(models_.size()),
                quad_first_, quad_base_, s);
    // 1 + the cascade: the cull and the caster shaders both read it.
    tf.shadow_pass = 1.0f + float(cascade);
    cull_and_draw(cmd, frame, 1 + cascade, &tf, true, 0);
}

void TreeRenderer::cull_and_draw(ID3D12GraphicsCommandList* cmd, UINT frame, UINT cb_index,
                                 const void* frame_constants, bool shadow,
                                 D3D12_GPU_VIRTUAL_ADDRESS shadow_cb) {
    const UINT f = frame & 1u;
    // Instances go up once per frame in flight, whichever pass runs first --
    // the shadow cascades do, now.
    if (inst_version_[f] != cpu_version_) {
        if (!cpu_instances_.empty())
            std::memcpy(inst_cpu_[f], cpu_instances_.data(), cpu_instances_.size() * sizeof(TreeGpuInstance));
        inst_version_[f] = cpu_version_;
    }
    const UINT count = static_cast<UINT>(cpu_instances_.size());
    std::memcpy(static_cast<unsigned char*>(frame_cb_cpu_[f]) + size_t(cb_index) * kCbStride,
                frame_constants, sizeof(TreeFrame));
    const D3D12_GPU_VIRTUAL_ADDRESS cb = frame_cb_[f]->GetGPUVirtualAddress() + UINT64(cb_index) * kCbStride;
    const D3D12_GPU_VIRTUAL_ADDRESS inst = inst_up_[f]->GetGPUVirtualAddress();

    // --- cull ---
    cmd->SetComputeRootSignature(cull_root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb);
    cmd->SetComputeRootShaderResourceView(1, inst);
    cmd->SetComputeRootShaderResourceView(2, model_buf_->GetGPUVirtualAddress());
    cmd->SetComputeRootShaderResourceView(3, slot_buf_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(4, counters_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(5, visible_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(6, args_->GetGPUVirtualAddress());
    cmd->SetComputeRootUnorderedAccessView(7, stats_->GetGPUVirtualAddress());
    cmd->SetPipelineState(clear_pso_.Get());
    cmd->Dispatch((draw_count_ + 63) / 64, 1, 1);
    uav_barrier(cmd, counters_.Get());
    if (count > 0) {
        cmd->SetPipelineState(cull_pso_.Get());
        cmd->Dispatch((count + 63) / 64, 1, 1);
    }
    uav_barrier(cmd, counters_.Get());
    cmd->SetPipelineState(args_pso_.Get());
    cmd->Dispatch((draw_count_ + 63) / 64, 1, 1);
    barrier(cmd, args_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    barrier(cmd, visible_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // --- draw ---
    cmd->SetGraphicsRootSignature(draw_root_.Get());
    ID3D12DescriptorHeap* heaps[] = {srv_heap_};
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetGraphicsRootConstantBufferView(1, cb);
    cmd->SetGraphicsRootShaderResourceView(2, vb_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(3, inst);
    cmd->SetGraphicsRootShaderResourceView(4, visible_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(5, model_buf_->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(6, look_buf_->GetGPUVirtualAddress());
    {
        const UINT inc = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_GPU_DESCRIPTOR_HANDLE g = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        cmd->SetGraphicsRootDescriptorTable(9, g);           // the shadow map
        g.ptr += UINT64(srv_slot_) * inc;
        cmd->SetGraphicsRootDescriptorTable(7, g);           // the atlases
    }
    // The shadow pass never samples the shadow constants; bind something valid.
    cmd->SetGraphicsRootConstantBufferView(8, shadow_cb ? shadow_cb : cb);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv.SizeInBytes = static_cast<UINT>(total_indices_ * 4);
    ibv.Format = DXGI_FORMAT_R32_UINT;
    cmd->IASetIndexBuffer(&ibv);
    const UINT nm = static_cast<UINT>(models_.size());
    cmd->SetPipelineState(shadow ? mesh_shadow_pso_.Get() : mesh_pso_.Get());
    cmd->ExecuteIndirect(cmd_sig_.Get(), nm * 2, args_.Get(), 0, nullptr, 0);
    cmd->SetPipelineState(shadow ? imp_shadow_pso_.Get() : imp_pso_.Get());
    cmd->ExecuteIndirect(cmd_sig_.Get(), 1, args_.Get(), UINT64(nm) * 2 * 24, nullptr, 0);

    barrier(cmd, args_.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(cmd, visible_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool TreeRenderer::read_counts(ID3D12CommandQueue* queue, std::vector<uint32_t>& per_draw,
                               uint32_t& overflow, std::string& error) {
    if (!ready_) return false;
    ComPtr<ID3D12Resource> rb;
    const UINT64 bytes = UINT64(draw_count_) * 4 + 16;
    if (!make_buffer(device_, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                     D3D12_RESOURCE_STATE_COPY_DEST, rb, error, "tree count readback")) return false;
    barrier(init_cmd_.Get(), counters_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(init_cmd_.Get(), stats_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    init_cmd_->CopyBufferRegion(rb.Get(), 0, counters_.Get(), 0, UINT64(draw_count_) * 4);
    init_cmd_->CopyBufferRegion(rb.Get(), UINT64(draw_count_) * 4, stats_.Get(), 0, 4);
    barrier(init_cmd_.Get(), counters_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barrier(init_cmd_.Get(), stats_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!run_and_wait(queue, error)) return false;
    uint32_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    per_draw.assign(p, p + draw_count_);
    overflow = p[draw_count_];
    rb->Unmap(0, nullptr);
    return true;
}

void TreeRenderer::shutdown() {
    for (int f = 0; f < 2; ++f) {
        if (inst_up_[f] && inst_cpu_[f]) inst_up_[f]->Unmap(0, nullptr);
        if (frame_cb_[f] && frame_cb_cpu_[f]) frame_cb_[f]->Unmap(0, nullptr);
        inst_cpu_[f] = frame_cb_cpu_[f] = nullptr;
    }
    ready_ = false;
}

}  // namespace engine
