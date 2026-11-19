// Opaque cutouts of the leaf cards (leaf_cutout.h).
#include "engine/trees/leaf_cutout.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace engine {

namespace {

// The RT alpha test's settings (gi_trace.hlsli, rt_leaf_solid).
const float kBlur = 0.1f;
const float kThreshold = 0.5f;
const float kTwoPi = 6.2831853f;
const float kLeafLen = 0.47f;

float sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
float frac(float x) { return x - std::floor(x); }
float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// leaf.hlsli's hash2 on floor(c).
float hash2(float cx, float cy) {
    const uint32_t qx = uint32_t(int32_t(std::floor(cx)) + 0x4000);
    const uint32_t qy = uint32_t(int32_t(std::floor(cy)) + 0x4000);
    uint32_t h = (qx * 0x8da6b343u) ^ (qy * 0xd8163841u);
    h = (h ^ (h >> 13)) * 0xcb1ab31fu;
    return float((h ^ (h >> 16)) & 0xFFFFu) / 65535.0f;
}

// One cell's leaf in leaf_layer: its hashes, decided once.
struct Leaf {
    bool gap;
    float c, s;          // cos and sin of its turn
    float offx, offy;    // its jitter, in cell units
};

Leaf make_leaf(float cx, float cy, float seed) {
    const float h0 = hash2(cx + seed * 97.0f, cy + seed * 97.0f);
    const float h1 = hash2(cy + seed * 53.0f + 17.0f, cx + seed * 53.0f + 17.0f);
    const float h2 = hash2(cx + seed * 31.0f + 41.0f, cy + seed * 31.0f + 41.0f);
    Leaf l;
    l.gap = h2 < 0.06f;
    const float ang = h0 * kTwoPi;
    l.c = std::cos(ang);
    l.s = std::sin(ang);
    l.offx = (h1 - 0.5f) * 0.18f;
    l.offy = (h2 - 0.5f) * 0.18f;
    return l;
}

// The leaf's coverage at f = g - cell - 0.5.
float leaf_value(const Leaf& l, float fx, float fy, float narrow) {
    const float dx = fx - l.offx, dy = fy - l.offy;
    const float rx = dx * l.c + dy * l.s, ry = -dx * l.s + dy * l.c;
    const float wid = (narrow > 0.5f ? 0.10f : 0.25f) * (1.0f - 0.55f * sat(rx / kLeafLen + 0.5f));
    const float e = (rx * rx) / (kLeafLen * kLeafLen) + (ry * ry) / (wid * wid);
    return sat((1.0f - e) / 0.2f);
}

float leaf_layer(float u, float v, float cells, float seed, float narrow) {
    const float gx = u * cells, gy = v * cells;
    const float cx = std::floor(gx), cy = std::floor(gy);
    const Leaf l = make_leaf(cx, cy, seed);
    if (l.gap) return 0.0f;
    return leaf_value(l, gx - cx - 0.5f, gy - cy - 0.5f, narrow);
}

float rim_at(float u, float v) {
    const float du = u - 0.5f, dv = v - 0.5f;
    return 1.0f - sat((std::sqrt(du * du + dv * dv) - 0.40f) / 0.1f);
}

float coverage_sharp(float u, float v, float kind, float seed, float blur, float narrow) {
    if (kind < 1.5f) {
        const float a0 = leaf_layer(u, v, 7.0f, seed, narrow);
        const float a1 = leaf_layer(u + 0.5f / 7.0f, v + 0.5f / 7.0f, 7.0f, seed + 0.37f, narrow);
        return std::max(a0, a1);
    }
    const float s = std::fabs(u - 0.5f);
    const float twig = 1.0f - sat(s / std::max(0.02f + blur, 0.03f));
    const float along = frac(v * 9.0f + s * 6.0f + seed * 3.0f);
    const float stroke = 1.0f - sat(std::fabs(along - 0.5f) / std::max(0.18f + blur * 2.0f, 0.2f));
    const float body = sat(1.0f - s * 2.1f) * sat(1.0f - std::fabs(v - 0.5f) * 2.05f);
    return std::max(twig, stroke * (body >= 0.05f ? 1.0f : 0.0f) * sat(body * 4.0f));
}

// A card: the parallelogram tree_mesh.cpp's card() makes, uv (0,1) (1,1)
// (1,0) (0,0) at its four corners. pos(u, v) = p00 + du * u + dv * v.
struct Card {
    float p00[3], du[3], dv[3];
    void at(float u, float v, float* out) const {
        for (int k = 0; k < 3; ++k) out[k] = p00[k] + du[k] * u + dv[k] * v;
    }
};

struct Emitter {
    const Card* card;
    std::vector<float>* out;
    uint32_t tris = 0;
    void tri(const float* a, const float* b, const float* c) {   // uv pairs
        float p[3];
        for (const float* q : {a, b, c}) {
            card->at(q[0], q[1], p);
            out->insert(out->end(), p, p + 3);
        }
        ++tris;
    }
    // A fan over a polygon of n uv points.
    void fan(const float (*pts)[2], int n) {
        for (int i = 1; i + 1 < n; ++i) tri(pts[0], pts[i], pts[i + 1]);
    }
};

// Broad leaves: a polygon per leaf, two layers of 7 x 7 cells.
uint32_t cut_broad(const Card& card, float seed, float narrow, Emitter& em) {
    const float avg = 0.55f;
    const float mix = sat(kBlur * 5.0f - 0.25f);
    // Five rays: a pentagon a leaf, three triangles. Eight traced SLOWER
    // (wood at 2560 x 1440: 5.25 ms against 5.01, with the needles' finer
    // grid) for 2.1x the BLAS; four lost enough leaf to let rays run further
    // (5.15). Inscribed, the pentagons were 24% short of the leaf: the wood's
    // floor read 48.4 against the alpha-tested cards' 46.4 at EV100 13, so
    // they are grown back to the leaf's area (below).
    const int kRays = 5;
    const float w0 = narrow > 0.5f ? 0.10f : 0.25f;
    uint32_t leaves = 0;
    for (int layer = 0; layer < 2; ++layer) {
        const float o = layer ? 0.5f / 7.0f : 0.0f;
        const float sd = seed + (layer ? 0.37f : 0.0f);
        for (int cy = 0; cy < 8; ++cy) {
            for (int cx = 0; cx < 8; ++cx) {
                const Leaf l = make_leaf(float(cx), float(cy), sd);
                if (l.gap) continue;
                // Inside this leaf (and only this leaf: the layers' max is the
                // union of their leaves, and the mix towards the average and
                // the rim are monotonic in it), at a point in grid units.
                auto inside = [&](float gx, float gy) {
                    if (std::floor(gx) != float(cx) || std::floor(gy) != float(cy)) return false;
                    const float val = leaf_value(l, gx - cx - 0.5f, gy - cy - 0.5f, narrow);
                    const float u = gx / 7.0f - o, v = gy / 7.0f - o;
                    return rim_at(u, v) * lerpf(val, avg, mix) >= kThreshold;
                };
                const float gcx = cx + 0.5f + l.offx, gcy = cy + 0.5f + l.offy;
                if (!inside(gcx, gcy)) continue;
                float pts[8][2];
                for (int k = 0; k < kRays; ++k) {
                    // Directions spread round the leaf's own ellipse, the
                    // first along its axis to its tip.
                    const float t = kTwoPi * float(k) / float(kRays);
                    float rx = kLeafLen * std::cos(t), ry = w0 * std::sin(t);
                    const float rl = std::sqrt(rx * rx + ry * ry);
                    rx /= rl; ry /= rl;
                    const float dx = l.c * rx - l.s * ry, dy = l.s * rx + l.c * ry;
                    // March to the first point outside, then bisect.
                    const int kSteps = 24;
                    const float kMax = 0.8f;
                    float lo = 0.0f, hi = kMax;
                    for (int i = 1; i <= kSteps; ++i) {
                        const float s = kMax * float(i) / float(kSteps);
                        if (!inside(gcx + dx * s, gcy + dy * s)) { hi = s; break; }
                        lo = s;
                    }
                    for (int i = 0; i < 14; ++i) {
                        const float m = 0.5f * (lo + hi);
                        if (inside(gcx + dx * m, gcy + dy * m)) lo = m; else hi = m;
                    }
                    // An n-gon inscribed in an ellipse covers n sin(2 pi/n) / 2 pi
                    // of it, 76% at five: pushed out by the root of the
                    // inverse, the pentagon has the leaf's own area.
                    const float grow = std::sqrt(kTwoPi / (float(kRays) * std::sin(kTwoPi / float(kRays))));
                    pts[k][0] = (gcx + dx * lo * grow) / 7.0f - o;
                    pts[k][1] = (gcy + dy * lo * grow) / 7.0f - o;
                }
                em.fan(pts, kRays);
                ++leaves;
            }
        }
    }
    return leaves;
}

// Needles: marching squares per half card in (s, a) = (|u - 0.5|,
// 9v + 6s + 3 seed), which is affine to (u, v), so a cell's straight edges
// are straight on the card, and every stroke is a band of constant a.
void cut_needles(const Card& card, float seed, Emitter& em) {
    // Coarse on purpose, with the broad leaves' five rays (above): 24 x 8
    // cost more to trace than it saved in fidelity.
    const int kNs = 10;                 // across a half card
    const int kPerUnit = 4;             // per stroke period
    const float a0 = 3.0f * seed, a1 = a0 + 12.0f;
    const int kNa = 12 * kPerUnit;
    std::vector<unsigned char> in((kNs + 1) * (kNa + 1));
    for (int side = 0; side < 2; ++side) {
        const float sg = side ? 1.0f : -1.0f;
        auto to_uv = [&](float s, float a, float* uv) {
            uv[0] = 0.5f + sg * s;
            uv[1] = (a - 6.0f * s - 3.0f * seed) / 9.0f;
        };
        auto inside = [&](float s, float a) {
            float uv[2];
            to_uv(s, a, uv);
            return leaf_coverage_cpu(uv[0], uv[1], 2.0f, seed, kBlur, 0.0f) >= kThreshold;
        };
        auto S = [&](int i) { return 0.5f * float(i) / float(kNs); };
        auto A = [&](int j) { return a0 + (a1 - a0) * float(j) / float(kNa); };
        for (int j = 0; j <= kNa; ++j)
            for (int i = 0; i <= kNs; ++i) in[j * (kNs + 1) + i] = inside(S(i), A(j)) ? 1 : 0;
        auto at = [&](int i, int j) { return in[j * (kNs + 1) + i] != 0; };
        auto quad = [&](float s0, float s1, float b0, float b1) {
            float p[4][2];
            to_uv(s0, b0, p[0]); to_uv(s1, b0, p[1]); to_uv(s1, b1, p[2]); to_uv(s0, b1, p[3]);
            em.fan(p, 4);
        };
        for (int j = 0; j < kNa; ++j) {
            int run = -1;   // first cell of a run of wholly inside cells
            for (int i = 0; i <= kNs; ++i) {
                const bool full = i < kNs && at(i, j) && at(i + 1, j) && at(i + 1, j + 1) && at(i, j + 1);
                if (full) { if (run < 0) run = i; continue; }
                if (run >= 0) { quad(S(run), S(i), A(j), A(j + 1)); run = -1; }
                if (i == kNs) break;
                // A boundary cell: its inside corners and the crossings
                // between them, round the cell in order.
                const int ci[4][2] = {{i, j}, {i + 1, j}, {i + 1, j + 1}, {i, j + 1}};
                bool any = false;
                for (int k = 0; k < 4; ++k) any = any || at(ci[k][0], ci[k][1]);
                if (!any) continue;
                float pts[8][2];
                int n = 0;
                for (int k = 0; k < 4; ++k) {
                    const int* p = ci[k];
                    const int* q = ci[(k + 1) & 3];
                    const bool ip = at(p[0], p[1]), iq = at(q[0], q[1]);
                    if (ip) to_uv(S(p[0]), A(p[1]), pts[n++]);
                    if (ip != iq) {
                        // Bisect along the edge from the inside end.
                        float s_in = S(ip ? p[0] : q[0]), a_in = A(ip ? p[1] : q[1]);
                        float s_out = S(ip ? q[0] : p[0]), a_out = A(ip ? q[1] : p[1]);
                        for (int it = 0; it < 12; ++it) {
                            const float sm = 0.5f * (s_in + s_out), am = 0.5f * (a_in + a_out);
                            if (inside(sm, am)) { s_in = sm; a_in = am; } else { s_out = sm; a_out = am; }
                        }
                        to_uv(s_in, a_in, pts[n++]);
                    }
                }
                if (n >= 3) em.fan(pts, n);
            }
        }
    }
}

}  // namespace

float leaf_coverage_cpu(float u, float v, float kind, float seed, float blur, float narrow) {
    const float avg = kind < 1.5f ? 0.55f : 0.5f;
    return rim_at(u, v) * lerpf(coverage_sharp(u, v, kind, seed, blur, narrow), avg, sat(blur * 5.0f - 0.25f));
}

void leaf_cutouts(const TreeMesh& mesh, bool narrow, std::vector<float>& tri_pos, LeafCutoutStats& stats) {
    // Cards are four vertices and two triangles (base, +1, +2), (base, +2,
    // +3): find each by its first triangle.
    for (size_t t = 0; t + 2 < mesh.idx.size(); t += 3) {
        const uint32_t b = mesh.idx[t];
        if (b + 3 >= mesh.v.size()) continue;
        const TreeVertex& v0 = mesh.v[b];
        if (!(v0.kind > 0.5f && v0.kind < 2.5f)) continue;
        if (mesh.idx[t + 1] != b + 1 || mesh.idx[t + 2] != b + 2) continue;
        // Corner 3 is uv (0, 0), corner 1 is (1, 1), corner 0 is (0, 1).
        Card card;
        for (int k = 0; k < 3; ++k) {
            card.p00[k] = mesh.v[b + 3].pos[k];
            card.du[k] = mesh.v[b + 1].pos[k] - mesh.v[b].pos[k];
            card.dv[k] = mesh.v[b].pos[k] - mesh.v[b + 3].pos[k];
        }
        Emitter em;
        em.card = &card;
        em.out = &tri_pos;
        if (v0.kind < 1.5f) stats.leaves += cut_broad(card, v0.seed, narrow ? 1.0f : 0.0f, em);
        else { cut_needles(card, v0.seed, em); stats.needle_tris += em.tris; }
        stats.triangles += em.tris;
        ++stats.cards;
    }
}

}  // namespace engine
