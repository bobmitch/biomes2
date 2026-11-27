#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "engine/props/forage.h"
#include "engine/render/shader_compile.h"
#include "worldgen/assign/json_lite.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace engine {

namespace {

const char* kShader = "engine/render/shaders/forage.hlsl";
const float kPi = 3.14159265f;
const double kDay = 86400.0;

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
    float in(float a, float b) { return a + (b - a) * uni(); }
};

uint64_t mix64(uint64_t z) {
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

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

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

struct V3 { float x, y, z; };
V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
    const float l = std::sqrt(dot(a, a));
    return l > 1e-9f ? a * (1.0f / l) : v3(0, 1, 0);
}

}  // namespace

Forage::Forage() {}
Forage::~Forage() { shutdown(); }

bool Forage::load_config(const char* path, std::string& error) {
    species_.clear();
    try {
        const jsonlite::Value root = jsonlite::parse_file(path);
        if (!root.has("forage")) return true;
        const jsonlite::Array& sp = root.at("forage").array("species");
        for (size_t i = 0; i < sp.size(); ++i) {
            const jsonlite::Value& e = sp[i];
            Species s;
            s.name = e.string("name");
            s.item = e.has("item") ? e.string("item") : s.name;
            if (e.has("yield")) {
                const jsonlite::Array& y = e.array("yield");
                if (y.size() != 2) throw std::runtime_error(s.name + ".yield wants [min, max]");
                s.yield_min = std::max(1, int(y[0].num));
                s.yield_max = std::max(s.yield_min, int(y[1].num));
            }
            if (e.has("regrow_days")) s.regrow_days = float(e.number("regrow_days"));
            if (e.has("look")) {
                const jsonlite::Value& l = e.at("look");
                if (l.has("size")) s.size = float(l.number("size"));
                auto col = [&](const char* k, float* out) {
                    if (!l.has(k)) return;
                    const jsonlite::Array& c = l.array(k);
                    if (c.size() != 3) throw std::runtime_error(s.name + ".look." + k + " wants three numbers");
                    for (int q = 0; q < 3; ++q) out[q] = float(c[size_t(q)].num);
                };
                col("leaf", s.leaf);
                col("berry", s.berry);
                col("flower", s.flower);
            }
            species_.push_back(s);
        }
    } catch (const std::exception& e) {
        error = std::string("archetypes.json forage: ") + e.what();
        return false;
    }
    return true;
}

// A strawberry-like plant, about 0.6 m across at scale 1, its foot at the
// origin: a rosette of trifoliate leaves on arching stalks (kLeaves), berries
// hanging at the rim with their green calyx (kBerries), and white flowers on
// stalks in their place while the plant grows back (kFlowers). Every index
// absolute (CLAUDE.md: SV_VertexID and the base vertex). Normals of flat
// pieces point up; the pixel shader turns them to the eye.
void Forage::build_models(const Species& sp, std::vector<Vertex>& vtx, std::vector<uint32_t>& idx) {
    float leaf[3], berry[3], flower[3];
    for (int k = 0; k < 3; ++k) {
        leaf[k] = srgb_to_linear(sp.leaf[k]);
        berry[k] = srgb_to_linear(sp.berry[k]);
        flower[k] = srgb_to_linear(sp.flower[k]);
    }
    const float stalk[3] = {leaf[0] * 1.1f + 0.01f, leaf[1] * 1.05f + 0.01f, leaf[2] * 0.7f};
    const float calyx[3] = {leaf[0] * 1.15f, leaf[1] * 1.2f, leaf[2] * 0.9f};
    const float middle[3] = {0.80f, 0.55f, 0.05f};

    auto add = [&](V3 p, V3 n, float u, float v, float kind, const float* c) {
        Vertex x;
        x.pos[0] = p.x; x.pos[1] = p.y; x.pos[2] = p.z;
        x.nrm[0] = n.x; x.nrm[1] = n.y; x.nrm[2] = n.z;
        x.uv[0] = u; x.uv[1] = v;
        x.kind = kind;
        x.col[0] = c[0]; x.col[1] = c[1]; x.col[2] = c[2];
        vtx.push_back(x);
        return uint32_t(vtx.size() - 1);
    };
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { idx.push_back(a); idx.push_back(b); idx.push_back(c); };
    // Normals of the vertices from v0 on, from the faces from i0 on, up.
    auto smooth = [&](size_t v0, size_t i0) {
        std::vector<V3> acc(vtx.size() - v0, v3(0, 0, 0));
        for (size_t t = i0; t + 2 < idx.size(); t += 3) {
            const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            const V3 pa = v3(vtx[a].pos[0], vtx[a].pos[1], vtx[a].pos[2]);
            const V3 pb = v3(vtx[b].pos[0], vtx[b].pos[1], vtx[b].pos[2]);
            const V3 pc = v3(vtx[c].pos[0], vtx[c].pos[1], vtx[c].pos[2]);
            V3 n = cross(pb - pa, pc - pa);
            if (n.y < 0.0f) n = n * -1.0f;
            acc[a - v0] = acc[a - v0] + n;
            acc[b - v0] = acc[b - v0] + n;
            acc[c - v0] = acc[c - v0] + n;
        }
        for (size_t k = 0; k < acc.size(); ++k) {
            const V3 n = norm(acc[k]);
            vtx[v0 + k].nrm[0] = n.x; vtx[v0 + k].nrm[1] = n.y; vtx[v0 + k].nrm[2] = n.z;
        }
    };
    // A flat ribbon along a path, its width level across the path.
    auto ribbon = [&](const std::vector<V3>& path, float w0, float w1, const float* c) {
        const size_t v0 = vtx.size(), i0 = idx.size();
        for (size_t k = 0; k < path.size(); ++k) {
            const V3 t = norm(path[std::min(k + 1, path.size() - 1)] - path[k > 0 ? k - 1 : 0]);
            V3 side = cross(t, v3(0, 1, 0));
            if (dot(side, side) < 1e-6f) side = v3(1, 0, 0);
            side = norm(side);
            const float f = float(k) / float(path.size() - 1);
            const float w = 0.5f * (w0 + (w1 - w0) * f);
            add(path[k] - side * w, v3(0, 1, 0), -1.0f, f, 1.0f, c);
            add(path[k] + side * w, v3(0, 1, 0), 1.0f, f, 1.0f, c);
        }
        for (size_t k = 0; k + 1 < path.size(); ++k) {
            const uint32_t a = uint32_t(v0 + 2 * k);
            tri(a, a + 1, a + 3);
            tri(a, a + 3, a + 2);
        }
        smooth(v0, i0);
    };
    auto bezier = [&](V3 a, V3 b, V3 c, int n) {
        std::vector<V3> p;
        for (int k = 0; k <= n; ++k) {
            const float t = float(k) / float(n);
            p.push_back(a * ((1 - t) * (1 - t)) + b * (2 * (1 - t) * t) + c * (t * t));
        }
        return p;
    };
    // A leaflet: a grid along `dir`, three across, its edges folded up and
    // every other row narrower (the teeth). uv = (across -1..1, along 0..1).
    auto leaflet = [&](V3 base, V3 dir, float len, float wid, Rng& rng) {
        const size_t v0 = vtx.size(), i0 = idx.size();
        const V3 side = norm(cross(v3(0, 1, 0), dir));
        const int R = 7;
        const float fold = 0.22f * wid, droop = rng.in(0.01f, 0.03f);
        for (int r = 0; r <= R; ++r) {
            const float t = float(r) / float(R);
            float w = wid * 0.5f * std::pow(std::max(0.0f, std::sin(kPi * (0.08f + 0.92f * t))), 0.7f);
            if (r % 2 == 1) w *= 0.86f;
            const V3 mid = base + dir * (len * t) + v3(0, -droop * t * t, 0);
            for (int c = -1; c <= 1; ++c) {
                const float lift = c != 0 ? fold * (w / (0.5f * wid)) : 0.0f;
                add(mid + side * (w * float(c)) + v3(0, lift, 0), v3(0, 1, 0), float(c), t, 0.0f, leaf);
            }
        }
        for (int r = 0; r < R; ++r) {
            const uint32_t a = uint32_t(v0 + 3 * r);
            for (uint32_t c = 0; c < 2; ++c) {
                tri(a + c, a + 3 + c, a + 4 + c);
                tri(a + c, a + 4 + c, a + 1 + c);
            }
        }
        smooth(v0, i0);
    };
    // A berry hanging from `top` along `axis`: rings round a conical body
    // with rounded shoulders, its normals outward, uv = (around, along).
    auto berry_body = [&](V3 top, V3 axis, float len, float rad) {
        const size_t v0 = vtx.size();
        V3 a1 = cross(axis, v3(1, 0, 0));
        if (dot(a1, a1) < 1e-6f) a1 = v3(0, 0, 1);
        a1 = norm(a1);
        const V3 a2 = norm(cross(axis, a1));
        const int S = 10, Rr = 8;
        add(top, axis * -1.0f, 0.0f, 0.0f, 2.0f, berry);
        for (int r = 1; r <= Rr; ++r) {
            const float t = float(r) / float(Rr);
            const float shoulder = std::min(1.0f, 0.55f + 3.0f * t);
            const float rr = rad * shoulder * std::pow(std::max(0.0f, 1.0f - std::pow(t, 1.7f)), 0.6f) + 1e-4f;
            for (int s = 0; s < S; ++s) {
                const float ang = 2.0f * kPi * float(s) / float(S);
                const V3 o = a1 * (std::cos(ang) * rr) + a2 * (std::sin(ang) * rr);
                add(top + axis * (len * t) + o, norm(o + axis * (0.6f * (t - 0.5f) * rad)), float(s) / float(S), t, 2.0f, berry);
            }
        }
        for (int s = 0; s < S; ++s) tri(uint32_t(v0), uint32_t(v0 + 1 + s), uint32_t(v0 + 1 + (s + 1) % S));
        for (int r = 0; r + 1 < Rr; ++r) {
            const uint32_t a = uint32_t(v0 + 1 + r * S), b = a + uint32_t(S);
            for (int s = 0; s < S; ++s) {
                const uint32_t s1 = uint32_t((s + 1) % S);
                tri(a + uint32_t(s), b + uint32_t(s), b + s1);
                tri(a + uint32_t(s), b + s1, a + s1);
            }
        }
    };
    // n pointed leaves in a star round `at`, square to `axis`, their tips
    // bent back by `flare`: a berry's sepals, a flower's petals.
    auto star = [&](V3 at, V3 axis, float r, int n, float kind, const float* c, float flare, Rng& rng) {
        V3 a1 = cross(axis, v3(1, 0, 0));
        if (dot(a1, a1) < 1e-6f) a1 = v3(0, 0, 1);
        a1 = norm(a1);
        const V3 a2 = norm(cross(axis, a1));
        const float off = rng.in(0.0f, 2.0f * kPi);
        const float wide = kind > 3.5f ? 0.55f : 0.30f;
        const size_t v0 = vtx.size(), i0 = idx.size();
        for (int k = 0; k < n; ++k) {
            const float ang = off + 2.0f * kPi * float(k) / float(n);
            const V3 d = a1 * std::cos(ang) + a2 * std::sin(ang);
            const V3 s = a1 * -std::sin(ang) + a2 * std::cos(ang);
            const uint32_t b = add(at, axis, 0, 0, kind, c);
            const V3 mid = at + d * (r * 0.55f) + axis * (-flare * r * 0.4f);
            const uint32_t l = add(mid - s * (r * wide), axis, -1, 0.55f, kind, c);
            const uint32_t q = add(mid + s * (r * wide), axis, 1, 0.55f, kind, c);
            const uint32_t t = add(at + d * r + axis * (-flare * r), axis, 0, 1, kind, c);
            tri(b, l, t);
            tri(b, t, q);
        }
        smooth(v0, i0);
    };

    for (int var = 0; var < kVariants; ++var) {
        Rng rng(0x5354524157ull * uint64_t(var + 1) ^ uint64_t(std::hash<std::string>()(sp.name)));
        // --- leaves ---
        Model m;
        m.first = uint32_t(idx.size());
        const int nl = 7 + var;
        float leaf_ang[12];
        for (int k = 0; k < nl; ++k) {
            const float a = 2.0f * kPi * (float(k) + rng.in(-0.25f, 0.25f)) / float(nl);
            leaf_ang[k] = a;
            const V3 out = v3(std::cos(a), 0, std::sin(a));
            const float rp = rng.in(0.07f, 0.13f), hp = rng.in(0.13f, 0.21f);
            const V3 tip = out * rp + v3(0, hp, 0);
            ribbon(bezier(v3(0, 0, 0), out * (0.15f * rp) + v3(0, hp * 0.9f, 0), tip, 5), 0.010f, 0.006f, stalk);
            // Three leaflets: the middle one straight out, the side ones turned.
            const float pitch = rng.in(-0.35f, -0.05f);
            for (int q = -1; q <= 1; ++q) {
                const float b = a + float(q) * rng.in(0.65f, 0.85f);
                const V3 d = norm(v3(std::cos(b), pitch + (q != 0 ? -0.08f : 0.0f), std::sin(b)));
                const float len = (q == 0 ? 0.105f : 0.092f) * rng.in(0.9f, 1.1f);
                leaflet(tip, d, len, len * 0.68f, rng);
            }
        }
        m.count = uint32_t(idx.size()) - m.first;
        models_.push_back(m);

        // --- berries, at the rim between the leaves ---
        Model mb;
        mb.first = uint32_t(idx.size());
        const int nb = 5 + int(rng.uni() * 3.0f);
        std::vector<V3> spots;
        for (int k = 0; k < nb; ++k) {
            const float a = leaf_ang[k % nl] + kPi / float(nl) + rng.in(-0.2f, 0.2f);
            const V3 out = v3(std::cos(a), 0, std::sin(a));
            const float rb = rng.in(0.14f, 0.22f), hb = rng.in(0.07f, 0.12f);
            const V3 top = out * rb + v3(0, hb, 0);
            spots.push_back(top);
            ribbon(bezier(v3(0, 0.01f, 0), out * (0.45f * rb) + v3(0, hb + 0.07f, 0), top, 5), 0.006f, 0.004f, stalk);
            const V3 axis = norm(out * rng.in(0.4f, 0.8f) + v3(0, -1, 0));
            const float len = rng.in(0.042f, 0.056f);
            berry_body(top, axis, len, len * rng.in(0.42f, 0.5f));
            star(top + axis * 0.004f, axis, len * 0.55f, 5, 3.0f, calyx, 0.6f, rng);
        }
        mb.count = uint32_t(idx.size()) - mb.first;
        models_.push_back(mb);

        // --- flowers, while it grows back ---
        Model mf;
        mf.first = uint32_t(idx.size());
        for (size_t k = 0; k < spots.size() && k < 4; ++k) {
            const V3 top = spots[k] + v3(0, 0.05f, 0);
            const V3 out = norm(v3(spots[k].x, 0, spots[k].z));
            ribbon(bezier(v3(0, 0.01f, 0), out * 0.05f + v3(0, top.y + 0.05f, 0), top, 5), 0.005f, 0.004f, stalk);
            const V3 face = norm(out * 0.5f + v3(0, 1, 0));
            star(top, face, 0.034f, 5, 4.0f, flower, -0.15f, rng);
            star(top + face * 0.004f, face, 0.011f, 6, 5.0f, middle, 0.0f, rng);
        }
        mf.count = uint32_t(idx.size()) - mf.first;
        models_.push_back(mf);
    }
}

bool Forage::init(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12RootSignature* mesh_root,
                  const char* archetypes_path, std::string& error) {
    if (!load_config(archetypes_path, error)) return false;
    if (species_.empty()) return true;   // nothing to forage: not ready, nothing drawn
    std::vector<Vertex> verts;
    std::vector<uint32_t> idx;
    models_.clear();
    for (const Species& s : species_) build_models(s, verts, idx);

    const UINT64 vbytes = verts.size() * sizeof(Vertex), ibytes = idx.size() * 4;
    vb_ = buffer(device, vbytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ib_ = buffer(device, ibytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
    ComPtr<ID3D12Resource> up = buffer(device, vbytes + ibytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!vb_ || !ib_ || !up) { error = "forage mesh buffers"; return false; }
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
        if (FAILED(hr)) { error = hr_text("forage upload setup", hr); return false; }
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
        inst_[f] = buffer(device, UINT64(kMaxDrawn) * sizeof(ForageGpu), D3D12_HEAP_TYPE_UPLOAD,
                          D3D12_RESOURCE_STATE_GENERIC_READ);
        if (!inst_[f]) { error = "forage instance buffer"; return false; }
        inst_[f]->Map(0, nullptr, reinterpret_cast<void**>(&inst_cpu_[f]));
    }

    ComPtr<ID3DBlob> vs, ps;
    if (!compile_shader(kShader, "vs_forage", "vs_5_1", vs, error)) return false;
    if (!compile_shader(kShader, "ps_forage", "ps_5_1", ps, error)) return false;
    const D3D12_INPUT_ELEMENT_DESC layout[5] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = mesh_root;
    pd.InputLayout = {layout, 5};
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
    if (FAILED(hr)) { error = hr_text("CreateGraphicsPipelineState (forage)", hr); return false; }
    ready_ = true;
    return true;
}

void Forage::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : threads_) t.join();
    threads_.clear();
}

void Forage::set_world(const worldgen::SeedWorld* w, const worldgen::WorldParams* wp,
                       const worldgen::ArchetypeTable* table) {
    shutdown();
    world_ = w;
    wp_ = wp;
    table_ = table;
    tiles_.clear();
    plants_.clear();
    picked_.clear();
    queue_.clear();
    done_.clear();
    in_flight_.clear();
    last_cx_ = last_cz_ = 1 << 30;
    stop_ = false;
    if (world_ && table_ && table_->forage.present && !table_->forage.species.empty())
        threads_.push_back(std::thread(&Forage::worker, this));
}

void Forage::worker() {
    for (;;) {
        TileKey k;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            k = queue_.front();
            queue_.pop_front();
            in_flight_.insert(k);
        }
        std::vector<worldgen::ForageInstance> out;
        // Each species' cells whose ORIGIN lies in the tile: one tile owns each.
        const float x0 = k.x * kTileM, x1 = x0 + kTileM, z0 = k.z * kTileM, z1 = z0 + kTileM;
        for (size_t s = 0; s < table_->forage.species.size(); ++s) {
            const float c = table_->forage.species[s].cell;
            const int i0 = int(std::ceil(x0 / c)), i1 = int(std::ceil(x1 / c));
            const int j0 = int(std::ceil(z0 / c)), j1 = int(std::ceil(z1 / c));
            if (i1 > i0 && j1 > j0) worldgen::forage_in_cells(*world_, *wp_, *table_, int(s), i0, j0, i1, j1, out);
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            in_flight_.erase(k);
            done_.push_back(std::make_pair(k, std::move(out)));
        }
    }
}

bool Forage::update(float x, float z, bool settle, const BoxFn& on_new) {
    if (threads_.empty()) return false;
    bool changed = false;
    for (int pass = 0; pass < 4000; ++pass) {
        const int cx = int(std::floor(x / kTileM)), cz = int(std::floor(z / kTileM));
        const int reach = int(std::ceil(kStreamRadius / kTileM)) + 1;
        {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& d : done_) {
                if (on_new) {
                    for (const worldgen::ForageInstance& f : d.second) {
                        const float lo[3] = {f.x - 1.0f, f.y - 2.0f, f.z - 1.0f};
                        const float hi[3] = {f.x + 1.0f, f.y + 2.0f, f.z + 1.0f};
                        on_new(lo, hi);
                    }
                }
                tiles_[d.first] = std::move(d.second);
                changed = true;
            }
            done_.clear();
        }
        if (cx != last_cx_ || cz != last_cz_) {
            last_cx_ = cx;
            last_cz_ = cz;
            std::vector<std::pair<float, TileKey> > want;
            for (int j = cz - reach; j <= cz + reach; ++j) {
                for (int i = cx - reach; i <= cx + reach; ++i) {
                    const float tx = (i + 0.5f) * kTileM - x, tz = (j + 0.5f) * kTileM - z;
                    const float d = std::sqrt(tx * tx + tz * tz) - 0.7072f * kTileM;
                    if (d > kStreamRadius) continue;
                    TileKey k = {i, j};
                    if (tiles_.count(k)) continue;
                    want.push_back(std::make_pair(d, k));
                }
            }
            std::sort(want.begin(), want.end(), [](const std::pair<float, TileKey>& a, const std::pair<float, TileKey>& b) {
                return a.first < b.first;
            });
            {
                std::lock_guard<std::mutex> lk(mu_);
                queue_.clear();
                for (const auto& w : want)
                    if (!in_flight_.count(w.second)) queue_.push_back(w.second);
            }
            cv_.notify_all();
            for (auto it = tiles_.begin(); it != tiles_.end();) {
                const float tx = (it->first.x + 0.5f) * kTileM - x, tz = (it->first.z + 0.5f) * kTileM - z;
                if (std::sqrt(tx * tx + tz * tz) > kStreamRadius + 2.2f * kTileM) {
                    it = tiles_.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            settled_ = queue_.empty() && in_flight_.empty() && done_.empty();
        }
        if (!settle || settled_) break;
        Sleep(2);
    }
    if (changed) rebuild();
    return changed;
}

void Forage::rebuild() {
    plants_.clear();
    for (const auto& t : tiles_) plants_.insert(plants_.end(), t.second.begin(), t.second.end());
}

float Forage::scale_of(const worldgen::ForageInstance& f) const {
    const float s = f.species < species_.size() ? species_[f.species].size : 0.6f;
    return (0.85f + 0.35f * f.size01) * s / 0.6f;
}

float Forage::pick_height(const worldgen::ForageInstance& f) const { return 0.12f * scale_of(f); }

bool Forage::ripe(uint32_t id, double now) const {
    const auto it = picked_.find(id);
    if (it == picked_.end()) return true;
    const int s = int(id >> 28);
    const double regrow = (s < int(species_.size()) ? species_[size_t(s)].regrow_days : 3.0f) * kDay;
    return now - it->second >= regrow;
}

double Forage::ripe_at(uint32_t id) const {
    const auto it = picked_.find(id);
    if (it == picked_.end()) return 0.0;
    const int s = int(id >> 28);
    return it->second + (s < int(species_.size()) ? species_[size_t(s)].regrow_days : 3.0f) * kDay;
}

void Forage::prepare(UINT frame, const float cam[3], const float view_proj[16], double now) {
    groups_[frame].clear();
    drawn_ = 0;
    if (!ready_ || plants_.empty()) return;
    float planes[6][4];
    frustum_planes(view_proj, planes);
    // Which plants are in view, once.
    std::vector<const worldgen::ForageInstance*> vis;
    std::vector<char> ripe_v;
    for (const worldgen::ForageInstance& f : plants_) {
        const float dx = f.x - cam[0], dz = f.z - cam[2];
        if (dx * dx + dz * dz > kDrawDist * kDrawDist) continue;
        const float r = 0.5f * scale_of(f);
        bool in = true;
        for (int p = 0; p < 6 && in; ++p)
            if (planes[p][0] * f.x + planes[p][1] * (f.y + 0.15f) + planes[p][2] * f.z + planes[p][3] < -r) in = false;
        if (!in) continue;
        vis.push_back(&f);
        ripe_v.push_back(ripe(f.id, now) ? 1 : 0);
    }
    ForageGpu* out = inst_cpu_[frame];
    size_t n = 0;
    for (uint32_t m = 0; m < uint32_t(models_.size()); ++m) {
        const int species = int(m) / (kVariants * kParts);
        const int variant = (int(m) / kParts) % kVariants;
        const int part = int(m) % kParts;
        const size_t first = n;
        for (size_t k = 0; k < vis.size() && n < kMaxDrawn; ++k) {
            const worldgen::ForageInstance& f = *vis[k];
            if (int(f.species) != species || int(f.variant) % kVariants != variant) continue;
            if (part == kBerries && !ripe_v[k]) continue;
            if (part == kFlowers && ripe_v[k]) continue;
            ForageGpu g = {};
            g.pos[0] = f.x; g.pos[1] = f.y - 0.01f; g.pos[2] = f.z;
            g.scale = scale_of(f);
            const float yaw = f.yaw01 * 2.0f * kPi;
            g.rot[0] = 0.0f; g.rot[1] = std::sin(0.5f * yaw); g.rot[2] = 0.0f; g.rot[3] = std::cos(0.5f * yaw);
            // Each plant its own green, a little.
            const uint64_t h = mix64(f.id);
            const float t = float(h & 0xFFFF) / 65535.0f, u = float((h >> 16) & 0xFFFF) / 65535.0f;
            g.tint[0] = 0.85f + 0.35f * t;
            g.tint[1] = 0.90f + 0.20f * u;
            g.tint[2] = 0.80f + 0.30f * (1.0f - t);
            g.fade = kDrawDist;
            out[n++] = g;
        }
        if (n > first) groups_[frame].push_back(Group{m, uint32_t(first), uint32_t(n - first)});
    }
    drawn_ = n;
}

void Forage::record(ID3D12GraphicsCommandList* cmd, UINT frame) {
    if (!ready_ || groups_[frame].empty()) return;
    cmd->SetPipelineState(pso_.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->IASetVertexBuffers(0, 1, &vbv_);
    cmd->IASetIndexBuffer(&ibv_);
    const D3D12_GPU_VIRTUAL_ADDRESS va = inst_[frame]->GetGPUVirtualAddress();
    for (const Group& g : groups_[frame]) {
        const Model& m = models_[g.model];
        if (m.count == 0) continue;
        cmd->SetGraphicsRootShaderResourceView(1, va + UINT64(g.first) * sizeof(ForageGpu));
        cmd->DrawIndexedInstanced(m.count, g.count, m.first, 0, 0);
    }
}

bool Forage::aim(const float eye[3], const float dir[3], const float hand[3], float reach, float occluder,
                 double now, Hit& out) const {
    float best = 1e30f;
    for (const worldgen::ForageInstance& f : plants_) {
        const float c[3] = {f.x, f.y + pick_height(f), f.z};
        float hand2 = 0.0f;
        for (int k = 0; k < 3; ++k) hand2 += (c[k] - hand[k]) * (c[k] - hand[k]);
        if (hand2 > reach * reach) continue;
        const float t = (c[0] - eye[0]) * dir[0] + (c[1] - eye[1]) * dir[1] + (c[2] - eye[2]) * dir[2];
        // The plant sits on the ground the ray meets: allow a little past it.
        if (t <= 0.0f || t > occluder + 0.5f) continue;
        float perp2 = 0.0f;
        for (int k = 0; k < 3; ++k) {
            const float q = c[k] - (eye[k] + dir[k] * t);
            perp2 += q * q;
        }
        const float tol = 0.32f * scale_of(f);
        if (perp2 > tol * tol) continue;
        if (!ripe(f.id, now)) continue;
        const float score = t + 3.0f * std::sqrt(perp2);
        if (score < best) {
            best = score;
            out.id = f.id;
            out.species = f.species;
            out.pos[0] = c[0]; out.pos[1] = c[1]; out.pos[2] = c[2];
            out.dist = t;
        }
    }
    return best < 1e29f;
}

int Forage::pick(uint32_t id, double now) {
    if (!ripe(id, now)) return 0;
    for (const worldgen::ForageInstance& f : plants_) {
        if (f.id != id) continue;
        if (f.species >= species_.size()) return 0;
        const Species& s = species_[f.species];
        picked_[id] = now;
        const int span = s.yield_max - s.yield_min + 1;
        return s.yield_min + int(mix64(uint64_t(id) * 0x9E37ull) % uint64_t(span));
    }
    return 0;
}

void Forage::clear_tops(int sx0, int sz0, float* tops) const {
    const float x0 = float(sx0) * 0.5f, z0 = float(sz0) * 0.5f;
    const float r = kClearRadius + 0.25f;
    for (const worldgen::ForageInstance& f : plants_) {
        if (f.x < x0 - r || f.x > x0 + 16.0f + r || f.z < z0 - r || f.z > z0 + 16.0f + r) continue;
        const float rr = r * scale_of(f);
        const int hx0 = std::max(0, int(std::floor((f.x - rr - x0) * 2.0f)));
        const int hx1 = std::min(31, int(std::floor((f.x + rr - x0) * 2.0f)));
        const int hz0 = std::max(0, int(std::floor((f.z - rr - z0) * 2.0f)));
        const int hz1 = std::min(31, int(std::floor((f.z + rr - z0) * 2.0f)));
        for (int hz = hz0; hz <= hz1; ++hz)
            for (int hx = hx0; hx <= hx1; ++hx) {
                const float cx = x0 + (float(hx) + 0.5f) * 0.5f - f.x, cz = z0 + (float(hz) + 0.5f) * 0.5f - f.z;
                if (cx * cx + cz * cz <= rr * rr) tops[hz * 32 + hx] = 1.0e30f;
            }
    }
}

bool Forage::nearest(const float p[3], bool ripe_only, double now, worldgen::ForageInstance& out) const {
    float bd = 1e30f;
    for (const worldgen::ForageInstance& f : plants_) {
        if (ripe_only && !ripe(f.id, now)) continue;
        const float dx = f.x - p[0], dz = f.z - p[2];
        const float d = dx * dx + dz * dz;
        if (d < bd) { bd = d; out = f; }
    }
    return bd < 1e29f;
}

bool Forage::save(const char* path, uint64_t field_id, std::string& error) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) { error = std::string("cannot write ") + path; return false; }
    const char magic[4] = {'F', 'R', 'G', '1'};
    const uint32_t n = uint32_t(picked_.size());
    std::fwrite(magic, 1, 4, f);
    std::fwrite(&field_id, 8, 1, f);
    std::fwrite(&n, 4, 1, f);
    for (const auto& p : picked_) {
        std::fwrite(&p.first, 4, 1, f);
        std::fwrite(&p.second, 8, 1, f);
    }
    std::fclose(f);
    return true;
}

bool Forage::load(const char* path, uint64_t field_id, std::string& error) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { error = std::string("no ") + path; return false; }
    char magic[4] = {};
    uint64_t fid = 0;
    uint32_t n = 0;
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "FRG1", 4) == 0 &&
              std::fread(&fid, 8, 1, f) == 1 && std::fread(&n, 4, 1, f) == 1;
    if (ok && fid != field_id) {
        error = std::string(path) + " is another world's";
        ok = false;
    }
    if (ok) {
        picked_.clear();
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t id = 0;
            double t = 0.0;
            if (std::fread(&id, 4, 1, f) != 1 || std::fread(&t, 8, 1, f) != 1) break;
            picked_[id] = t;
        }
    } else if (error.empty()) {
        error = std::string(path) + " is not a forage save";
    }
    std::fclose(f);
    return ok;
}

}  // namespace engine
