#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/props/glow_mushrooms.h"
#include "engine/render/shader_compile.h"
#include "worldgen/assign/json_lite.h"
#include "worldgen/world.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace engine {

namespace {

const char* kShader = "engine/render/shaders/mushrooms.hlsl";

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
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)))) return nullptr;
    return r;
}

// A small counter-based generator: every draw a pure function of its key.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t next() {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    float uni() { return float(next() >> 40) / float(1u << 24); }
};

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

void qmul(const float a[4], const float b[4], float o[4]) {
    o[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    o[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    o[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    o[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// The mushroom's up axis under its rotation.
void qup(const float q[4], float o[3]) {
    o[0] = 2.0f * (q[0] * q[1] - q[3] * q[2]);
    o[1] = 1.0f - 2.0f * (q[0] * q[0] + q[2] * q[2]);
    o[2] = 2.0f * (q[1] * q[2] + q[3] * q[0]);
}

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

}  // namespace

bool GlowMushrooms::load_config(const char* path, std::string& error) {
    try {
        const jsonlite::Value root = jsonlite::parse_file(path);
        if (root.has("glow_mushrooms")) {
            const jsonlite::Value& g = root.at("glow_mushrooms");
            auto num = [&](const char* k, float& v) { if (g.has(k)) v = float(g.number(k)); };
            auto pair = [&](const char* k, float& a, float& b) {
                if (!g.has(k)) return;
                const jsonlite::Array& arr = g.array(k);
                if (arr.size() != 2) throw std::runtime_error(std::string(k) + " wants two numbers");
                a = float(arr[0].num);
                b = float(arr[1].num);
            };
            num("per_chamber", cfg_.per_chamber);
            num("tunnel_chance", cfg_.tunnel_chance);
            float cmin = float(cfg_.cluster_min), cmax = float(cfg_.cluster_max);
            pair("cluster", cmin, cmax);
            cfg_.cluster_min = (std::max)(1, int(cmin));
            cfg_.cluster_max = (std::max)(cfg_.cluster_min, int(cmax));
            num("cluster_radius", cfg_.cluster_radius);
            pair("size", cfg_.size_min, cfg_.size_max);
            if (g.has("colour")) {
                const jsonlite::Array& c = g.array("colour");
                if (c.size() != 3) throw std::runtime_error("colour wants three numbers");
                for (int k = 0; k < 3; ++k) cfg_.colour[k] = float(c[size_t(k)].num);
            }
            num("nits", cfg_.nits);
            num("lumens_each", cfg_.lumens_each);
            num("range", cfg_.range);
            if (g.has("item")) cfg_.item = g.string("item");
        }
    } catch (const std::exception& e) {
        error = std::string("archetypes.json glow_mushrooms: ") + e.what();
        return false;
    }
    float lin[3];
    for (int k = 0; k < 3; ++k) lin[k] = srgb_to_linear(cfg_.colour[k]);
    const float y = (std::max)(1e-4f, 0.2126f * lin[0] + 0.7152f * lin[1] + 0.0722f * lin[2]);
    for (int k = 0; k < 3; ++k) glow_lin_[k] = lin[k] / y;
    return true;
}

// Three mushrooms in metres, the stem's foot at the origin (sunk 5 cm, so a
// floor that the mesh puts a few centimetres off the field still holds it).
// Every index absolute (CLAUDE.md: SV_VertexID and the base vertex). Drawn
// double sided, so the winding does not matter; the pixel shader turns each
// normal to the eye. `emit` per vertex: the gills glow most, the cap less,
// the stem a little towards its top.
void GlowMushrooms::build_models(std::vector<Vertex>& v, std::vector<uint32_t>& idx) {
    struct Shape { float h, r0, r1, R, H; };
    const Shape shapes[3] = {
        {0.13f, 0.016f, 0.011f, 0.055f, 0.035f},   // the common one
        {0.21f, 0.013f, 0.009f, 0.040f, 0.030f},   // tall, small cap
        {0.07f, 0.020f, 0.016f, 0.070f, 0.030f},   // squat, wide cap
    };
    const int S = 10;   // sides
    const float kPi = 3.14159265f;
    for (const Shape& sh : shapes) {
        Model m;
        m.first = uint32_t(idx.size());
        m.height = sh.h + sh.H;
        auto ring = [&](float y, float r, float ny, float nr, float emit) {
            const uint32_t base = uint32_t(v.size());
            for (int s = 0; s < S; ++s) {
                const float a = 2.0f * kPi * float(s) / float(S);
                const float c = std::cos(a), sn = std::sin(a);
                Vertex x;
                x.pos[0] = r * c; x.pos[1] = y; x.pos[2] = r * sn;
                const float l = std::sqrt(nr * nr + ny * ny);
                x.nrm[0] = nr * c / l; x.nrm[1] = ny / l; x.nrm[2] = nr * sn / l;
                x.emit = emit;
                v.push_back(x);
            }
            return base;
        };
        auto strip = [&](uint32_t a, uint32_t b) {
            for (int s = 0; s < S; ++s) {
                const uint32_t s1 = uint32_t((s + 1) % S);
                idx.push_back(a + uint32_t(s)); idx.push_back(b + uint32_t(s)); idx.push_back(b + s1);
                idx.push_back(a + uint32_t(s)); idx.push_back(b + s1); idx.push_back(a + s1);
            }
        };
        // The stem, four rings, a slight taper.
        uint32_t prev = 0;
        for (int k = 0; k <= 3; ++k) {
            const float t = float(k) / 3.0f;
            const float y = -0.05f + (sh.h + 0.05f) * t;
            const float r = sh.r0 + (sh.r1 - sh.r0) * t;
            const uint32_t cur = ring(y, r, 0.0f, 1.0f, 0.05f + 0.25f * t * t);
            if (k > 0) strip(prev, cur);
            prev = cur;
        }
        // The gills: from the stem's top out to the cap's lip, facing down.
        const float lip_y = sh.h - 0.25f * sh.H;
        const uint32_t g0 = ring(sh.h, sh.r1, -1.0f, 0.0f, 1.0f);
        const uint32_t g1 = ring(lip_y + 0.004f, sh.R * 0.97f, -1.0f, 0.15f, 1.0f);
        strip(g0, g1);
        // The cap: a dome from the lip to the apex.
        const int K = 5;
        prev = 0;
        for (int k = 0; k <= K; ++k) {
            const float t = 1.0f - float(k) / float(K);         // 1 at the lip, 0 at the apex
            const float a = t * 0.5f * kPi;
            const float r = sh.R * std::sin(a) + 1e-4f;
            const float y = sh.h + sh.H * std::cos(a) - 0.25f * sh.H * t * t;
            // The ellipsoid's normal.
            const float nr = std::sin(a) / sh.R, ny = std::cos(a) / sh.H;
            const uint32_t cur = ring(y, r, ny, nr, 0.65f - 0.25f * (1.0f - t));
            if (k > 0) strip(prev, cur);
            prev = cur;
        }
        // The lip itself, joining gills and cap.
        strip(g1, g1 + uint32_t(S));   // the cap's first ring (the lip) follows the gills' outer one
        m.count = uint32_t(idx.size()) - m.first;
        models_.push_back(m);
    }
}

bool GlowMushrooms::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
                         const char* archetypes_path, std::string& error) {
    if (!load_config(archetypes_path, error)) return false;
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    build_models(verts, idx);

    const UINT64 vbytes = verts.size() * sizeof(Vertex), ibytes = idx.size() * 4;
    vb_ = buffer(device, vbytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ib_ = buffer(device, ibytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> up = buffer(device, vbytes + ibytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vb_ || !ib_ || !up) { error = "mushroom mesh buffers"; return false; }
    {
        uint8_t* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        std::memcpy(p, verts.data(), size_t(vbytes));
        std::memcpy(p + vbytes, idx.data(), size_t(ibytes));
        up->Unmap(0, nullptr);
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> cmd;
        ComPtr<ID3D12Fence> fence;
        HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&cmd));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr)) { error = hr_text("mushroom upload setup", hr); return false; }
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
    vbv_.StrideInBytes = sizeof(Vertex);
    ibv_.BufferLocation = ib_->GetGPUVirtualAddress();
    ibv_.SizeInBytes = UINT(ibytes);
    ibv_.Format = DXGI_FORMAT_R32_UINT;

    for (UINT f = 0; f < kFrames; ++f) {
        inst_[f] = buffer(device, UINT64(kMaxDrawn) * sizeof(MushroomGpu), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!inst_[f]) { error = "mushroom instance buffer"; return false; }
        inst_[f]->Map(0, nullptr, reinterpret_cast<void**>(&inst_cpu_[f]));
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kShader, "vs_mushroom", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kShader, "ps_mushroom", "ps_5_1", ps, error)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[3] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = mesh_root;
    pd.InputLayout = {layout, 3};
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
        pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.DepthStencilState.DepthEnable = TRUE;
    pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 3;   // direct, ambient, the puddle mask
    pd.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RTVFormats[1] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.RTVFormats[2] = DXGI_FORMAT_R16G16B16A16_FLOAT;
    pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    pd.SampleDesc.Count = 1;
    const HRESULT hr = device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso_));
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (mushrooms)", hr); return false; }
    ready_ = true;
    return true;
}

void GlowMushrooms::populate(size_t ci, const GroundFn& ground) {
    const worldgen::SeedWorld::Cave& cave = world_->caves[ci];
    const int n = cave.seg_count;
    if (n < 2) return;
    // The mouth's bowl (the first capsule) grows none: daylight reaches it.
    // The last three are the chamber's (worldgen's cave layout).
    const int chamber_from = n >= 5 ? n - 3 : n - 1;
    const float kPi = 3.14159265f;
    for (int k = 1; k < n; ++k) {
        const worldgen::SeedWorld::EntranceSeg& e = world_->entrances[size_t(cave.seg_begin + k)];
        Rng rng((uint64_t(cave.id) << 20) ^ (uint64_t(k) * 0x2545F4914F6CDD1Dull) ^ 0x6D757368726F6F6Dull);
        const bool chamber = k >= chamber_from;
        const float want = chamber ? cfg_.per_chamber : cfg_.tunnel_chance;
        int count = int(want);
        if (rng.uni() < want - float(count)) ++count;
        for (int c = 0; c < count; ++c) {
            const float u = rng.uni();
            const float ax = e.ax + (e.bx - e.ax) * u, ay = e.ay + (e.by - e.ay) * u, az = e.az + (e.bz - e.az) * u;
            const float ra = e.ra + (e.rb - e.ra) * u;
            const float ang = 2.0f * kPi * rng.uni();
            const float off = ra * (0.1f + 0.55f * rng.uni());
            const float cx = ax + off * std::cos(ang), cz = az + off * std::sin(ang);
            const float floor_y = ground(cx, cz, ay);
            // Started in rock, or fell further than a cave floor lies.
            if (floor_y >= ay - 0.01f || ay - floor_y > ra + 3.0f) continue;
            Cluster cl;
            cl.id = uint32_t(rng.next());
            const int nm = cfg_.cluster_min + int(rng.uni() * float(cfg_.cluster_max - cfg_.cluster_min + 1));
            for (int j = 0; j < nm; ++j) {
                const float rr = cfg_.cluster_radius * std::sqrt(rng.uni());
                const float a2 = 2.0f * kPi * rng.uni();
                const float mx = cx + rr * std::cos(a2), mz = cz + rr * std::sin(a2);
                const float top = floor_y + 0.7f;
                const float my = ground(mx, mz, top);
                if (my >= top - 0.01f || std::fabs(my - floor_y) > 0.45f) continue;
                Mushroom m;
                m.pos[0] = mx; m.pos[1] = my - 0.02f; m.pos[2] = mz;
                m.scale = cfg_.size_min + (cfg_.size_max - cfg_.size_min) * rng.uni();
                // A yaw, then a lean of up to ~14 degrees about a level axis.
                const float yaw = 2.0f * kPi * rng.uni();
                const float qy[4] = {0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)};
                const float tilt = 0.25f * rng.uni(), ta = 2.0f * kPi * rng.uni();
                const float qt[4] = {std::cos(ta) * std::sin(0.5f * tilt), 0.0f, std::sin(ta) * std::sin(0.5f * tilt),
                                     std::cos(0.5f * tilt)};
                qmul(qt, qy, m.rot);
                const float pick = rng.uni();
                m.model = pick < 0.55f ? 0u : (pick < 0.8f ? 1u : 2u);
                m.id = uint32_t(rng.next());
                cl.m.push_back(m);
            }
            if (cl.m.size() < 2) continue;
            float s[3] = {0.0f, 0.0f, 0.0f};
            for (const Mushroom& m : cl.m) for (int q = 0; q < 3; ++q) s[q] += m.pos[q];
            for (int q = 0; q < 3; ++q) cl.centre[q] = s[q] / float(cl.m.size());
            // Over the caps, high enough that the floor under the light is
            // not a hot spot of the inverse square: the cluster's glow comes
            // from half a metre of caps, not a point.
            cl.centre[1] += 0.45f;
            cl.left = int(cl.m.size());
            n_mushrooms_ += cl.m.size();
            clusters_.push_back(cl);
        }
    }
}

void GlowMushrooms::update(const float cam[3], const GroundFn& ground, int max_caves) {
    if (!ready_ || !world_) return;
    const auto& caves = world_->caves;
    if (generated_.size() != caves.size()) generated_.assign(caves.size(), 0);
    int done = 0;
    for (size_t i = 0; i < caves.size() && done < max_caves; ++i) {
        if (generated_[i]) continue;
        const worldgen::SeedWorld::Cave& c = caves[i];
        auto d2 = [&](float x, float z) { return (x - cam[0]) * (x - cam[0]) + (z - cam[2]) * (z - cam[2]); };
        const float reach = kGenReach + c.length;
        if ((std::min)(d2(c.x, c.z), d2(c.cx, c.cz)) > reach * reach) continue;
        populate(i, ground);
        generated_[i] = 1;
        ++n_generated_;
        ++done;
        dirty_ = true;
    }
}

void GlowMushrooms::prepare(UINT frame, const float cam[3], const float view_proj[16]) {
    groups_[frame].clear();
    drawn_ = 0;
    if (!ready_ || clusters_.empty()) return;
    float planes[6][4];
    frustum_planes(view_proj, planes);
    MushroomGpu* out = inst_cpu_[frame];
    size_t n = 0;
    for (uint32_t model = 0; model < uint32_t(models_.size()); ++model) {
        const size_t first = n;
        for (const Cluster& cl : clusters_) {
            if (cl.left == 0) continue;
            const float dx = cl.centre[0] - cam[0], dz = cl.centre[2] - cam[2];
            if (dx * dx + dz * dz > (kDrawDist + 2.0f) * (kDrawDist + 2.0f)) continue;
            bool in = true;
            for (int p = 0; p < 6 && in; ++p)
                if (planes[p][0] * cl.centre[0] + planes[p][1] * cl.centre[1] + planes[p][2] * cl.centre[2] +
                    planes[p][3] < -(cfg_.cluster_radius + 0.5f)) in = false;
            if (!in) continue;
            for (const Mushroom& m : cl.m) {
                if (m.picked || m.model != model || n >= kMaxDrawn) continue;
                MushroomGpu g = {};
                for (int k = 0; k < 3; ++k) g.pos[k] = m.pos[k];
                g.scale = m.scale;
                for (int k = 0; k < 4; ++k) g.rot[k] = m.rot[k];
                // Each its own strength, from its id: a cluster is not one flat glow.
                const float vary = 0.7f + 0.6f * float(m.id & 0xFFFFu) / 65535.0f;
                for (int k = 0; k < 3; ++k) g.glow[k] = glow_lin_[k] * cfg_.nits * vary;
                out[n++] = g;
            }
        }
        if (n > first) groups_[frame].push_back(Group{model, uint32_t(first), uint32_t(n - first)});
    }
    drawn_ = n;
}

void GlowMushrooms::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || groups_[frame].empty()) return;
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv_);
    cmd->IASetIndexBuffer(&ibv_);
    const D3D12_GPU_VIRTUAL_ADDRESS va = inst_[frame]->GetGPUVirtualAddress();
    for (const Group& g : groups_[frame]) {
        const Model& m = models_[g.model];
        cmd->SetGraphicsRootShaderResourceView(1, va + UINT64(g.first) * sizeof(MushroomGpu));
        cmd->DrawIndexedInstanced(m.count, g.count, m.first, 0, 0);
    }
}

void GlowMushrooms::lights(const float cam[3], std::vector<LocalLight>& out) const {
    for (const Cluster& cl : clusters_) {
        if (cl.left == 0) continue;
        const float dx = cl.centre[0] - cam[0], dy = cl.centre[1] - cam[1], dz = cl.centre[2] - cam[2];
        if (dx * dx + dy * dy + dz * dz > kLightRadius * kLightRadius) continue;
        LocalLight l;
        for (int k = 0; k < 3; ++k) {
            l.pos[k] = cl.centre[k];
            l.color[k] = glow_lin_[k];
        }
        l.lumens = cfg_.lumens_each * float(cl.left);
        l.range = cfg_.range;
        l.flicker = 0.0f;
        l.shadow = true;
        // Stable per cluster, apart from the renderer's own (counted up from
        // 1) and the torch's 0, so a cluster keeps its shadow slot.
        l.id = 0x80000000u | (cl.id & 0x7FFFFFFFu);
        out.push_back(l);
    }
}

bool GlowMushrooms::pick(const float eye[3], const float dir[3], const float hand[3], float reach, float occluder) {
    Cluster* best_cl = nullptr;
    Mushroom* best = nullptr;
    float best_score = 1e30f;
    for (Cluster& cl : clusters_) {
        if (cl.left == 0) continue;
        const float hx = cl.centre[0] - hand[0], hy = cl.centre[1] - hand[1], hz = cl.centre[2] - hand[2];
        const float lim = reach + cfg_.cluster_radius + 0.5f;
        if (hx * hx + hy * hy + hz * hz > lim * lim) continue;
        for (Mushroom& m : cl.m) {
            if (m.picked) continue;
            // The middle of the cap, under the mushroom's own lean.
            float up[3];
            qup(m.rot, up);
            const float h = models_[m.model].height * m.scale;
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = m.pos[k] + up[k] * (h * 0.8f);
            const float t = (c[0] - eye[0]) * dir[0] + (c[1] - eye[1]) * dir[1] + (c[2] - eye[2]) * dir[2];
            if (t <= 0.0f || t > occluder + 0.25f) continue;
            float perp2 = 0.0f, hand2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float q = c[k] - (eye[k] + dir[k] * t);
                perp2 += q * q;
                hand2 += (c[k] - hand[k]) * (c[k] - hand[k]);
            }
            if (hand2 > reach * reach) continue;
            // Generous: a mushroom is a few centimetres across, and a picker
            // that needs a pixel-perfect aim is no fun.
            const float tol = 0.12f + 0.06f * m.scale;
            if (perp2 > tol * tol) continue;
            const float score = t + 4.0f * std::sqrt(perp2);
            if (score < best_score) { best_score = score; best = &m; best_cl = &cl; }
        }
    }
    if (!best) return false;
    best->picked = true;
    --best_cl->left;
    ++picked_count_;
    dirty_ = true;
    return true;
}

void GlowMushrooms::targets_near(const float p[3], float radius, std::vector<float>& xyz) const {
    for (const Cluster& cl : clusters_) {
        for (const Mushroom& m : cl.m) {
            if (m.picked) continue;
            float up[3];
            qup(m.rot, up);
            const float h = models_[m.model].height * m.scale * 0.8f;
            float c[3];
            for (int k = 0; k < 3; ++k) c[k] = m.pos[k] + up[k] * h;
            const float dx = c[0] - p[0], dy = c[1] - p[1], dz = c[2] - p[2];
            if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
            xyz.insert(xyz.end(), c, c + 3);
        }
    }
}

bool GlowMushrooms::nearest(const float p[3], float out[3]) const {
    float bd = 1e30f;
    for (const Cluster& cl : clusters_) {
        if (cl.left == 0) continue;
        const float dx = cl.centre[0] - p[0], dy = cl.centre[1] - p[1], dz = cl.centre[2] - p[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < bd) { bd = d; for (int k = 0; k < 3; ++k) out[k] = cl.centre[k]; }
    }
    return bd < 1e29f;
}

}  // namespace engine
