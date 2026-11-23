#include "engine/render/rain.h"
#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace engine {

namespace {
// Matches cbuffer RainConstants in rain.hlsl: 256 bytes.
struct RainConstants {
    float view_proj[16];
    float inv_view_proj[16];
    float cam_pos[3];   float time;
    float intensity;    float fall;      float box;    float height;
    float wind[2];      float exposure;  float pixel_angle;
    float light[3];     float near_plane;
    float canopy[4];
    float inv_size[2];  float far_plane; float splash_count;
    float near_drops;   float debug;   float drip;   float pad0;
    float hf[4];
};
static_assert(sizeof(RainConstants) == 256, "RainConstants must match rain.hlsl");
const char* kRainShader = "engine/render/shaders/rain.hlsl";
}  // namespace

bool Rain::init(ID3D12Device* device, DXGI_FORMAT target, unsigned canopy_offset, unsigned hf_offset,
                std::string& error) {
    ready_ = false;
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
            error = "rain constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_DESCRIPTOR_RANGE r[3] = {};
        r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r[0].NumDescriptors = 1;
        r[0].BaseShaderRegister = 0;
        r[0].OffsetInDescriptorsFromTableStart = 0;
        r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r[1].NumDescriptors = 1;
        r[1].BaseShaderRegister = 1;
        r[1].OffsetInDescriptorsFromTableStart = canopy_offset;
        r[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r[2].NumDescriptors = 1;
        r[2].BaseShaderRegister = 2;
        r[2].OffsetInDescriptorsFromTableStart = hf_offset;
        D3D12_ROOT_PARAMETER p[2] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].DescriptorTable.NumDescriptorRanges = 3;
        p[1].DescriptorTable.pDescriptorRanges = r;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 2;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "rain root signature";
            return false;
        }
    }
    const char* vs_e[2] = {"vs_streak", "vs_splash"};
    const char* ps_e[2] = {"ps_streak", "ps_splash"};
    ComPtr<ID3D12PipelineState>* out[2] = {&streak_pso_, &splash_pso_};
    for (int k = 0; k < 2; ++k) {
        ComPtr<ID3DBlob> vs, ps;
        if (!compile_shader(kRainShader, vs_e[k], "vs_5_1", vs, error)) return false;
        if (!compile_shader(kRainShader, ps_e[k], "ps_5_1", ps, error)) return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = root_.Get();
        pd.VS.pShaderBytecode = vs->GetBufferPointer();
        pd.VS.BytecodeLength = vs->GetBufferSize();
        pd.PS.pShaderBytecode = ps->GetBufferPointer();
        pd.PS.BytecodeLength = ps->GetBufferSize();
        pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable = TRUE;
        D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        b.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D12_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D12_BLEND_ZERO;
        b.DestBlendAlpha = D3D12_BLEND_ONE;
        b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        // RGB only: the scene's alpha is the post chain's debug mark.
        b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                                  D3D12_COLOR_WRITE_ENABLE_BLUE;
        pd.SampleMask = UINT_MAX;
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0] = target;
        pd.SampleDesc.Count = 1;
        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(out[k]->GetAddressOf())))) {
            error = std::string("rain pso ") + vs_e[k];
            return false;
        }
    }
    ready_ = true;
    return true;
}

void Rain::record(ID3D12GraphicsCommandList* cmd, unsigned frame, const Frame& f,
                  D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_GPU_DESCRIPTOR_HANDLE table) {
    if (!ready_ || (f.intensity <= 0.001f && f.drip <= 0.001f)) return;
    RainConstants c = {};
    std::memcpy(c.view_proj, f.view_proj, sizeof c.view_proj);
    std::memcpy(c.inv_view_proj, f.inv_view_proj, sizeof c.inv_view_proj);
    for (int k = 0; k < 3; ++k) {
        c.cam_pos[k] = f.cam_pos[k];
        c.light[k] = f.light[k];
    }
    c.time = f.time;
    c.intensity = f.intensity;
    c.drip = f.drip;
    c.fall = 8.5f;          // terminal speed of a ~2 mm drop
    c.box = 22.0f;
    c.height = 12.0f;
    c.wind[0] = f.wind[0];
    c.wind[1] = f.wind[1];
    c.exposure = 0.045f;    // a streak is the drop's path over this long
    c.pixel_angle = 2.0f * f.tan_half_fov / float(f.height ? f.height : 1);
    c.near_plane = f.near_plane;
    c.far_plane = f.far_plane;
    for (int k = 0; k < 4; ++k) c.canopy[k] = f.canopy[k];
    for (int k = 0; k < 4; ++k) c.hf[k] = f.hf[k];
    c.inv_size[0] = 1.0f / float(f.width ? f.width : 1);
    c.inv_size[1] = 1.0f / float(f.height ? f.height : 1);
    c.splash_count = float(kMaxSplashes);
    c.near_drops = float(kNearDrops);
    // BIOMES_RAIN_DBG: 1 splashes drawn big and opaque, 2 also placed on the
    // heightfield map's height with no checks -- which check drops them.
    static const float dbg = std::getenv("BIOMES_RAIN_DBG") ? float(std::atof(std::getenv("BIOMES_RAIN_DBG"))) : 0.0f;
    c.debug = dbg;
    std::memcpy(cb_cpu_[frame & 1u], &c, sizeof c);

    D3D12_VIEWPORT vp = {};
    vp.Width = float(f.width);
    vp.Height = float(f.height);
    vp.MaxDepth = 1.0f;
    D3D12_RECT sc = {0, 0, LONG(f.width), LONG(f.height)};
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->SetGraphicsRootSignature(root_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb_[frame & 1u]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(1, table);
    cmd->SetPipelineState(splash_pso_.Get());
    cmd->DrawInstanced(kMaxSplashes * 6u, 1, 0, 0);
    cmd->SetPipelineState(streak_pso_.Get());
    cmd->DrawInstanced((kNearDrops + kFarDrops) * 6u, 1, 0, 0);
}

}  // namespace engine
