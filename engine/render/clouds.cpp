#include "engine/render/clouds.h"
#include "engine/render/shader_compile.h"
#include "worldgen/assign/json_lite.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace engine {

namespace {
// Matches cbuffer CloudConstants in clouds.hlsl: 288 bytes.
struct CloudConstants {
    float inv_view_proj[16];
    float prev_view_proj[16];
    float cam_pos[3];     float time;
    float key_dir[3];     float cam_alt_km;
    float key_lux_top[3]; float frame;
    float sky_mean[3];    float coverage;
    float ground[3];      float cloud_type;
    float drift[2];       float wind_dir[2];
    float base_alt;       float sigma;         float base_scale;   float detail_scale;
    float res[2];         float inv_res[2];
    float map_extent;     float history_valid; float precip;       float shear;
    unsigned prev_index;  float haze_km;       float detail_amount; float pad0;
    float shadow_map[4];
};
static_assert(sizeof(CloudConstants) == 304, "CloudConstants must match clouds.hlsl");

const char* kCloudShader = "engine/render/shaders/clouds.hlsl";
const UINT kWeatherSize = 512;
const D3D12_RESOURCE_STATES kNps = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
const D3D12_RESOURCE_STATES kRead = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const D3D12_RESOURCE_STATES kUav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

float smooth01(float a, float b, float x) {
    float t = (x - a) / (b - a);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return t * t * (3.0f - 2.0f * t);
}
}  // namespace

bool Clouds::load(const char* archetypes_path, std::string& error) {
    try {
        const jsonlite::Value root = jsonlite::parse_file(archetypes_path);
        if (!root.has("clouds")) return true;
        const jsonlite::Value& c = root.at("clouds");
        CloudSettings& s = settings_;
        if (c.has("base_alt")) s.base_alt = float(c.number("base_alt"));
        if (c.has("sigma")) s.sigma = float(c.number("sigma"));
        if (c.has("base_scale")) s.base_scale = float(c.number("base_scale"));
        if (c.has("detail_scale")) s.detail_scale = float(c.number("detail_scale"));
        if (c.has("shear")) s.shear = float(c.number("shear"));
        if (c.has("haze_km")) s.haze_km = float(c.number("haze_km"));
        if (c.has("detail_amount")) s.detail_amount = float(c.number("detail_amount"));
        if (c.has("drift_factor")) s.drift_factor = float(c.number("drift_factor"));
    } catch (const std::exception& e) {
        error = std::string("archetypes.json clouds: ") + e.what();
        return false;
    }
    return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE Clouds::cpu(UINT slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = heap_->GetCPUDescriptorHandleForHeapStart();
    h.ptr += SIZE_T(slot) * inc_;
    return h;
}
D3D12_GPU_DESCRIPTOR_HANDLE Clouds::gpu(UINT slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = heap_->GetGPUDescriptorHandleForHeapStart();
    h.ptr += UINT64(slot) * inc_;
    return h;
}

namespace {
bool make_tex(ID3D12Device* dev, Microsoft::WRL::ComPtr<ID3D12Resource>& out, D3D12_RESOURCE_DIMENSION dim,
              UINT w, UINT h, UINT d, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = dim;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = UINT16(d);
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    out.Reset();
    return SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&out)));
}
void view2d(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE srv,
            D3D12_CPU_DESCRIPTOR_HANDLE uav) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    dev->CreateShaderResourceView(r, &sd, srv);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev->CreateUnorderedAccessView(r, nullptr, &ud, uav);
}
void view3d(ID3D12Device* dev, ID3D12Resource* r, DXGI_FORMAT fmt, UINT depth, D3D12_CPU_DESCRIPTOR_HANDLE srv,
            D3D12_CPU_DESCRIPTOR_HANDLE uav) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = fmt;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture3D.MipLevels = 1;
    dev->CreateShaderResourceView(r, &sd, srv);
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = fmt;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
    ud.Texture3D.WSize = depth;
    dev->CreateUnorderedAccessView(r, nullptr, &ud, uav);
}
}  // namespace

bool Clouds::init(ID3D12Device* device, ID3D12DescriptorHeap* heap, ID3D12Resource* transmittance,
                  ID3D12Resource* sky_view, UINT width, UINT height, std::string& error) {
    ready_ = false;
    dev_ = device;
    heap_ = heap;
    inc_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const DXGI_FORMAT rgba8 = DXGI_FORMAT_R8G8B8A8_UNORM, f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!make_tex(device, base_, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 128, 128, 128, rgba8, kUav) ||
        !make_tex(device, detail_, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 32, 32, 32, rgba8, kUav) ||
        !make_tex(device, weather_, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kWeatherSize, kWeatherSize, 1, f16, kUav) ||
        !make_tex(device, shadow_, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kShadowSize, kShadowSize, 1,
                  DXGI_FORMAT_R16_FLOAT, kUav)) {
        error = "cloud volumes";
        return false;
    }
    noise_done_ = false;
    view3d(device, base_.Get(), rgba8, 128, cpu(kSrvBase + 0), cpu(kUavBase + 0));
    view3d(device, detail_.Get(), rgba8, 32, cpu(kSrvBase + 1), cpu(kUavBase + 1));
    view2d(device, weather_.Get(), f16, cpu(kSrvBase + 2), cpu(kUavBase + 2));
    view2d(device, shadow_.Get(), DXGI_FORMAT_R16_FLOAT, cpu(kShadowSrv), cpu(kShadowUav));
    shadow_readable_ = false;
    device->CreateShaderResourceView(transmittance, nullptr, cpu(kSrvBase + 3));
    device->CreateShaderResourceView(sky_view, nullptr, cpu(kSrvBase + 4));
    for (int f = 0; f < 2; ++f) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = 512;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&cb_[f])))) {
            error = "cloud constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
    }
    {
        D3D12_DESCRIPTOR_RANGE rs = {}, ru = {};
        rs.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rs.NumDescriptors = 11;
        ru.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ru.NumDescriptors = 12;   // u0..u10, then u11 the shadow map (kShadowUav)
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[1].DescriptorTable.NumDescriptorRanges = 1;
        p[1].DescriptorTable.pDescriptorRanges = &rs;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &ru;
        for (int k = 0; k < 3; ++k) p[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC ss[3] = {};
        for (int k = 0; k < 3; ++k) {
            ss[k].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            ss[k].AddressU = ss[k].AddressV = ss[k].AddressW =
                k == 0 ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            ss[k].MaxLOD = D3D12_FLOAT32_MAX;
            ss[k].ShaderRegister = k;
            ss[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        ss[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;   // the sky-view table wraps in azimuth
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        rsd.NumStaticSamplers = 3;
        rsd.pStaticSamplers = ss;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "cloud root signature";
            return false;
        }
    }
    const char* entries[6] = {"cs_noise_base", "cs_noise_detail", "cs_weather", "cs_march", "cs_temporal", "cs_shadow"};
    ComPtr<ID3D12PipelineState>* psos[6] = {&base_pso_, &detail_pso_, &weather_pso_, &march_pso_, &temporal_pso_,
                                            &shadow_pso_};
    for (int k = 0; k < 6; ++k) {
        ComPtr<ID3DBlob> cs;
        if (!compile_shader(kCloudShader, entries[k], "cs_5_1", cs, error)) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC cd = {};
        cd.pRootSignature = root_.Get();
        cd.CS.pShaderBytecode = cs->GetBufferPointer();
        cd.CS.BytecodeLength = cs->GetBufferSize();
        if (FAILED(device->CreateComputePipelineState(&cd, IID_PPV_ARGS(psos[k]->GetAddressOf())))) {
            error = std::string("cloud pso ") + entries[k];
            return false;
        }
    }
    if (!resize(width, height, error)) return false;
    ready_ = true;
    return true;
}

bool Clouds::resize(UINT width, UINT height, std::string& error) {
    w_ = (width + 3) / 4;
    h_ = (height + 3) / 4;
    if (w_ == 0) w_ = 1;
    if (h_ == 0) h_ = 1;
    return make_targets(error);
}

bool Clouds::make_targets(std::string& error) {
    const DXGI_FORMAT f16 = DXGI_FORMAT_R16G16B16A16_FLOAT, r16 = DXGI_FORMAT_R16_FLOAT;
    const D3D12_RESOURCE_DIMENSION t2 = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    if (!make_tex(dev_, raw_, t2, w_, h_, 1, f16, kUav) || !make_tex(dev_, raw_dist_, t2, w_, h_, 1, r16, kUav) ||
        !make_tex(dev_, hist_[0], t2, w_, h_, 1, f16, kNps) || !make_tex(dev_, hdist_[0], t2, w_, h_, 1, r16, kNps) ||
        !make_tex(dev_, hist_[1], t2, w_, h_, 1, f16, kNps) || !make_tex(dev_, hdist_[1], t2, w_, h_, 1, r16, kNps) ||
        !make_tex(dev_, out_, t2, w_, h_, 1, f16, kUav) || !make_tex(dev_, out_dist_, t2, w_, h_, 1, r16, kUav)) {
        error = "cloud targets";
        return false;
    }
    view2d(dev_, raw_.Get(), f16, cpu(kSrvBase + 5), cpu(kUavBase + 3));
    view2d(dev_, raw_dist_.Get(), r16, cpu(kSrvBase + 6), cpu(kUavBase + 4));
    view2d(dev_, hist_[0].Get(), f16, cpu(kSrvBase + 7), cpu(kUavBase + 5));
    view2d(dev_, hdist_[0].Get(), r16, cpu(kSrvBase + 8), cpu(kUavBase + 6));
    view2d(dev_, hist_[1].Get(), f16, cpu(kSrvBase + 9), cpu(kUavBase + 7));
    view2d(dev_, hdist_[1].Get(), r16, cpu(kSrvBase + 10), cpu(kUavBase + 8));
    view2d(dev_, out_.Get(), f16, cpu(kOutSrv + 0), cpu(kUavBase + 9));
    view2d(dev_, out_dist_.Get(), r16, cpu(kOutSrv + 1), cpu(kUavBase + 10));
    out_readable_ = false;
    frames_ = 0;
    prev_ = 0;
    return true;
}

void Clouds::record(ID3D12GraphicsCommandList* cmd, UINT frame, const Frame& f) {
    if (!ready_) return;
    auto tr = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER rb = {};
        rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        rb.Transition.pResource = r;
        rb.Transition.StateBefore = a;
        rb.Transition.StateAfter = b;
        rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &rb);
    };
    // The storm decides the sky: a calm day's few fair-weather cumulus, a
    // gale's nearly closed deck of cumulonimbus, rain from the darkest of it.
    const float s = f.storm < 0.0f ? 0.0f : (f.storm > 1.0f ? 1.0f : f.storm);
    coverage_ = 0.12f + 0.8f * std::pow(s, 1.3f);
    type_ = 0.35f + 0.65f * smooth01(0.45f, 0.95f, s);
    precip_ = smooth01(0.55f, 0.9f, s);

    CloudConstants c = {};
    std::memcpy(c.inv_view_proj, f.inv_view_proj, sizeof c.inv_view_proj);
    std::memcpy(c.prev_view_proj, f.prev_view_proj, sizeof c.prev_view_proj);
    for (int k = 0; k < 3; ++k) {
        c.cam_pos[k] = f.cam_pos[k];
        c.key_dir[k] = f.key_dir[k];
        c.key_lux_top[k] = f.key_lux_top[k];
        c.sky_mean[k] = f.sky_mean[k];
        c.ground[k] = f.ground[k];
    }
    c.time = f.time;
    c.cam_alt_km = f.cam_alt_km;
    c.frame = float(frames_ % 1024u);
    c.coverage = coverage_;
    c.cloud_type = type_;
    c.precip = precip_;
    c.drift[0] = f.drift[0] * settings_.drift_factor;
    c.drift[1] = f.drift[1] * settings_.drift_factor;
    c.wind_dir[0] = f.wind_dir[0];
    c.wind_dir[1] = f.wind_dir[1];
    c.base_alt = settings_.base_alt;
    c.sigma = settings_.sigma;
    c.base_scale = settings_.base_scale;
    c.detail_scale = settings_.detail_scale;
    c.res[0] = float(w_);
    c.res[1] = float(h_);
    c.inv_res[0] = 1.0f / float(w_);
    c.inv_res[1] = 1.0f / float(h_);
    c.map_extent = settings_.map_extent;
    c.history_valid = (f.history_valid && frames_ > 0) ? 1.0f : 0.0f;
    c.shear = settings_.shear;
    c.prev_index = prev_;
    c.haze_km = settings_.haze_km;
    c.detail_amount = settings_.detail_amount;
    c.pad0 = std::getenv("BIOMES_CLOUDS_DBG") ? float(std::atoi(std::getenv("BIOMES_CLOUDS_DBG"))) : 0.0f;
    // The shadow map follows the camera's point in the cloud-base plane, in
    // whole texels so the shadows do not crawl as it moves.
    {
        const float ly = f.key_dir[1] > 0.03f ? f.key_dir[1] : 0.03f;
        const float lift = settings_.base_alt - f.cam_pos[1];
        const float tex = 2.0f * kShadowExtent / float(kShadowSize);
        for (int k = 0; k < 2; ++k) {
            const float q = f.cam_pos[k * 2] + f.key_dir[k * 2] / ly * lift;
            shadow_c_[k] = std::floor(q / tex) * tex;
        }
        c.shadow_map[0] = shadow_c_[0];
        c.shadow_map[1] = shadow_c_[1];
        c.shadow_map[2] = kShadowExtent;
        c.shadow_map[3] = float(kShadowSize);
    }
    std::memcpy(cb_cpu_[frame & 1u], &c, sizeof c);

    cmd->SetComputeRootSignature(root_.Get());
    cmd->SetComputeRootConstantBufferView(0, cb_[frame & 1u]->GetGPUVirtualAddress());
    cmd->SetComputeRootDescriptorTable(1, gpu(kSrvBase));
    cmd->SetComputeRootDescriptorTable(2, gpu(kUavBase));

    if (!noise_done_) {
        cmd->SetPipelineState(base_pso_.Get());
        cmd->Dispatch(128 / 4, 128 / 4, 128 / 4);
        cmd->SetPipelineState(detail_pso_.Get());
        cmd->Dispatch(32 / 4, 32 / 4, 32 / 4);
        tr(base_.Get(), kUav, kNps);
        tr(detail_.Get(), kUav, kNps);
        noise_done_ = true;
    } else {
        tr(weather_.Get(), kNps, kUav);
    }
    cmd->SetPipelineState(weather_pso_.Get());
    cmd->Dispatch(kWeatherSize / 8, kWeatherSize / 8, 1);
    tr(weather_.Get(), kUav, kNps);
    // The cloud-shadow map, for the composite to darken the sun by.
    if (shadow_readable_) tr(shadow_.Get(), kRead, kUav);
    cmd->SetPipelineState(shadow_pso_.Get());
    cmd->Dispatch(kShadowSize / 8, kShadowSize / 8, 1);
    tr(shadow_.Get(), kUav, kRead);
    shadow_readable_ = true;

    if (frames_ > 0) {
        tr(raw_.Get(), kNps, kUav);
        tr(raw_dist_.Get(), kNps, kUav);
    }
    if (out_readable_) {
        tr(out_.Get(), kRead, kUav);
        tr(out_dist_.Get(), kRead, kUav);
    }
    cmd->SetPipelineState(march_pso_.Get());
    cmd->Dispatch((w_ + 7) / 8, (h_ + 7) / 8, 1);
    tr(raw_.Get(), kUav, kNps);
    tr(raw_dist_.Get(), kUav, kNps);

    const unsigned cur = 1u - prev_;
    tr(hist_[cur].Get(), kNps, kUav);
    tr(hdist_[cur].Get(), kNps, kUav);
    cmd->SetPipelineState(temporal_pso_.Get());
    cmd->Dispatch((w_ + 7) / 8, (h_ + 7) / 8, 1);
    tr(hist_[cur].Get(), kUav, kNps);
    tr(hdist_[cur].Get(), kUav, kNps);
    tr(out_.Get(), kUav, kRead);
    tr(out_dist_.Get(), kUav, kRead);
    out_readable_ = true;
    prev_ = cur;
    ++frames_;
}

}  // namespace engine
