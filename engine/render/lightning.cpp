// The worldgen headers first: d3d12.h brings windows.h's min and max macros,
// which break std::min in world.h.
#include "worldgen/assign/json_lite.h"
#include "worldgen/world.h"

#include "engine/render/lightning.h"
#include "engine/render/shader_compile.h"

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace engine {

namespace {
const double kSlot = 2.5;        // s: at most one strike per slot
const float  kLife = 1.2f;       // s a strike lasts, strokes and afterglow
const float  kPulse = 0.03f;     // s: a return stroke's decay

// Matches cbuffer LightningConstants in lightning.hlsl: 112 bytes.
struct LightningConstants {
    float view_proj[16];
    float cam_pos[3];  float channel;
    float near_plane;  float far_plane;  float pixel_angle;  float count;
    float inv_size[2]; float pad[2];
};
static_assert(sizeof(LightningConstants) == 112, "LightningConstants must match lightning.hlsl");
const char* kShader = "engine/render/shaders/lightning.hlsl";

float u01(uint64_t key, uint64_t k) {
    return float(worldgen::hash64(key ^ (k * 0x9E3779B97F4A7C15ull)) >> 40) / 16777216.0f;
}

// A small deterministic generator for the bolt's shape.
struct Rng {
    uint64_t s;
    float next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return float(s >> 40) / 16777216.0f;
    }
    float sym() { return next() * 2.0f - 1.0f; }
};
}  // namespace

bool Lightning::load(const char* archetypes_path, std::string& error) {
    try {
        const jsonlite::Value root = jsonlite::parse_file(archetypes_path);
        if (!root.has("weather")) return true;
        const jsonlite::Value& w = root.at("weather");
        if (!w.has("lightning")) return true;
        const jsonlite::Value& l = w.at("lightning");
        LightningSettings& s = settings_;
        if (l.has("storm")) {
            s.storm_lo = float(l.array("storm").at(0).num);
            s.storm_hi = float(l.array("storm").at(1).num);
        }
        if (l.has("per_minute")) s.per_minute = float(l.number("per_minute"));
        if (l.has("candela")) s.candela = float(l.number("candela"));
        if (l.has("range_m")) s.range_m = float(l.number("range_m"));
        if (l.has("ground_share")) s.ground_share = float(l.number("ground_share"));
    } catch (const std::exception& e) {
        error = std::string("archetypes.json weather.lightning: ") + e.what();
        return false;
    }
    return true;
}

void Lightning::set_view_basis(const float fwd[3], const float right[3]) {
    for (int k = 0; k < 3; ++k) {
        fwd_[k] = fwd[k];
        right_[k] = right[k];
    }
}

Strike Lightning::at(double t, const float cam[3], float base_alt, const std::function<float(double)>& storm_at,
                     const std::function<float(float, float)>& ground_h) const {
    Strike s;
    uint64_t key = 0;
    float age = 0.0f;
    float x = 0.0f, z = 0.0f;
    if (forced_) {
        // Straight at the bearing, on the ground, at its first stroke's peak.
        float fx = fwd_[0], fz = fwd_[2];
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-4f) { fx /= fl; fz /= fl; } else { fx = 0.0f; fz = 1.0f; }
        float rx = right_[0], rz = right_[2];
        const float rl = std::sqrt(rx * rx + rz * rz);
        if (rl > 1e-4f) { rx /= rl; rz /= rl; }
        const float c = std::cos(forced_bearing_), sn = std::sin(forced_bearing_);
        x = cam[0] + (fx * c + rx * sn) * forced_dist_;
        z = cam[2] + (fz * c + rz * sn) * forced_dist_;
        key = (uint64_t(seed_) << 32) ^ 0xF0C3Dull;
        s.id = 0xFFFFFFF0u;
        s.ground = !forced_cloud_;
        age = 0.0f;
    } else {
        const int64_t kc = int64_t(std::floor(t / kSlot));
        bool found = false;
        for (int64_t k = kc; k >= kc - 1 && !found; --k) {
            const uint64_t kk = (uint64_t(seed_) << 32) ^ (uint64_t(k) * 0xD1B54A32D192ED03ull) ^ 0x4C49474Eull;
            const double start_slot = double(k) * kSlot;
            float g = (storm_at(start_slot) - settings_.storm_lo) / (settings_.storm_hi - settings_.storm_lo);
            g = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
            const float p = settings_.per_minute / 60.0f * float(kSlot) * g;
            if (u01(kk, 1) >= p) continue;
            const double start = start_slot + double(u01(kk, 2)) * (kSlot - kLife);
            const float a = float(t - start);
            if (a < 0.0f || a > kLife) continue;
            key = kk;
            age = a;
            s.id = uint32_t(k & 0x7FFFFFFF);
            found = true;
        }
        if (!found) return s;
        // Round the camera's 2 km cell, so a strike does not move as the
        // camera does, and within range_m of it.
        const float cx = (std::floor(cam[0] / 2000.0f) + 0.5f) * 2000.0f;
        const float cz = (std::floor(cam[2] / 2000.0f) + 0.5f) * 2000.0f;
        x = cx + (u01(key, 3) * 2.0f - 1.0f) * settings_.range_m;
        z = cz + (u01(key, 4) * 2.0f - 1.0f) * settings_.range_m;
        s.ground = u01(key, 5) < settings_.ground_share;
    }
    s.active = true;
    s.age = age;
    const float gy = ground_h(x, z);
    s.ground_pt[0] = x; s.ground_pt[1] = gy > 0.0f ? gy : 0.0f; s.ground_pt[2] = z;
    // The channel leaves the cloud base a few hundred metres off the point
    // it strikes: lightning leans.
    s.cloud_pt[0] = x + (u01(key, 6) * 2.0f - 1.0f) * 400.0f;
    s.cloud_pt[1] = base_alt + 100.0f;
    s.cloud_pt[2] = z + (u01(key, 7) * 2.0f - 1.0f) * 400.0f;
    if (s.ground) {
        for (int k = 0; k < 3; ++k) s.flash_pt[k] = 0.5f * (s.ground_pt[k] + s.cloud_pt[k]);
    } else {
        s.flash_pt[0] = x;
        s.flash_pt[1] = base_alt + 800.0f + u01(key, 8) * 2000.0f;
        s.flash_pt[2] = z;
    }
    // The strokes: one to four for a ground strike (one to three in the
    // cloud), 40-150 ms apart, each a sharp pulse; a continuing current
    // glows under them for a quarter of a second.
    const int n = s.ground ? 1 + int(u01(key, 9) * 4.0f) : 1 + int(u01(key, 9) * 3.0f);
    float tk = 0.0f, pulse = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i > 0) tk += 0.04f + 0.11f * u01(key, 10 + uint64_t(i));
        if (age >= tk) pulse += (i == 0 ? 1.0f : 0.6f) * std::exp(-(age - tk) / kPulse);
    }
    const float glow = 0.08f * std::exp(-age / 0.25f);
    const float peak = s.ground ? settings_.candela : 0.5f * settings_.candela;
    s.candela = peak * (pulse + glow);
    s.channel = s.ground ? (pulse > 1.0f ? 1.0f : pulse) + 2.0f * glow : 0.0f;
    if (s.channel > 1.0f) s.channel = 1.0f;
    const float dx = s.flash_pt[0] - cam[0], dy = s.flash_pt[1] - cam[1], dz = s.flash_pt[2] - cam[2];
    s.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    return s;
}

const std::vector<BoltSegment>& Lightning::bolt(const Strike& s, float base_alt) {
    (void)base_alt;
    if (!s.active || !s.ground) { segs_.clear(); bolt_id_ = 0xFFFFFFFFu; return segs_; }
    if (s.id == bolt_id_) return segs_;
    bolt_id_ = s.id;
    segs_.clear();
    Rng rng{(uint64_t(seed_) << 32) ^ (uint64_t(s.id) * 0x9E3779B97F4A7C15ull) ^ 0xB017ull};
    for (int i = 0; i < 4; ++i) rng.next();

    struct P { float v[3]; };
    // Midpoint displacement: each level splits every segment and pushes the
    // middle sideways by a share of its length, mostly horizontally --
    // lightning wanders across more than it doubles back.
    auto jag = [&](P a, P b, int levels, float disp) {
        std::vector<P> pts = {a, b};
        for (int l = 0; l < levels; ++l) {
            std::vector<P> nx;
            nx.reserve(pts.size() * 2);
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const P& p0 = pts[i];
                const P& p1 = pts[i + 1];
                const float dx = p1.v[0] - p0.v[0], dy = p1.v[1] - p0.v[1], dz = p1.v[2] - p0.v[2];
                const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
                P m = {{0.5f * (p0.v[0] + p1.v[0]) + rng.sym() * len * disp,
                        0.5f * (p0.v[1] + p1.v[1]) + rng.sym() * len * disp * 0.3f,
                        0.5f * (p0.v[2] + p1.v[2]) + rng.sym() * len * disp}};
                nx.push_back(p0);
                nx.push_back(m);
            }
            nx.push_back(pts.back());
            pts.swap(nx);
        }
        return pts;
    };
    auto emit = [&](const std::vector<P>& pts, float bright, float width) {
        for (size_t i = 0; i + 1 < pts.size() && segs_.size() < kMaxSegments; ++i) {
            BoltSegment g;
            for (int k = 0; k < 3; ++k) { g.a[k] = pts[i].v[k]; g.b[k] = pts[i + 1].v[k]; }
            g.bright = bright;
            g.width = width;
            segs_.push_back(g);
        }
    };
    const P top = {{s.cloud_pt[0], s.cloud_pt[1], s.cloud_pt[2]}};
    const P bot = {{s.ground_pt[0], s.ground_pt[1], s.ground_pt[2]}};
    const std::vector<P> main = jag(top, bot, 8, 0.22f);
    emit(main, 1.0f, 1.5f);
    // Branches: a few forks off the upper part, reaching down and out and
    // dying in the air, dimmer and thinner.
    const int nb = 3 + int(rng.next() * 5.0f);
    for (int b = 0; b < nb; ++b) {
        const size_t at = size_t(rng.next() * 0.7f * float(main.size() - 1));
        const P from = main[at];
        const float drop = (from.v[1] - bot.v[1]) * (0.15f + 0.35f * rng.next());
        const float ang = rng.next() * 6.2831853f;
        const float out = drop * (0.4f + 0.6f * rng.next());
        const P to = {{from.v[0] + std::cos(ang) * out, from.v[1] - drop, from.v[2] + std::sin(ang) * out}};
        emit(jag(from, to, 5, 0.25f), 0.35f, 0.8f);
    }
    return segs_;
}

bool Lightning::init(ID3D12Device* device, DXGI_FORMAT target, std::string& error) {
    ready_ = false;
    for (int f = 0; f < 2; ++f) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd = {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Width = 256;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&cb_[f])))) {
            error = "lightning constants";
            return false;
        }
        cb_[f]->Map(0, nullptr, &cb_cpu_[f]);
        bd.Width = sizeof(BoltSegment) * kMaxSegments;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                   nullptr, IID_PPV_ARGS(&segbuf_[f])))) {
            error = "lightning segments";
            return false;
        }
        segbuf_[f]->Map(0, nullptr, &seg_cpu_[f]);
    }
    {
        D3D12_DESCRIPTOR_RANGE r = {};
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 1;
        r.BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER p[3] = {};
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1].Descriptor.ShaderRegister = 1;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &r;
        D3D12_ROOT_SIGNATURE_DESC rsd = {};
        rsd.NumParameters = 3;
        rsd.pParameters = p;
        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
            FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            error = "lightning root signature";
            return false;
        }
    }
    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kShader, "vs_bolt", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kShader, "ps_bolt", "ps_5_1", ps, error)) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = root_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = FALSE;   // a bolt kilometres off is still drawn, depth tested by hand
    D3D12_RENDER_TARGET_BLEND_DESC& b = pd.BlendState.RenderTarget[0];
    b.BlendEnable = TRUE;
    b.SrcBlend = D3D12_BLEND_ONE;
    b.DestBlend = D3D12_BLEND_ONE;
    b.BlendOp = D3D12_BLEND_OP_ADD;
    b.SrcBlendAlpha = D3D12_BLEND_ZERO;
    b.DestBlendAlpha = D3D12_BLEND_ONE;
    b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    b.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN |
                              D3D12_COLOR_WRITE_ENABLE_BLUE;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = target;
    pd.SampleDesc.Count = 1;
    if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_)))) {
        error = "lightning pso";
        return false;
    }
    ready_ = true;
    return true;
}

void Lightning::record(ID3D12GraphicsCommandList* cmd, unsigned frame, const Frame& f,
                       D3D12_CPU_DESCRIPTOR_HANDLE rtv, D3D12_GPU_DESCRIPTOR_HANDLE depth_table) {
    const unsigned fi = frame & 1u;
    if (!ready_ || segs_.empty() || f.channel <= 0.001f) return;
    if (uploaded_id_[fi] != bolt_id_) {
        std::memcpy(seg_cpu_[fi], segs_.data(), segs_.size() * sizeof(BoltSegment));
        uploaded_[fi] = unsigned(segs_.size());
        uploaded_id_[fi] = bolt_id_;
    }
    LightningConstants c = {};
    std::memcpy(c.view_proj, f.view_proj, sizeof c.view_proj);
    for (int k = 0; k < 3; ++k) c.cam_pos[k] = f.cam_pos[k];
    c.channel = f.channel;
    c.near_plane = f.near_plane;
    c.far_plane = f.far_plane;
    c.pixel_angle = f.pixel_angle;
    c.count = float(uploaded_[fi]);
    c.inv_size[0] = 1.0f / float(f.width ? f.width : 1);
    c.inv_size[1] = 1.0f / float(f.height ? f.height : 1);
    std::memcpy(cb_cpu_[fi], &c, sizeof c);

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
    cmd->SetPipelineState(pso_.Get());
    cmd->SetGraphicsRootConstantBufferView(0, cb_[fi]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootShaderResourceView(1, segbuf_[fi]->GetGPUVirtualAddress());
    cmd->SetGraphicsRootDescriptorTable(2, depth_table);
    cmd->DrawInstanced(uploaded_[fi] * 12u, 1, 0, 0);
}

}  // namespace engine
