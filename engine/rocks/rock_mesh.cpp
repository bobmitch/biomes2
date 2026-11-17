#include "engine/rocks/rock_mesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace engine {

namespace {

struct V3 { float x, y, z; };
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) { const float l = std::sqrt(dot(a, a)); return l > 0.0f ? mul(a, 1.0f / l) : a; }

uint32_t hash(uint32_t x) {
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
float unit(uint32_t h) { return float(h >> 8) * (1.0f / 16777216.0f); }

// Smooth 3D value noise and a few octaves of it, in [-1, 1].
float lattice(int x, int y, int z, uint32_t seed) {
    return unit(hash(uint32_t(x) * 0x8DA6B343u ^ uint32_t(y) * 0xD8163841u ^ uint32_t(z) * 0xCB1AB31Fu ^ seed)) * 2.0f - 1.0f;
}
float vnoise(V3 p, uint32_t seed) {
    const int ix = int(std::floor(p.x)), iy = int(std::floor(p.y)), iz = int(std::floor(p.z));
    const float fx = p.x - ix, fy = p.y - iy, fz = p.z - iz;
    const float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy), uz = fz * fz * (3 - 2 * fz);
    float c[2][2][2];
    for (int a = 0; a < 2; ++a) for (int b = 0; b < 2; ++b) for (int d = 0; d < 2; ++d)
        c[a][b][d] = lattice(ix + a, iy + b, iz + d, seed);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = lerp(c[0][0][0], c[1][0][0], ux), x10 = lerp(c[0][1][0], c[1][1][0], ux);
    const float x01 = lerp(c[0][0][1], c[1][0][1], ux), x11 = lerp(c[0][1][1], c[1][1][1], ux);
    return lerp(lerp(x00, x10, uy), lerp(x01, x11, uy), uz);
}
float fbm(V3 p, uint32_t seed, int oct) {
    float s = 0.0f, a = 0.5f;
    for (int o = 0; o < oct; ++o) { s += a * vnoise(p, seed + uint32_t(o) * 977u); p = mul(p, 2.03f); a *= 0.5f; }
    return s;
}

// An icosphere: unit directions and triangles, `sub` subdivisions.
void icosphere(int subdiv, std::vector<V3>& dirs, std::vector<uint32_t>& tris) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    dirs = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
            {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (V3& d : dirs) d = norm(d);
    tris = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
            3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (int s = 0; s < subdiv; ++s) {
        std::map<std::pair<uint32_t, uint32_t>, uint32_t> mid;
        auto midpoint = [&](uint32_t a, uint32_t b) {
            const std::pair<uint32_t, uint32_t> k(std::min(a, b), std::max(a, b));
            auto it = mid.find(k);
            if (it != mid.end()) return it->second;
            dirs.push_back(norm(add(dirs[a], dirs[b])));
            const uint32_t i = uint32_t(dirs.size() - 1);
            mid[k] = i;
            return i;
        };
        std::vector<uint32_t> nt;
        nt.reserve(tris.size() * 4);
        for (size_t f = 0; f < tris.size(); f += 3) {
            const uint32_t a = tris[f], b = tris[f + 1], c = tris[f + 2];
            const uint32_t ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
            const uint32_t q[12] = {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca};
            nt.insert(nt.end(), q, q + 12);
        }
        tris.swap(nt);
    }
}

// The shape of one model: where the surface lies along unit direction d.
struct Shape {
    int kind;
    uint32_t seed;
    std::vector<std::pair<V3, float> > planes;   // normal, distance
    V3 stretch;

    explicit Shape(int k, uint32_t s) : kind(k), seed(s) {
        uint32_t h = hash(s);
        auto next = [&]() { h = hash(h + 0x9E3779B9u); return unit(h); };
        const int np = kind == 0 ? 7 + int(next() * 4.0f) : 5 + int(next() * 3.0f);
        for (int i = 0; i < np; ++i) {
            V3 n;
            do { n = {next() * 2 - 1, next() * 2 - 1, next() * 2 - 1}; } while (dot(n, n) > 1.0f || dot(n, n) < 0.05f);
            n = norm(n);
            // Mostly side and top facets: a downward cut would be buried.
            if (n.y < -0.3f) n.y = -n.y * 0.5f;
            n = norm(n);
            planes.push_back(std::make_pair(n, kind == 0 ? 0.34f + 0.12f * next() : 0.38f + 0.1f * next()));
        }
        stretch = kind == 0 ? V3{0.95f + 0.15f * next(), 0.62f + 0.18f * next(), 0.85f + 0.15f * next()}
                            : V3{1.0f, 0.7f + 0.15f * next(), 0.55f + 0.15f * next()};
    }

    V3 point(V3 d) const {
        float r = 0.5f * (1.0f + 0.12f * fbm(mul(d, 2.2f), seed, 3));
        for (const auto& pl : planes) {
            const float c = dot(d, pl.first);
            if (c > 1e-3f) r = std::min(r, pl.second / c);
        }
        V3 p = mul(d, r);
        p = {p.x * stretch.x, p.y * stretch.y, p.z * stretch.z};
        if (kind == 1) {
            // Strata: a sawtooth of ledges up the face, slightly tilted.
            const float s = (p.y + 0.08f * p.x) * 7.0f + 0.6f * fbm(mul(p, 3.0f), seed + 77u, 2);
            const float saw = s - std::floor(s);
            const float k = 1.0f + 0.07f * (saw - 0.5f);
            p.x *= k;
            p.z *= k;
        }
        // Fine roughness, along the direction.
        const float rough = 1.0f + 0.035f * fbm(mul(p, 9.0f), seed + 31u, 3);
        return mul(p, rough);
    }
};

void emit_level(const Shape& sh, int subdiv, RockMeshes& out, uint32_t& first, uint32_t& count,
                uint32_t& vfirst, uint32_t& vcount, float& base_y, bool set_base) {
    std::vector<V3> dirs;
    std::vector<uint32_t> tris;
    icosphere(subdiv, dirs, tris);
    std::vector<V3> pos(dirs.size());
    float lo = 1e9f;
    for (size_t i = 0; i < dirs.size(); ++i) {
        pos[i] = sh.point(dirs[i]);
        lo = std::min(lo, pos[i].y);
    }
    if (set_base) base_y = lo;   // both levels share the near level's base
    std::vector<V3> nrm(dirs.size(), V3{0, 0, 0});
    for (size_t f = 0; f < tris.size(); f += 3) {
        const V3 a = pos[tris[f]], b = pos[tris[f + 1]], c = pos[tris[f + 2]];
        const V3 fn = cross(sub(b, a), sub(c, a));
        for (int k = 0; k < 3; ++k) nrm[tris[f + k]] = add(nrm[tris[f + k]], fn);
    }
    vfirst = uint32_t(out.v.size());
    vcount = uint32_t(dirs.size());
    for (size_t i = 0; i < dirs.size(); ++i) {
        RockVertex rv;
        const V3 p = {pos[i].x, pos[i].y - base_y, pos[i].z};
        const V3 n = norm(nrm[i]);
        rv.pos[0] = p.x; rv.pos[1] = p.y; rv.pos[2] = p.z;
        rv.nrm[0] = n.x; rv.nrm[1] = n.y; rv.nrm[2] = n.z;
        out.v.push_back(rv);
    }
    first = uint32_t(out.idx.size());
    count = uint32_t(tris.size());
    // Outward winding: icosphere faces are counter-clockwise seen from
    // outside; D3D's default front face is clockwise, so flip each.
    for (size_t f = 0; f < tris.size(); f += 3) {
        out.idx.push_back(vfirst + tris[f]);
        out.idx.push_back(vfirst + tris[f + 2]);
        out.idx.push_back(vfirst + tris[f + 1]);
    }
}

}  // namespace

void build_rock_meshes(RockMeshes& out) {
    out = RockMeshes();
    const uint32_t nm = kBoulderModels + kOutcropModels;
    for (uint32_t m = 0; m < nm; ++m) {
        const int kind = m < kBoulderModels ? 0 : 1;
        const Shape sh(kind, 0xA5A5u + m * 7919u);
        RockModel rm = {};
        float base_y = 0.0f;
        emit_level(sh, kind == 0 ? 3 : 4, out, rm.first[0], rm.count[0], rm.vfirst[0], rm.vcount[0], base_y, true);
        emit_level(sh, 2, out, rm.first[1], rm.count[1], rm.vfirst[1], rm.vcount[1], base_y, false);
        float top = 0.0f, reach = 0.0f;
        for (uint32_t i = rm.vfirst[0]; i < rm.vfirst[0] + rm.vcount[0]; ++i) {
            const RockVertex& v = out.v[i];
            top = std::max(top, v.pos[1]);
            reach = std::max(reach, std::sqrt(v.pos[0] * v.pos[0] + v.pos[2] * v.pos[2]));
        }
        rm.height = top;
        rm.radius = reach;
        out.models.push_back(rm);
    }
}

}  // namespace engine
