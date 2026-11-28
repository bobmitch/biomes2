#include "engine/trees/tree_mesh.h"
#include "worldgen/assign/json_lite.h"

#include <algorithm>
#include <cmath>

namespace engine {
namespace {

const float kPi = 3.14159265f;

struct V3 {
    float x, y, z;
    V3() : x(0), y(0), z(0) {}
    V3(float a, float b, float c) : x(a), y(b), z(c) {}
    V3 operator+(const V3& o) const { return V3(x + o.x, y + o.y, z + o.z); }
    V3 operator-(const V3& o) const { return V3(x - o.x, y - o.y, z - o.z); }
    V3 operator*(float s) const { return V3(x * s, y * s, z * s); }
};
float dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(const V3& a, const V3& b) {
    return V3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
float len(const V3& a) { return std::sqrt(dot(a, a)); }
V3 norm(const V3& a) {
    const float l = len(a);
    return l > 1e-8f ? a * (1.0f / l) : V3(0, 1, 0);
}
float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

// xorshift32: presentation only, never hashed.
struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) { for (int i = 0; i < 4; ++i) next(); }
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() { return float(next() >> 8) * (1.0f / 16777216.0f); }
    float range(float a, float b) { return a + (b - a) * uni(); }
};

// A unit vector perpendicular to d.
V3 perp(const V3& d) {
    const V3 a = std::fabs(d.y) < 0.9f ? V3(0, 1, 0) : V3(1, 0, 0);
    return norm(cross(d, a));
}

// Rotate v about unit axis k by angle a (Rodrigues).
V3 rotate(const V3& v, const V3& k, float a) {
    const float c = std::cos(a), s = std::sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0f - c));
}

struct Node {
    V3    p;
    float r;
    float sway;
    float ao;
};

class Builder {
public:
    Builder(TreeMesh& m) : m_(m) {}

    // A tube through the nodes with `sides` sides, frames parallel-transported
    // so it does not twist. v runs along the length in metres.
    void tube(const std::vector<Node>& n, int sides) {
        if (n.size() < 2 || sides < 3) return;
        const uint32_t base = static_cast<uint32_t>(m_.v.size());
        V3 t0 = norm(n[1].p - n[0].p);
        V3 u = perp(t0);
        float vlen = 0.0f;
        for (size_t i = 0; i < n.size(); ++i) {
            V3 t;
            if (i == 0) t = t0;
            else if (i + 1 == n.size()) t = norm(n[i].p - n[i - 1].p);
            else t = norm(n[i + 1].p - n[i - 1].p);
            // Parallel transport: remove u's component along the new tangent.
            u = norm(u - t * dot(u, t));
            const V3 w = cross(t, u);
            if (i > 0) vlen += len(n[i].p - n[i - 1].p);
            for (int s = 0; s <= sides; ++s) {
                const float a = 2.0f * kPi * float(s) / float(sides);
                const V3 dir = u * std::cos(a) + w * std::sin(a);
                TreeVertex v = {};
                const V3 p = n[i].p + dir * n[i].r;
                v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
                v.nrm[0] = dir.x; v.nrm[1] = dir.y; v.nrm[2] = dir.z;
                // u in metres round the circumference, so bark is not
                // stretched on thin branches; v in metres along.
                v.uv[0] = float(s) / float(sides) * 2.0f * kPi * std::max(n[i].r, 0.05f);
                v.uv[1] = vlen;
                v.sway = n[i].sway;
                v.ao = n[i].ao;
                v.kind = float(kKindBark);
                v.seed = 0.0f;
                m_.v.push_back(v);
            }
        }
        const uint32_t ring = static_cast<uint32_t>(sides + 1);
        for (uint32_t i = 0; i + 1 < n.size(); ++i) {
            for (uint32_t s = 0; s < static_cast<uint32_t>(sides); ++s) {
                const uint32_t a = base + i * ring + s, b = a + 1;
                const uint32_t c = a + ring, d = c + 1;
                m_.idx.push_back(a); m_.idx.push_back(c); m_.idx.push_back(b);
                m_.idx.push_back(b); m_.idx.push_back(c); m_.idx.push_back(d);
            }
        }
    }

    // A flat disc at the cut, for the stump's top or the log's end.
    void cap(const V3& c, float r, int sides, bool up) {
        const uint32_t base = static_cast<uint32_t>(m_.v.size());
        TreeVertex ctr = {};
        ctr.pos[0] = c.x; ctr.pos[1] = c.y; ctr.pos[2] = c.z;
        ctr.nrm[1] = up ? 1.0f : -1.0f;
        ctr.uv[0] = 0.0f; ctr.uv[1] = 0.0f;   // ring distance 0 at the pith
        ctr.ao = 1.0f;
        ctr.kind = float(up ? kKindCapUp : kKindCapDown);
        ctr.seed = r;                          // the shader reads radius from it
        m_.v.push_back(ctr);
        for (int s = 0; s <= sides; ++s) {
            const float a = 2.0f * kPi * float(s) / float(sides);
            TreeVertex v = ctr;
            v.pos[0] = c.x + std::cos(a) * r;
            v.pos[2] = c.z + std::sin(a) * r;
            v.uv[0] = r;   // distance from the pith, for the growth rings
            m_.v.push_back(v);
        }
        for (int s = 0; s < sides; ++s) {
            const uint32_t a = base + 1 + s, b = a + 1;
            if (up) { m_.idx.push_back(base); m_.idx.push_back(b); m_.idx.push_back(a); }
            else    { m_.idx.push_back(base); m_.idx.push_back(a); m_.idx.push_back(b); }
        }
    }

    // A leaf card: centre, half-extent vectors, the normal it is lit with.
    void card(const V3& c, const V3& hu, const V3& hv, const V3& light_n, uint32_t kind,
              float seed, float sway, float ao) {
        const uint32_t base = static_cast<uint32_t>(m_.v.size());
        const V3 corners[4] = {c - hu - hv, c + hu - hv, c + hu + hv, c - hu + hv};
        const float uvs[4][2] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        for (int k = 0; k < 4; ++k) {
            TreeVertex v = {};
            v.pos[0] = corners[k].x; v.pos[1] = corners[k].y; v.pos[2] = corners[k].z;
            v.nrm[0] = light_n.x; v.nrm[1] = light_n.y; v.nrm[2] = light_n.z;
            v.uv[0] = uvs[k][0]; v.uv[1] = uvs[k][1];
            // The card's far edge moves more than the edge on the twig.
            v.sway = std::min(1.0f, sway + (k >= 2 ? 0.15f : 0.0f));
            v.ao = ao;
            v.kind = float(kind);
            v.seed = seed;
            m_.v.push_back(v);
        }
        m_.idx.push_back(base); m_.idx.push_back(base + 1); m_.idx.push_back(base + 2);
        m_.idx.push_back(base); m_.idx.push_back(base + 2); m_.idx.push_back(base + 3);
    }

    // For shapes built by hand (root_flare).
    TreeMesh& mesh() { return m_; }

private:
    TreeMesh& m_;
};

// The crown as an ellipsoid about (0, cy, 0): how far out a point is, 0 at the
// centre and 1 on the surface. Drives leaf normals and occlusion.
struct Crown {
    float cy, rx, ry;
    float depth(const V3& p) const {
        const float dx = p.x / rx, dy = (p.y - cy) / ry, dz = p.z / rx;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    V3 outward(const V3& p) const {
        return norm(V3(p.x / (rx * rx), (p.y - cy) / (ry * ry), p.z / (rx * rx)));
    }
    // Shrink the segment s + d*L until its end is inside.
    float fit(const V3& s, const V3& d, float L) const {
        for (int i = 0; i < 12 && depth(s + d * L) > 1.0f; ++i) L *= 0.85f;
        return L;
    }
};

// The trunk, with the cut ring at kCutHeight and the caps there.
// The root flare: where a trunk meets the ground it widens and splits into
// buttresses -- the roots' tops -- and a few of them run out and down into
// the soil. Without it every trunk stood on the ground like a post.
//
// A tube of its own round the trunk's base, with more sides than the trunk
// so its lobes resolve: from 0.6 m under the ground to `hf` up, its radius
// the trunk's times (1 + F(y) x lobe(angle)), F rising concave to `Fmax` at
// the ground, lobe 4-6 ridges of random strength. It is never narrower than
// the trunk it wraps until its top, where it slips just inside, so the two
// never fight. Normals from the lobed section's own slope, so the ridges
// shade. The strongest lobes send a root out along the ground and under it.
// Its randomness comes from a COPY of the tree's generator: every tree keeps
// its branches and only gains a base.
void root_flare(Builder& b, const TreeLook& L, float H, float R0, float top, float top_r_frac, int trunk_sides,
                const std::vector<Node>& trunk, const Rng& tree_rng) {
    if (L.roots <= 0.0f) return;
    Rng rng(tree_rng.s ^ 0x5EED0F1Au);
    // Where the flare meets the trunk it must stay INSIDE it, and the trunk
    // is a polygon: the middle of each flat face is cos(pi / sides) of the
    // radius out -- 5% in at ten sides, 13% at six. A 3% inset let the
    // rounder flare poke out through every face in a row of dark teeth.
    const float inset = 1.0f - std::cos(kPi / float(trunk_sides)) + 0.03f;
    // And the trunk wanders sideways as it rises; the flare follows its axis.
    auto axis_at = [&](float y) {
        for (size_t i = 1; i < trunk.size(); ++i) {
            if (y <= trunk[i].p.y || i + 1 == trunk.size()) {
                const Node& a = trunk[i - 1];
                const Node& c = trunk[i];
                const float t = clamp01((y - a.p.y) / std::max(c.p.y - a.p.y, 1e-3f));
                return V3(a.p.x + (c.p.x - a.p.x) * t, 0.0f, a.p.z + (c.p.z - a.p.z) * t);
            }
        }
        return V3(0, 0, 0);
    };
    auto rmain = [&](float y) {
        const float r = R0 * (1.0f - (1.0f - top_r_frac) * clamp01(y / top));
        const float fl = clamp01(1.0f - y / (0.08f * H + 0.3f));
        return r * (1.0f + 0.35f * fl * fl);
    };
    const float hf = 3.5f * R0 + 0.25f;
    const float Fmax = 0.85f * L.roots;
    const int lobes = 4 + int(rng.uni() * 3.0f);
    const float phase = rng.uni() * 2.0f * kPi;
    float amp[6];
    for (int k = 0; k < 6; ++k) amp[k] = 0.35f + 0.65f * rng.uni();
    auto F = [&](float y) {
        const float u = clamp01(1.0f - y / hf);
        return Fmax * u * u * std::sqrt(u);
    };
    auto lobe = [&](float a) {
        const float x = (a - phase) * float(lobes) / (2.0f * kPi);
        const int k = (int(std::floor(x + 0.5f)) % lobes + lobes) % lobes;
        const float l = 0.5f + 0.5f * std::cos(2.0f * kPi * x);
        return 0.15f + 0.85f * l * l * l * amp[k];
    };
    auto R = [&](float y, float a) {
        const float f = F(y);
        return rmain(std::max(y, 0.0f)) * (1.0f + f * lobe(a) - inset * (1.0f - f / Fmax));
    };
    const int sides = trunk_sides >= 8 ? 24 : 14;
    const float ys[] = {-0.6f, -0.15f, 0.0f, 0.08f, 0.2f, 0.35f, 0.55f, 0.75f, 1.0f, 1.3f, 1.7f, 2.2f};
    std::vector<float> rings;
    for (float y : ys) if (y < hf) rings.push_back(y);
    rings.push_back(hf);
    const uint32_t base = static_cast<uint32_t>(b.mesh().v.size());
    const float da = 0.01f;
    for (size_t i = 0; i < rings.size(); ++i) {
        const float y = rings[i];
        for (int s = 0; s <= sides; ++s) {
            const float a = 2.0f * kPi * float(s) / float(sides);
            const float r = R(y, a);
            const float drda = (R(y, a + da) - R(y, a - da)) / (2.0f * da);
            const float dy = 0.05f;
            const float drdy = (R(y + dy, a) - R(y - dy, a)) / (2.0f * dy);
            const V3 radial(std::cos(a), 0.0f, std::sin(a));
            const V3 tang(-std::sin(a), 0.0f, std::cos(a));
            const V3 nrm = norm(radial - tang * (drda / std::max(r, 1e-3f)) + V3(0, -drdy, 0));
            TreeVertex v = {};
            const V3 ax = axis_at(y);
            v.pos[0] = ax.x + radial.x * r; v.pos[1] = y; v.pos[2] = ax.z + radial.z * r;
            v.nrm[0] = nrm.x; v.nrm[1] = nrm.y; v.nrm[2] = nrm.z;
            v.uv[0] = float(s) / float(sides) * 2.0f * kPi * std::max(rmain(0.0f), 0.05f);
            v.uv[1] = y + 0.6f;
            v.sway = 0.0f;
            // The trunk's own occlusion where they meet (0.85), a little
            // darker only in the crotches at the ground: at 0.55 the flare
            // read as a dark skirt wherever it showed.
            v.ao = 0.72f + 0.13f * clamp01(y / (0.5f * hf));
            v.kind = float(kKindBark);
            v.seed = 0.0f;
            b.mesh().v.push_back(v);
        }
    }
    for (size_t i = 0; i + 1 < rings.size(); ++i) {
        for (int s = 0; s < sides; ++s) {
            const uint32_t a0 = base + uint32_t(i * (sides + 1) + s), a1 = a0 + 1;
            const uint32_t b0 = a0 + uint32_t(sides + 1), b1 = b0 + 1;
            // Wound as tube() winds its own rings. This ring runs the other
            // way round (counter-clockwise from above, tube()'s clockwise),
            // and the bark shader flips the normal of a back face (cards are
            // double-sided): wound like tube(), every face of the flare was a
            // back face and shaded black.
            const uint32_t t[6] = {a0, a1, b0, a1, b1, b0};
            b.mesh().idx.insert(b.mesh().idx.end(), t, t + 6);
        }
    }
    // Roots: out along the strongest lobes, dipping into the ground.
    for (int k = 0; k < lobes; ++k) {
        if (amp[k] < 0.55f) continue;
        const float a = phase + 2.0f * kPi * float(k) / float(lobes) + rng.range(-0.15f, 0.15f);
        const V3 d(std::cos(a), 0.0f, std::sin(a));
        const float r0 = R0 * (0.3f + 0.25f * amp[k]) * L.roots;
        const float reach = R0 * (2.5f + 2.5f * rng.uni()) * (0.6f + 0.4f * L.roots);
        std::vector<Node> rn;
        const float start = rmain(0.0f) * (1.0f + 0.5f * Fmax * amp[k]);
        const int nn = 4;
        for (int j = 0; j < nn; ++j) {
            const float t = float(j) / float(nn - 1);
            Node q;
            q.p = d * (start * 0.85f + reach * t) + V3(rng.range(-0.08f, 0.08f) * reach * t, 0.12f - 0.45f * t * t, 0.0f);
            q.r = r0 * (1.0f - 0.75f * t);
            q.sway = 0.0f;
            q.ao = 0.72f;
            rn.push_back(q);
        }
        b.tube(rn, trunk_sides >= 8 ? 6 : 4);
    }
}

void build_trunk(Builder& b, TreeMesh& m, const TreeLook& L, float H, float top_frac,
                 float top_r_frac, int sides, Rng& rng, std::vector<V3>& axis,
                 std::vector<float>& radius) {
    const float R0 = L.trunk * H;
    const float top = H * top_frac;
    std::vector<Node> n;
    auto rad_at = [&](float y) {
        float r = R0 * (1.0f - (1.0f - top_r_frac) * clamp01(y / top));
        const float flare = clamp01(1.0f - y / (0.08f * H + 0.3f));
        return r * (1.0f + 0.35f * flare * flare);
    };
    const float ys[2] = {-0.6f, kCutHeight};
    for (int i = 0; i < 2; ++i) {
        Node k; k.p = V3(0, ys[i], 0); k.r = rad_at(std::max(0.0f, ys[i])); k.sway = 0.0f; k.ao = 0.7f;
        n.push_back(k);
    }
    const int segs = 10;
    V3 off(0, 0, 0);
    for (int i = 1; i <= segs; ++i) {
        const float y = kCutHeight + (top - kCutHeight) * float(i) / float(segs);
        off = off + V3(rng.range(-1, 1), 0, rng.range(-1, 1)) * (0.012f * H / segs * 3.0f);
        Node k;
        k.p = V3(off.x, y, off.z);
        k.r = rad_at(y);
        const float t = y / H;
        k.sway = 0.25f * t * t;
        k.ao = y < L.crown_start * H ? 0.85f : 0.6f;
        n.push_back(k);
    }
    b.tube(n, sides);
    root_flare(b, L, H, R0, top, top_r_frac, sides, n, rng);
    const float rc = rad_at(kCutHeight);
    b.cap(V3(0, kCutHeight, 0), rc * 0.995f, sides, true);
    b.cap(V3(0, kCutHeight, 0), rc * 0.995f, sides, false);
    axis.clear(); radius.clear();
    for (size_t i = 1; i < n.size(); ++i) { axis.push_back(n[i].p); radius.push_back(n[i].r); }
    // Keep it on the mesh: a felled log broken into pieces closes each end
    // with the cut's disc scaled to the radius there.
    m.trunk_y.clear(); m.trunk_r.clear(); m.trunk_x.clear(); m.trunk_z.clear();
    for (size_t i = 1; i < n.size(); ++i) {
        m.trunk_y.push_back(n[i].p.y); m.trunk_r.push_back(n[i].r);
        m.trunk_x.push_back(n[i].p.x); m.trunk_z.push_back(n[i].p.z);
    }
}

// Point and radius on the trunk at height y, from the sampled axis.
void trunk_at(const std::vector<V3>& axis, const std::vector<float>& radius, float y,
              V3& p, float& r) {
    for (size_t i = 1; i < axis.size(); ++i) {
        if (axis[i].y >= y || i + 1 == axis.size()) {
            const float t = clamp01((y - axis[i - 1].y) / std::max(1e-4f, axis[i].y - axis[i - 1].y));
            p = axis[i - 1] + (axis[i] - axis[i - 1]) * t;
            r = radius[i - 1] + (radius[i] - radius[i - 1]) * t;
            return;
        }
    }
    p = axis.back(); r = radius.back();
}

// A curved branch from s along d, length Ln, base radius rb.
std::vector<Node> branch_nodes(const V3& s, V3 d, float Ln, float rb, float tip_r,
                               float up_bend, int segs, float sway0, float sway1,
                               const Crown* cr, Rng& rng) {
    std::vector<Node> n;
    V3 p = s;
    for (int i = 0; i <= segs; ++i) {
        const float t = float(i) / float(segs);
        Node k;
        k.p = p;
        k.r = rb + (tip_r - rb) * t;
        k.sway = sway0 + (sway1 - sway0) * t;
        k.ao = cr ? 0.35f + 0.6f * clamp01(cr->depth(p)) : 0.8f;
        n.push_back(k);
        d = norm(d + V3(0, up_bend, 0) * (1.0f / segs) +
                 V3(rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)) * 0.12f);
        p = p + d * (Ln / segs);
    }
    return n;
}

void leaf_cluster(Builder& b, const V3& at, float size, int cards, uint32_t kind,
                  const Crown& cr, float sway, Rng& rng) {
    const float depth = clamp01(cr.depth(at));
    const float ao = 0.3f + 0.7f * depth * depth;
    const V3 out = cr.outward(at);
    const V3 ln = norm(out * 0.75f + V3(0, 0.45f, 0));
    for (int c = 0; c < cards; ++c) {
        const V3 jitter(rng.range(-1, 1), rng.range(-0.6f, 0.6f), rng.range(-1, 1));
        const V3 ctr = at + jitter * (size * 0.35f);
        // A random plane, biased towards facing out and up.
        V3 nrm = norm(out * 0.6f + V3(rng.range(-1, 1), rng.range(-0.2f, 1.0f), rng.range(-1, 1)));
        V3 hu = perp(nrm);
        hu = rotate(hu, nrm, rng.range(0.0f, 2.0f * kPi));
        const V3 hv = cross(nrm, hu);
        const float s = size * rng.range(0.8f, 1.2f) * 0.5f;
        b.card(ctr, hu * s, hv * s, ln, kind, rng.uni(), sway, ao);
    }
}

// Leaves on short twigs off a branch, so the foliage fills the crown's volume
// instead of lining the branches: a cluster at the end of each twig.
void twig_clusters(Builder& b, const std::vector<Node>& br, float leaf, int twigs, int cards,
                   uint32_t kind, const Crown& cr, Rng& rng) {
    for (int t = 0; t < twigs; ++t) {
        const float f = 0.35f + 0.65f * (float(t) + rng.uni()) / float(twigs);
        const size_t ni = (std::min)(br.size() - 2, size_t(f * (br.size() - 1)));
        const V3 a = br[ni].p, d = norm(br[ni + 1].p - br[ni].p);
        V3 side = rotate(perp(d), d, rng.range(0.0f, 2.0f * kPi));
        side = norm(side + V3(0, 0.35f, 0) + cr.outward(a) * 0.4f);
        const V3 tip = a + side * (leaf * rng.range(0.4f, 0.9f));
        leaf_cluster(b, tip, leaf, cards, kind, cr, 0.9f, rng);
    }
}

void build_broadleaf(Builder& b, TreeMesh& m, const TreeLook& L, float H, int lod, bool weeping, Rng& rng) {
    std::vector<V3> axis; std::vector<float> rad;
    build_trunk(b, m, L, H, 0.88f, 0.3f, lod == 0 ? 10 : 6, rng, axis, rad);
    const float cb = L.crown_start * H;
    Crown cr; cr.cy = 0.5f * (cb + H); cr.rx = L.crown_radius * H; cr.ry = 0.5f * (H - cb);
    const int limbs = L.limbs + int(rng.uni() * 3.0f);
    const float leaf = L.leaf_size * (lod == 0 ? 1.0f : 1.9f);
    for (int k = 0; k < limbs; ++k) {
        const float y = cb + (0.88f * H - cb) * (0.04f + 0.86f * (float(k) + rng.uni() * 0.6f) / float(limbs));
        V3 sp; float sr;
        trunk_at(axis, rad, y, sp, sr);
        const float az = float(k) * 2.39996f + rng.range(-0.3f, 0.3f);
        const float rise = weeping ? rng.range(0.15f, 0.45f) : rng.range(0.45f, 1.0f) + 0.4f * (y - cb) / (H - cb);
        const V3 d = norm(V3(std::cos(az), rise, std::sin(az)));
        float Ln = cr.fit(sp, d, cr.rx * 1.6f) * rng.range(0.8f, 1.0f);
        // Limbs a third of the trunk where they leave it, twigs a third of
        // the limb: at half and more, every branch was a club, and a 3-sided
        // club seen from below is a dark pyramid -- the "shards".
        const float rb = (std::min)(sr * 0.38f, 0.02f * Ln + 0.03f);
        std::vector<Node> limb = branch_nodes(sp, d, Ln, rb, rb * 0.15f, 0.25f, 5, 0.25f, 0.6f, &cr, rng);
        if (lod == 0 || rb > 0.07f) b.tube(limb, lod == 0 ? 7 : 5);
        // Second-order branches along the limb. A willow keeps the near
        // level's count at both levels: its strands hang from them, and with
        // fewer the far level was a different, sparser tree.
        const int subs = (lod == 0 || weeping ? 7 : 4) + int(rng.uni() * 2.0f);
        for (int j = 0; j < subs; ++j) {
            const float t = 0.3f + 0.65f * (float(j) + rng.uni() * 0.5f) / float(subs);
            const size_t ni = std::min(limb.size() - 2, size_t(t * (limb.size() - 1)));
            const V3 at = limb[ni].p;
            const V3 ld = norm(limb[ni + 1].p - limb[ni].p);
            V3 sd = rotate(ld, perp(ld), rng.range(0.5f, 1.1f));
            sd = rotate(sd, ld, rng.range(0.0f, 2.0f * kPi));
            if (!weeping) sd = norm(sd + V3(0, 0.3f, 0));
            const float sl = cr.fit(at, sd, Ln * rng.range(0.35f, 0.6f));
            std::vector<Node> sub = branch_nodes(at, sd, sl, (std::min)(limb[ni].r * 0.4f, 0.012f * sl + 0.012f),
                                                 0.006f, 0.2f, 3, 0.5f, 0.85f, &cr, rng);
            if (lod == 0) b.tube(sub, 5);
            if (weeping) {
                // Strands hanging from the branch end: the willow's curtain.
                // The SAME three strands at both levels, from a generator of
                // their own (one draw from the tree's), so the far level's
                // fewer, longer cards hang where the near level's do and do
                // not shift every branch after them. Before, the far level had
                // one strand a branch, 1.7x as wide, from a different stream
                // of draws: a different tree, which the solid far leaves made
                // plain as it switched.
                Rng srng(rng.next());
                const V3 tip = sub.back().p;
                const float leaf0 = L.leaf_size;   // the near level's leaf, for the strand's width
                for (int s = 0; s < 3; ++s) {
                    const V3 top = tip + V3(srng.range(-1, 1), 0, srng.range(-1, 1)) * leaf0 * 0.6f;
                    const float drop = std::min(top.y - 0.8f, H * srng.range(0.25f, 0.5f));
                    const int cards = std::max(2, int(drop / (leaf * 1.2f)));
                    const V3 out = norm(V3(top.x, 0, top.z) + V3(0.001f, 0, 0));
                    const float turn = srng.range(-0.6f, 0.6f);
                    for (int c = 0; c < cards; ++c) {
                        const V3 ctr = top - V3(0, (float(c) + 0.5f) * drop / cards, 0) + out * (0.15f * float(c) * leaf / leaf0);
                        const V3 hv(0, drop / cards * 0.6f, 0);
                        const V3 hu = rotate(out, V3(0, 1, 0), turn) * (leaf0 * 0.35f);
                        const float depth = clamp01(cr.depth(ctr));
                        b.card(ctr, hu, hv, norm(out + V3(0, 0.3f, 0)), kKindLeaf, srng.uni(),
                               0.7f + 0.3f * float(c) / cards, 0.35f + 0.6f * depth);
                    }
                }
            } else {
                twig_clusters(b, sub, leaf, lod == 0 ? 4 : 2, lod == 0 ? 4 : 3, kKindLeaf, cr, rng);
                leaf_cluster(b, sub.back().p, leaf, lod == 0 ? 4 : 3, kKindLeaf, cr, 0.85f, rng);
            }
        }
        if (!weeping) leaf_cluster(b, limb.back().p, leaf, lod == 0 ? 5 : 3, kKindLeaf, cr, 0.8f, rng);
    }
    // Close the top of the crown over the trunk's end.
    if (!weeping) leaf_cluster(b, V3(axis.back().x, 0.9f * H, axis.back().z), leaf, lod == 0 ? 6 : 3, kKindLeaf, cr, 0.7f, rng);
}

void build_conifer(Builder& b, TreeMesh& m, const TreeLook& L, float H, int lod, Rng& rng) {
    std::vector<V3> axis; std::vector<float> rad;
    build_trunk(b, m, L, H, 0.98f, 0.06f, lod == 0 ? 8 : 5, rng, axis, rad);
    const float cb = L.crown_start * H;
    const float rcone = L.crown_radius * H;
    const float leaf = L.leaf_size * (lod == 0 ? 1.0f : 1.7f);
    const float step = lod == 0 ? std::max(0.45f, H / 38.0f) : std::max(0.9f, H / 18.0f);
    const float droop = rng.range(0.10f, 0.35f);
    int w = 0;
    for (float y = cb; y < 0.97f * H; y += step * rng.range(0.85f, 1.15f), ++w) {
        V3 sp; float sr;
        trunk_at(axis, rad, y, sp, sr);
        const float rel = (H - y) / std::max(1e-3f, H - cb);
        const int nb = (lod == 0 ? 5 : 4) + int(rng.uni() * 3.0f);
        const float az0 = rng.range(0.0f, 2.0f * kPi);
        for (int k = 0; k < nb; ++k) {
            const float az = az0 + 2.0f * kPi * (float(k) + rng.range(-0.2f, 0.2f)) / float(nb);
            const float Ln = (rcone * std::pow(rel, 0.95f) + 0.2f) * rng.range(0.8f, 1.1f);
            const V3 d = norm(V3(std::cos(az), -droop * rng.range(0.6f, 1.3f), std::sin(az)));
            std::vector<Node> br = branch_nodes(sp, d, Ln, std::max(0.015f, sr * 0.35f), 0.008f, 0.12f, 3, 0.2f, 0.75f, nullptr, rng);
            if (lod == 0 && Ln > 0.6f) b.tube(br, 3);
            // Needle sprays along the branch: flat-ish cards along it, with a
            // crossed second card at LOD 0 so it has volume from the side.
            const int cards = std::max(1, int(Ln / (leaf * 0.75f)));
            for (int c = 0; c < cards; ++c) {
                const float t = (float(c) + 0.6f) / float(cards);
                const size_t ni = std::min(br.size() - 2, size_t(t * (br.size() - 1)));
                const V3 bd = norm(br[ni + 1].p - br[ni].p);
                const V3 ctr = br[ni].p + (br[ni + 1].p - br[ni].p) * (t * (br.size() - 1) - float(ni));
                V3 side = norm(cross(bd, V3(0, 1, 0)));
                side = rotate(side, bd, rng.range(-0.5f, 0.5f));
                const float hw = leaf * 0.5f * (0.7f + 0.5f * (1.0f - t)), hl = leaf * 0.65f;
                const float dist = std::sqrt(ctr.x * ctr.x + ctr.z * ctr.z);
                const float local_r = rcone * rel + 0.2f;
                const float depth = clamp01(dist / std::max(0.2f, local_r));
                const float ao = 0.35f + 0.65f * depth;
                const V3 ln = norm(norm(V3(ctr.x, 0, ctr.z) + V3(1e-3f, 0, 0)) * 0.7f + V3(0, 0.55f, 0));
                b.card(ctr, side * hw, bd * hl, ln, kKindNeedle, rng.uni(), 0.5f + 0.4f * t, ao);
                if (lod == 0) {
                    const V3 up = norm(cross(side, bd));
                    b.card(ctr, up * (hw * 0.6f), bd * hl, ln, kKindNeedle, rng.uni(), 0.5f + 0.4f * t, ao);
                }
            }
        }
    }
    // The leader.
    const V3 top(axis.back().x, H * 0.985f, axis.back().z);
    for (int c = 0; c < 3; ++c) {
        const V3 hu = rotate(V3(1, 0, 0), V3(0, 1, 0), c * kPi / 3.0f) * (leaf * 0.25f);
        b.card(top, hu, V3(0, leaf * 0.6f, 0), V3(0, 1, 0), kKindNeedle, rng.uni(), 0.9f, 1.0f);
    }
}

void build_pine(Builder& b, TreeMesh& m, const TreeLook& L, float H, int lod, Rng& rng) {
    std::vector<V3> axis; std::vector<float> rad;
    build_trunk(b, m, L, H, 0.93f, 0.25f, lod == 0 ? 9 : 6, rng, axis, rad);
    const float cb = L.crown_start * H;
    Crown cr; cr.cy = 0.5f * (cb + H); cr.rx = L.crown_radius * H; cr.ry = 0.5f * (H - cb) * 1.1f;
    const float leaf = L.leaf_size * (lod == 0 ? 1.0f : 1.7f);
    const int limbs = L.limbs + int(rng.uni() * 3.0f);
    for (int k = 0; k < limbs; ++k) {
        const float y = cb + (0.93f * H - cb) * (float(k) + rng.uni() * 0.7f) / float(limbs);
        V3 sp; float sr;
        trunk_at(axis, rad, y, sp, sr);
        const float az = float(k) * 2.39996f + rng.range(-0.4f, 0.4f);
        const V3 d = norm(V3(std::cos(az), rng.range(0.25f, 0.8f), std::sin(az)));
        const float Ln = cr.fit(sp, d, cr.rx * 1.5f) * rng.range(0.75f, 1.0f);
        std::vector<Node> limb = branch_nodes(sp, d, Ln, sr * 0.45f, sr * 0.1f, 0.15f, 4, 0.25f, 0.6f, &cr, rng);
        b.tube(limb, lod == 0 ? 5 : 3);
        // Tufts: a star of needle cards at the end and partway along.
        const int tufts = lod == 0 ? 3 : 2;
        for (int t = 0; t < tufts; ++t) {
            const V3 at = limb[limb.size() - 1 - std::min<size_t>(limb.size() - 1, size_t(t))].p;
            leaf_cluster(b, at, leaf, lod == 0 ? 4 : 3, kKindNeedle, cr, 0.75f, rng);
        }
    }
    leaf_cluster(b, V3(axis.back().x, 0.95f * H, axis.back().z), leaf, 4, kKindNeedle, cr, 0.7f, rng);
}

void build_shrub(Builder& b, TreeMesh& m, const TreeLook& L, float H, int lod, Rng& rng) {
    // A short trunk to carry the cut, stems splaying from its top.
    std::vector<V3> axis; std::vector<float> rad;
    TreeLook l2 = L;
    build_trunk(b, m, l2, H, std::min(0.95f, (kCutHeight + 0.35f) / H), 0.8f, 6, rng, axis, rad);
    const V3 base = axis.back();
    Crown cr; cr.cy = H * 0.55f; cr.rx = L.crown_radius * H; cr.ry = H * 0.5f;
    const float leaf = L.leaf_size * (lod == 0 ? 1.0f : 1.8f);
    const int stems = L.limbs + int(rng.uni() * 3.0f);
    for (int k = 0; k < stems; ++k) {
        const float az = float(k) * 2.39996f + rng.range(-0.3f, 0.3f);
        const V3 d = norm(V3(std::cos(az) * rng.range(0.4f, 0.9f), 1.0f, std::sin(az) * rng.range(0.4f, 0.9f)));
        const float Ln = cr.fit(base, d, H) * rng.range(0.8f, 1.0f);
        std::vector<Node> st = branch_nodes(base, d, Ln, L.trunk * H * 0.5f, 0.01f, 0.1f, 4, 0.3f, 0.9f, &cr, rng);
        b.tube(st, lod == 0 ? 4 : 3);
        for (size_t i = st.size() / 2; i < st.size(); ++i) {
            leaf_cluster(b, st[i].p, leaf, lod == 0 ? 3 : 2, kKindLeaf, cr, 0.9f, rng);
        }
    }
}

void build_snag(Builder& b, TreeMesh& m, const TreeLook& L, float H, int lod, Rng& rng) {
    std::vector<V3> axis; std::vector<float> rad;
    // Broken off: the top keeps 45% of the base radius, and stops short.
    build_trunk(b, m, L, H, rng.range(0.75f, 1.0f), 0.45f, lod == 0 ? 8 : 5, rng, axis, rad);
    const int stubs = L.limbs + int(rng.uni() * 2.0f);
    for (int k = 0; k < stubs; ++k) {
        const float y = H * rng.range(L.crown_start, 0.8f);
        if (y > axis.back().y - 0.5f) continue;
        V3 sp; float sr;
        trunk_at(axis, rad, y, sp, sr);
        const float az = rng.range(0.0f, 2.0f * kPi);
        const V3 d = norm(V3(std::cos(az), rng.range(-0.1f, 0.7f), std::sin(az)));
        std::vector<Node> st = branch_nodes(sp, d, rng.range(0.6f, 2.8f), sr * 0.4f, sr * 0.15f, 0.0f, 3, 0.1f, 0.2f, nullptr, rng);
        b.tube(st, lod == 0 ? 5 : 3);
    }
}

}  // namespace

bool load_tree_looks(const worldgen::ArchetypeTable& table, const char* json_path,
                     std::vector<TreeLook>& out, std::string& error) {
    out.clear();
    try {
        const jsonlite::Value root = jsonlite::parse_file(json_path);
        if (!root.has("trees")) return true;
        const jsonlite::Array& sp = root.at("trees").array("species");
        for (size_t i = 0; i < sp.size() && i < table.trees.species.size(); ++i) {
            TreeLook l;
            const std::string& f = sp[i].string("form");
            if (f == "broadleaf") l.form = TreeForm::Broadleaf;
            else if (f == "weeping") l.form = TreeForm::Weeping;
            else if (f == "conifer") l.form = TreeForm::Conifer;
            else if (f == "pine") l.form = TreeForm::Pine;
            else if (f == "shrub") l.form = TreeForm::Shrub;
            else if (f == "snag") l.form = TreeForm::Snag;
            else { error = "tree form '" + f + "' is not one the mesh generator knows"; return false; }
            if (sp[i].has("shape")) {
                const jsonlite::Value& s = sp[i].at("shape");
                const jsonlite::Array& bk = s.array("bark");
                const jsonlite::Array& lf = s.array("leaf");
                for (int k = 0; k < 3; ++k) {
                    l.bark[k] = float(bk[k].num);
                    l.leaf[k] = float(lf[k].num);
                }
                l.crown_start = float(s.number("crown_start"));
                l.crown_radius = float(s.number("crown_radius"));
                l.trunk = float(s.number("trunk"));
                l.leaf_size = float(s.number("leaf_size"));
                l.limbs = int(s.number("limbs"));
                l.leaf_density = float(s.number("leaf_density"));
                if (s.has("bark_scan")) l.bark_scan = s.string("bark_scan");
                if (s.has("bend")) l.bend = float(s.number("bend"));
                if (s.has("roots")) l.roots = float(s.number("roots"));
            }
            const worldgen::TreeSpecies& ts = table.trees.species[i];
            l.nominal_height = 0.5f * (ts.height_min + ts.height_max);
            out.push_back(l);
        }
    } catch (const std::exception& e) {
        error = std::string("tree looks: ") + e.what();
        return false;
    }
    return true;
}

TreeModel build_tree_model(const TreeLook& look, uint32_t species, uint32_t variant) {
    TreeModel tm;
    const float H = look.nominal_height;
    for (int lod = 0; lod < 2; ++lod) {
        // The same seed at both levels, so LOD 1 is the same tree simplified
        // rather than a different tree: the skeleton draws identical numbers
        // until the levels diverge on what they emit.
        Rng rng(0x51EDu ^ (species * 0x9E3779B1u) ^ (variant * 0x85EBCA77u));
        TreeMesh& m = tm.lod[lod];
        Builder b(m);
        switch (look.form) {
        case TreeForm::Broadleaf: build_broadleaf(b, m, look, H, lod, false, rng); break;
        case TreeForm::Weeping:   build_broadleaf(b, m, look, H, lod, true, rng); break;
        case TreeForm::Conifer:   build_conifer(b, m, look, H, lod, rng); break;
        case TreeForm::Pine:      build_pine(b, m, look, H, lod, rng); break;
        case TreeForm::Shrub:     build_shrub(b, m, look, H, lod, rng); break;
        case TreeForm::Snag:      build_snag(b, m, look, H, lod, rng); break;
        }
    }
    float rmax = 0.0f, top = 0.0f;
    for (size_t i = 0; i < tm.lod[0].v.size(); ++i) {
        const TreeVertex& v = tm.lod[0].v[i];
        rmax = std::max(rmax, std::sqrt(v.pos[0] * v.pos[0] + v.pos[2] * v.pos[2]));
        top = std::max(top, v.pos[1]);
    }
    tm.radius = std::max(rmax, 0.5f);
    tm.top = std::max(top, 1.0f);
    tm.trunk_radius = look.trunk * H;
    tm.crown_base = look.crown_start * H;
    tm.crown_centre = 0.5f * (tm.crown_base + tm.top);
    tm.crown_r = std::max(0.3f, std::min(look.crown_radius * H, 0.5f * (tm.top - tm.crown_base)) * 0.8f);
    if (look.form == TreeForm::Snag) tm.crown_r = 0.0f;
    return tm;
}

void trunk_axis_at(const TreeModel& m, float y, float& x, float& z, float& r) {
    const TreeMesh& t = m.lod[0];
    x = z = r = 0.0f;
    if (t.trunk_y.empty()) return;
    size_t i = 1;
    for (; i < t.trunk_y.size(); ++i) if (y <= t.trunk_y[i]) break;
    if (i >= t.trunk_y.size()) i = t.trunk_y.size() - 1;
    if (i == 0) { x = t.trunk_x[0]; z = t.trunk_z[0]; r = t.trunk_r[0]; return; }
    const float d = std::max(1e-4f, t.trunk_y[i] - t.trunk_y[i - 1]);
    const float u = clamp01((y - t.trunk_y[i - 1]) / d);
    x = t.trunk_x[i - 1] + (t.trunk_x[i] - t.trunk_x[i - 1]) * u;
    z = t.trunk_z[i - 1] + (t.trunk_z[i] - t.trunk_z[i - 1]) * u;
    r = t.trunk_r[i - 1] + (t.trunk_r[i] - t.trunk_r[i - 1]) * u;
}

CrownVolume crown_volume(const TreeModel& tm, const TreeLook& look) {
    CrownVolume c;
    if (look.leaf_density <= 0.0f) return c;
    c.rv = 0.5f * (tm.top - tm.crown_base);
    c.cy = tm.crown_base + c.rv;
    c.rh = 0.9f * tm.radius;
    // Beer-Lambert through randomly oriented leaves: extinction G x leaf
    // area density, G = 0.5 for a spherical leaf-angle distribution (Ross).
    c.sigma = 0.5f * look.leaf_density;
    if (c.rv < 0.25f || c.rh < 0.25f) c = CrownVolume();
    return c;
}

}  // namespace engine
