#include "engine/trees/felling.h"
#include "engine/physics/physics_world.h"
#include "engine/render/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace engine {
namespace {

void qrot(const float q[4], const float v[3], float out[3]) {
    // v + 2 w (q x v) + 2 q x (q x v)
    const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
    const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
    const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
    out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
    out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// A point of the model, at (x, y, z) in model metres, in the world.
void model_point(const float pos[3], const float rot[4], float scale, float x, float y, float z,
                 float out[3]) {
    const float l[3] = {x * scale, y * scale, z * scale};
    float r[3];
    qrot(rot, l, r);
    for (int k = 0; k < 3; ++k) out[k] = pos[k] + r[k];
}

// ... and the inverse: a world point in the model's own metres. The
// rotation is a unit quaternion, so its inverse is its conjugate.
void world_point(const float pos[3], const float rot[4], float scale, const float p[3],
                 float out[3]) {
    const float d[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    const float inv[4] = {-rot[0], -rot[1], -rot[2], rot[3]};
    float l[3];
    qrot(inv, d, l);
    const float k = 1.0f / ((scale > 1e-4f) ? scale : 1e-4f);
    for (int i = 0; i < 3; ++i) out[i] = l[i] * k;
}

// Squared distance from a point to a segment, and where along it.
float point_seg(const float p[3], const float a[3], const float b[3], float& t) {
    const float d[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const float w[3] = {p[0] - a[0], p[1] - a[1], p[2] - a[2]};
    t = dd > 1e-8f ? (w[0] * d[0] + w[1] * d[1] + w[2] * d[2]) / dd : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float q[3] = {w[0] - d[0] * t, w[1] - d[1] * t, w[2] - d[2] * t};
    return q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
}

// Closest approach between two segments, and where along each. Ericson,
// "Real-Time Collision Detection", 5.1.9, with the degenerate cases folded
// into the clamps.
float seg_seg(const float p1[3], const float q1[3], const float p2[3], const float q2[3],
              float& s, float& t) {
    float d1[3], d2[3], r[3];
    for (int k = 0; k < 3; ++k) { d1[k] = q1[k] - p1[k]; d2[k] = q2[k] - p2[k]; r[k] = p1[k] - p2[k]; }
    const float a = d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2];
    const float e = d2[0] * d2[0] + d2[1] * d2[1] + d2[2] * d2[2];
    const float f = d2[0] * r[0] + d2[1] * r[1] + d2[2] * r[2];
    const float c = d1[0] * r[0] + d1[1] * r[1] + d1[2] * r[2];
    const float b = d1[0] * d2[0] + d1[1] * d2[1] + d1[2] * d2[2];
    s = 0.0f; t = 0.0f;
    if (a <= 1e-8f && e <= 1e-8f) {
        // Both degenerate.
    } else if (a <= 1e-8f) {
        t = f / e;
    } else if (e <= 1e-8f) {
        s = -c / a;
    } else {
        const float denom = a * e - b * b;
        s = denom > 1e-8f ? (b * f - c * e) / denom : 0.0f;
        s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
        t = (b * s + f) / e;
        if (t < 0.0f) { t = 0.0f; s = -c / a; }
        else if (t > 1.0f) { t = 1.0f; s = (b - c) / a; }
    }
    s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    float u[3];
    for (int k = 0; k < 3; ++k) u[k] = (p1[k] + d1[k] * s) - (p2[k] + d2[k] * t);
    return u[0] * u[0] + u[1] * u[1] + u[2] * u[2];
}

// The way a tree goes over: its own, from its id, so the same tree always
// falls the same way and no two neighbours go together. It is NOT the
// direction of the axe -- a tree falls where it leans, and being able to
// aim one by walking round it made every fall read as a kick.
void topple_heading(uint32_t id, float out[3]) {
    uint32_t h = id * 0x9E3779B1u;
    h ^= h >> 15; h *= 0x85EBCA6Bu; h ^= h >> 13; h *= 0xC2B2AE35u; h ^= h >> 16;
    const float a = float(h & 0xFFFFFFu) / float(0x1000000u) * 6.28318531f;
    out[0] = std::cos(a); out[1] = 0.0f; out[2] = std::sin(a);
}

// The way a tree LEANS, which is the way it goes over. Every tree is
// planted with up to three degrees of lean of its own
// (tree_standing_rotation), so this is already a direction of its own --
// and, hinged at the stump, it is the only direction gravity helps. Felled
// across its lean a tree has to be pushed over its own weight first: a
// 24 m one nudged at 0.05 rad/s against two degrees of lean simply hung
// there, three degrees off upright, for as long as anyone watched.
// False if it stands too straight to tell.
bool topple_lean(const float rot[4], float out[3]) {
    const float up_l[3] = {0.0f, 1.0f, 0.0f};
    float u[3];
    qrot(rot, up_l, u);
    const float l = std::sqrt(u[0] * u[0] + u[2] * u[2]);
    if (l < 0.004f) return false;      // under a quarter of a degree
    out[0] = u[0] / l; out[1] = 0.0f; out[2] = u[2] / l;
    return true;
}

const uint32_t kMagic = 0x45455254u;   // "TREE"
const uint32_t kFormat = 3u;           // 1: logs. 2: unused. 3: stumps and pieces.

// The player, in `Piece::struck`. Tree ids are 32 bits and piece keys start
// one above this (`next_piece_` is pre-incremented), so it is the one value
// in the map that can never be either.
const uint64_t kPlayerKey = 0x100000000ull;

}  // namespace

int TreeFelling::hits_needed(float trunk_r) const {
    // Two swings and one more per 6 cm of diameter: a birch sapling in a
    // couple, an old oak in about a dozen.
    int n = int(2.0f + (2.0f * trunk_r) / 0.06f + 0.5f);
    return n < 2 ? 2 : (n > 14 ? 14 : n);
}

// The capsule, the end discs, the mass and the health of a band of trunk.
// Everything downstream -- the draw, the body, what a chop does, what E gets
// -- comes from here, so a piece and the half it breaks into are measured
// the same way.
void TreeFelling::piece_geometry(Piece& p) const {
    const TreeRenderer& tr = renderer_->trees();
    const TreeModel& m = tr.model(p.model / kTreeVariants, p.model % kTreeVariants);
    float cut_x = 0.0f, cut_z = 0.0f, cut_r = 0.0f;
    trunk_axis_at(m, kCutHeight, cut_x, cut_z, cut_r);
    float rlo = 0.0f, rhi = 0.0f;
    trunk_axis_at(m, p.lo, p.ax_lo[0], p.ax_lo[1], rlo);
    trunk_axis_at(m, p.hi, p.ax_hi[0], p.ax_hi[1], rhi);
    if (cut_r <= 1e-4f) {
        // No profile (an impossible model): fall back to the trunk radius.
        cut_r = rlo = rhi = m.trunk_radius;
        p.cap_lo = p.cap_hi = 0.0f;
    } else {
        p.cap_lo = rlo / cut_r;
        p.cap_hi = rhi / cut_r;
    }
    // The capsule is the thinner end, less the bark the draw hides, so a
    // piece does not stand proud of its own surface at the narrow end.
    const float r_mid = 0.5f * (rlo + rhi);
    p.r = (std::max)(0.04f, (std::min)(rlo, rhi) * p.scale * 0.9f);
    const float len = (std::max)(0.05f, (p.hi - p.lo) * p.scale);
    const float vol = 3.14159265f * r_mid * p.scale * r_mid * p.scale * len;
    p.mass = vol * tune_.wood_density;
    p.max_health = (std::max)(tune_.min_health, vol * tune_.health_per_m3);
    p.wood = (std::max)(1, int(vol * tune_.wood_per_m3 + 0.5f));
}

TreeFelling::Piece* TreeFelling::make_piece(const Piece& from, float lo, float hi, int stage,
                                            const float pos[3], const float rot[4],
                                            const float vel[3], const float spin[3], bool asleep) {
    Piece p;
    p.key = ++next_piece_;
    p.tree = from.tree;
    p.model = from.model;
    p.scale = from.scale;
    p.stage = stage;
    p.lo = lo;
    p.hi = hi;
    for (int k = 0; k < 3; ++k) p.pos[k] = pos[k];
    for (int k = 0; k < 4; ++k) p.rot[k] = rot[k];
    piece_geometry(p);
    p.health = p.max_health;
    const float a[3] = {p.ax_lo[0] * p.scale, p.lo * p.scale, p.ax_lo[1] * p.scale};
    const float b[3] = {p.ax_hi[0] * p.scale, p.hi * p.scale, p.ax_hi[1] * p.scale};
    p.body = physics_->add_piece(p.pos, p.rot, p.r, a, b, vel, spin, asleep);
    pieces_[p.key] = p;
    return &pieces_[p.key];
}

void TreeFelling::update(float x, float z, float dt) {
    if (!renderer_ || !physics_) return;
    // Rocks (layer 9) near the player get their colliders here too: every
    // mode that has a player already calls this, and a rock is a prop like
    // a trunk -- static, diffed by id.
    renderer_->rocks().sync_colliders(physics_, renderer_->rock_field().rocks(),
                                      renderer_->rock_field().version(), x, z);
    const TreeRenderer& tr = renderer_->trees();
    if (!tr.ready()) return;
    TreeField& field = renderer_->tree_field();

    // Trunk colliders: rebuilt when the player has moved a few metres or the
    // standing set changed. A diff by id, so a tree keeps its body.
    const float dx = x - last_x_, dz = z - last_z_;
    if (dx * dx + dz * dz > 9.0f || field.version() != field_version_) {
        last_x_ = x; last_z_ = z;
        field_version_ = field.version();
        std::unordered_map<uint32_t, uint32_t> keep;
        const std::vector<worldgen::TreeInstance>& st = field.standing();
        const float r2 = kColliderRadius * kColliderRadius;
        for (size_t i = 0; i < st.size(); ++i) {
            const worldgen::TreeInstance& t = st[i];
            const float ex = t.x - x, ez = t.z - z;
            if (ex * ex + ez * ez > r2) continue;
            std::unordered_map<uint32_t, uint32_t>::iterator it = colliders_.find(t.id);
            if (it != colliders_.end()) {
                // Kept while the tree stays put; a tree dropping into a dug
                // hole gets a new capsule at its new base.
                std::unordered_map<uint32_t, float>::iterator yi = collider_y_.find(t.id);
                if (yi != collider_y_.end() && std::fabs(yi->second - t.y) < 0.01f) {
                    keep[t.id] = it->second;
                    colliders_.erase(it);
                    continue;
                }
                physics_->remove_body(it->second);
                colliders_.erase(it);
            }
            const TreeModel& m = tr.model(t.species, t.variant);
            const float s = t.height / tr.look(t.species).nominal_height;
            const float r = m.trunk_radius * s * 0.9f;
            if (r < 0.03f) continue;
            float q[4];
            tree_standing_rotation(t, q);
            const float top_local[3] = {0.0f, std::fmax(m.crown_base, 0.5f * m.top) * s, 0.0f};
            float top[3];
            qrot(q, top_local, top);
            const float a[3] = {t.x, t.y - 0.5f, t.z};
            const float b[3] = {t.x + top[0], t.y + top[1], t.z + top[2]};
            const uint32_t h = physics_->add_static_capsule(a, b, r);
            if (h) { keep[t.id] = h; collider_y_[t.id] = t.y; }
        }
        for (std::unordered_map<uint32_t, uint32_t>::iterator it = colliders_.begin(); it != colliders_.end(); ++it) {
            physics_->remove_body(it->second);
            collider_y_.erase(it->first);
        }
        colliders_.swap(keep);
    }

    // Every piece as physics has it, the crown of any log that has finished
    // falling, and the rest of one that has stopped moving.
    std::vector<uint64_t> shed;
    for (std::unordered_map<uint64_t, Piece>::iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        Piece& p = it->second;
        bool active = false;
        physics_->body_pose(p.body, p.pos, p.rot, active);
        p.age += dt;
        if (dt <= 0.0f) continue;
        float lin[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
        physics_->body_velocity(p.body, lin, ang);
        // How fast its far end is going. For a bare trunk that is its own
        // surface -- a log turning about its length is rolling, which is
        // motion -- but a tree still on its crown turns about its BUTT,
        // which barely moves while the top is doing twenty metres a second.
        const TreeModel& fm = tr.model(p.model / kTreeVariants, p.model % kTreeVariants);
        const float arm = p.crowned ? ((std::max)(fm.top, p.hi) - p.lo) * p.scale : p.r;
        const float sp = std::sqrt(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]) +
                         std::sqrt(ang[0] * ang[0] + ang[1] * ang[1] + ang[2] * ang[2]) * arm;
        if (!active || sp < kRestSpeed) p.slow_s += dt; else p.slow_s = 0.0f;
        // ... unless the player is leaning on it. A shove moves a log far
        // slower than kRestSpeed, so without this the thing being pushed
        // goes to sleep under the player's shoulder every half second.
        if (physics_->push().on && physics_->push().body == p.body) p.slow_s = 0.0f;
        const float up_y = 1.0f - 2.0f * (p.rot[0] * p.rot[0] + p.rot[2] * p.rot[2]);
        const float tilt = std::acos((std::max)(-1.0f, (std::min)(1.0f, up_y))) * 57.29578f;
        if (p.crowned && p.hinge && tilt >= tune_.hinge_release_deg) release_hinge(p);
        if (p.crowned) {
            // The crown comes off the moment it MEETS THE GROUND, which
            // in the open is the first thing a falling tree touches at all.
            // Two things it deliberately does not count:
            //  - a contact on the BOLE. That is the stump the tree is
            //    pivoting on, and the tree that knocked this one down, and
            //    neither takes a twig off the top.
            //  - a contact with another PROP -- a standing trunk, a rock, a
            //    log. A tree falling through a wood tangles with its
            //    neighbours and breaks limbs on them; it does not shed its
            //    whole canopy, and the canopy is also the only part of it
            //    that reaches a tree fifteen metres away. Shedding on those
            //    too, a faller in a close wood was a bare bole by twenty
            //    degrees and could not knock anything down at all.
            // The rest rule and the timer stay behind both, for a tree that
            // ends up hung up in its neighbours and never reaches the
            // ground at all.
            const bool met = crown_touched(p);
            if (met || p.slow_s >= kRestFor || p.age >= tune_.shed_after_s)
                shed.push_back(p.key);
        } else if (p.slow_s >= kRestFor && active) {
            physics_->rest_body(p.body);
            p.slow_s = 0.0f;
        }
    }
    // BEFORE the shed: a tree whose crown has just swept into a neighbour
    // has touched something, so this is the frame it drops that crown, and
    // the blow it struck is the whole tree's.
    impacts_frame();
    for (uint64_t k : shed) {
        std::unordered_map<uint64_t, Piece>::iterator it = pieces_.find(k);
        if (it != pieces_.end()) shed_crown(it->second);
    }

    // The debris: it falls, it stops, it lies there, it goes.
    for (size_t i = 0; i < debris_.size();) {
        Debris& d = debris_[i];
        d.age += dt;
        if (d.body) {
            bool active = true;
            physics_->body_pose(d.body, d.pos, d.rot, active);
            float lin[3] = {0, 0, 0}, ang[3] = {0, 0, 0};
            physics_->body_velocity(d.body, lin, ang);
            const float sp = std::sqrt(lin[0] * lin[0] + lin[1] * lin[1] + lin[2] * lin[2]);
            if (!active || sp < kDebrisSlow) d.slow_s += dt; else d.slow_s = 0.0f;
            if (d.slow_s >= kDebrisSlowFor || d.age >= kDebrisAir) {
                physics_->remove_body(d.body);
                d.body = 0;
                const float tx = d.pos[0] - d.spawn[0], tz = d.pos[2] - d.spawn[2];
                const float travel = std::sqrt(tx * tx + tz * tz);
                ++debris_stats_.stopped;
                debris_stats_.travel_sum += travel;
                debris_stats_.stop_sum += d.age;
                if (travel > debris_stats_.travel_max) debris_stats_.travel_max = travel;
            }
        } else {
            d.still_s += dt;
        }
        if (d.still_s > tune_.debris_rest_s) {
            d.fade -= dt / (std::max)(0.05f, tune_.debris_fade_s);
        }
        if (d.fade <= 0.0f) {
            if (d.body) physics_->remove_body(d.body);
            debris_[i] = debris_.back();
            debris_.pop_back();
            continue;
        }
        ++i;
    }

    // What the renderer draws of all of it: a stump per felled tree, a band
    // of the mesh per piece, and a band of the crown per clump of debris.
    std::vector<TreeGpuInstance> extra;
    extra.reserve(stumps_.size() + pieces_.size() + debris_.size());
    for (std::unordered_map<uint32_t, Stump>::const_iterator it = stumps_.begin(); it != stumps_.end(); ++it) {
        const Stump& s = it->second;
        TreeGpuInstance g = {};
        for (int k = 0; k < 3; ++k) g.pos[k] = s.base[k];
        for (int k = 0; k < 4; ++k) g.rot[k] = s.rot[k];
        g.scale = s.scale;
        g.model = s.model;
        g.mode = tree_mode(kTreeStump);
        g.hit_time = -100.0f;
        g.id = s.id;
        extra.push_back(g);
    }
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        const Piece& p = it->second;
        TreeGpuInstance g = {};
        for (int k = 0; k < 3; ++k) g.pos[k] = p.pos[k];
        for (int k = 0; k < 4; ++k) g.rot[k] = p.rot[k];
        g.scale = p.scale;
        g.model = p.model;
        g.hit_time = p.hit_time;
        g.id = p.tree;
        if (p.crowned) {
            // Still a whole tree above the cut: crown and all, as it falls.
            g.mode = tree_mode(kTreeLog);
        } else {
            g.mode = tree_mode(kTreePiece);
            g.clip_lo = p.lo;
            g.clip_hi = p.hi;
            g.cap_lo = p.cap_lo;
            g.cap_hi = p.cap_hi;
            g.axis_lo[0] = p.ax_lo[0]; g.axis_lo[1] = p.ax_lo[1];
            g.axis_hi[0] = p.ax_hi[0]; g.axis_hi[1] = p.ax_hi[1];
        }
        extra.push_back(g);
    }
    for (size_t i = 0; i < debris_.size(); ++i) {
        const Debris& d = debris_[i];
        TreeGpuInstance g = {};
        // d.pos is the band's middle; the instance wants the model's origin.
        const float off_local[3] = {0.0f, 0.5f * (d.lo + d.hi) * d.scale, 0.0f};
        float off[3];
        qrot(d.rot, off_local, off);
        for (int k = 0; k < 3; ++k) g.pos[k] = d.pos[k] - off[k];
        for (int k = 0; k < 4; ++k) g.rot[k] = d.rot[k];
        g.scale = d.scale;
        g.model = d.model;
        g.mode = tree_mode(kTreeDebris, d.fade);
        g.hit_time = -100.0f;
        g.id = d.tree;
        g.clip_lo = d.lo;
        g.clip_hi = d.hi;
        extra.push_back(g);
    }
    renderer_->set_tree_extras(extra);
}

void TreeFelling::set_player(const float feet[3], float height, float radius) {
    for (int k = 0; k < 3; ++k) player_[k] = feet[k];
    player_h_ = height;
    player_r_ = radius > 0.05f ? radius : 0.05f;
}

// Has anything touched this tree ABOVE its bole since the last step? The
// contact comes back in world metres, so it is taken into the model's own
// frame and compared with the band the crown occupies: `hi` is the top of
// the bole, everything over it is branches.
//
// Why not any contact at all. A hinged tree pivots against its own stump on
// the way over, and a tree knocked down by another is struck on its TRUNK by
// the faller -- both are contacts, and neither takes a twig off the top.
bool TreeFelling::crown_touched(const Piece& p) const {
    if (!p.body) return false;
    const std::vector<PhysicsWorld::Touch>& ts = physics_->touches();
    for (size_t i = 0; i < ts.size(); ++i) {
        if (ts[i].body != p.body || ts[i].prop) continue;
        float local[3];
        world_point(p.pos, p.rot, p.scale, ts[i].point, local);
        if (local[1] > p.hi) return true;
    }
    return false;
}

// A tree whose crown has met something lets it go: the body becomes the
// bole's capsule alone (same pose, same motion), and the crown above it
// becomes a few clumps thrown along the way the tree was already going.
// The trunk slips off its stump once it is far enough over: past the
// release angle the hinge is a lie, and holding it there would stand the
// butt up in the air for the whole rest of the fall.
void TreeFelling::release_hinge(Piece& p) {
    if (!p.hinge) return;
    physics_->remove_hinge(p.hinge);
    p.hinge = 0;
    physics_->set_body_damping(p.body, 0.1f, 0.6f);   // what a landing log wants
}

void TreeFelling::shed_crown(Piece& p) {
    release_hinge(p);
    const TreeRenderer& tr = renderer_->trees();
    const TreeModel& m = tr.model(p.model / kTreeVariants, p.model % kTreeVariants);
    float vel[3] = {0, 0, 0}, spin[3] = {0, 0, 0};
    bool active = false;
    physics_->body_velocity(p.body, vel, spin);
    physics_->body_pose(p.body, p.pos, p.rot, active);
    // The bole's new body turns about ITS OWN centre, which is metres down
    // the trunk from the whole tree's. Handed the whole tree's velocity it
    // would be given the speed of a point it no longer contains -- which,
    // now that the crown comes off mid-fall rather than off a log lying
    // still, is the difference between a trunk settling and one launched.
    float mid[3];
    model_point(p.pos, p.rot, p.scale, 0.5f * (p.ax_lo[0] + p.ax_hi[0]),
                0.5f * (p.lo + p.hi), 0.5f * (p.ax_lo[1] + p.ax_hi[1]), mid);
    physics_->body_point_velocity(p.body, mid, vel);
    p.crowned = false;
    physics_->watch_contacts(p.body, false);
    physics_->remove_body(p.body);
    const float a[3] = {p.ax_lo[0] * p.scale, p.lo * p.scale, p.ax_lo[1] * p.scale};
    const float b[3] = {p.ax_hi[0] * p.scale, p.hi * p.scale, p.ax_hi[1] * p.scale};
    p.body = physics_->add_piece(p.pos, p.rot, p.r, a, b, vel, spin, false);

    // The crown: everything above the bole, in equal bands. Each goes where
    // it was already going -- the body's own velocity at that point -- so
    // the crown carries on with the fall instead of dropping on the stump.
    const float top = m.top;
    const int n = tune_.debris_pieces;
    if (n <= 0 || top <= p.hi + 0.5f) return;
    for (int i = 0; i < n; ++i) {
        Debris d;
        d.tree = p.tree;
        d.model = p.model;
        d.scale = p.scale;
        d.lo = p.hi + (top - p.hi) * float(i) / float(n);
        d.hi = p.hi + (top - p.hi) * float(i + 1) / float(n);
        for (int k = 0; k < 4; ++k) d.rot[k] = p.rot[k];
        // The sphere stands at the band's own middle, so `pos` -- which is
        // the MODEL's origin, as every tree instance's is -- is that point
        // less the band's offset turned into the world.
        const float mid = 0.5f * (d.lo + d.hi);
        float centre[3];
        model_point(p.pos, p.rot, p.scale, 0.0f, mid, 0.0f, centre);
        // Thrown along the way it was going, at debris_toss of the speed
        // that part of the tree had -- which, now that the crown comes off
        // against the ground rather than off a log already lying still, is
        // most of a tip doing eight metres a second: the clumps scatter
        // ahead of the fall (1.2-1.6 m, worst 1.9) rather than spill off a
        // trunk that has stopped.
        // The point velocity of the BOLE's body, extrapolated to where the
        // crown was: a rigid body's field is v + w x r wherever r points, so
        // this is the speed that part of the tree had the instant before it
        // came apart.
        float pv[3] = {0, 0, 0};
        physics_->body_point_velocity(p.body, centre, pv);
        for (int k = 0; k < 3; ++k) pv[k] *= tune_.debris_toss;
        // A clump is as wide as the crown reaches at its height, and never
        // wider than half the gap to the next one: at 0.75 of the band the
        // three overlapped deeply, shoved each other apart for ever and
        // kept knocking the log, which never came to rest.
        const float band = (d.hi - d.lo) * p.scale;
        const float r = (std::min)(m.radius * p.scale * 0.5f, band * 0.45f);
        for (int k = 0; k < 3; ++k) d.pos[k] = d.spawn[k] = centre[k];
        d.body = physics_->add_debris(centre, d.rot, (std::max)(0.3f, r), 6.0f, pv, spin);
        if (d.body) debris_.push_back(d);
    }
}

void TreeFelling::fell(uint32_t id, uint32_t model, float scale, const float base[3],
                       const float stand_rot[4], const float away[3], bool knocked) {
    const TreeRenderer& tr = renderer_->trees();
    const TreeModel& m = tr.model(model / kTreeVariants, model % kTreeVariants);
    const float s = scale;
    const float r = m.trunk_radius * s * 0.85f;

    // The tree stops standing: out of the field's list, its collider gone.
    renderer_->tree_field().set_felled(id, true);
    damage_.erase(id);
    std::unordered_map<uint32_t, uint32_t>::iterator c = colliders_.find(id);
    if (c != colliders_.end()) { physics_->remove_body(c->second); colliders_.erase(c); }

    // The stump: a short static capsule whose top is the cut.
    Stump st;
    st.id = id;
    st.model = model;
    st.scale = s;
    for (int k = 0; k < 3; ++k) st.base[k] = base[k];
    for (int k = 0; k < 4; ++k) st.rot[k] = stand_rot[k];
    st.r = r;
    st.top = kCutHeight * s;
    if (!stumps_.count(id)) {
        const float a[3] = {base[0], base[1] - 0.4f, base[2]};
        const float b[3] = {base[0], base[1] + kCutHeight * s - r, base[2]};
        st.body = physics_->add_static_capsule(a, b, r);
        stumps_[id] = st;
    }

    // The log: the bole, from a radius above the cut -- so it rests ON the
    // stump rather than inside it -- to where the crown begins, or half the
    // height, whichever is higher; a fir's crown starts low.
    Piece seed;
    seed.tree = id;
    seed.model = model;
    seed.scale = s;
    const float y0 = kCutHeight + (r + 0.02f) / (std::max)(0.05f, s);
    const float y1 = (std::max)((std::max)(m.crown_base, 0.5f * m.top), y0 + 0.5f / (std::max)(0.05f, s));
    // The axis it goes over about: up x away, horizontal, square to the
    // way it is falling. The nudge is tiny -- just enough to break an
    // upright tree's equilibrium, since a pendulum balanced exactly on its
    // end never falls -- and gravity does the rest, slowly at first and
    // faster as it goes. 0.35 rad/s here read as a shove.
    const float axis[3] = {away[2], 0.0f, -away[0]};
    // A CUT tree only needs breaking out of the vertical: it is going the
    // way it already leans and gravity does the rest, slowly at first and
    // faster as it goes over. A KNOCKED one is being driven somewhere it
    // may not want to go -- across its own lean, which a hinge at the
    // stump would simply refuse, leaving it standing at three degrees for
    // as long as anyone watched -- so it is thrown instead, which is what
    // being hit by three tonnes of oak amounts to.
    const float k = knocked ? tune_.knock_spin_rad_s : tune_.topple_spin_rad_s;
    const float spin[3] = {axis[0] * k, 0.0f, axis[2] * k};
    const float none[3] = {0, 0, 0};
    Piece* p = make_piece(seed, y0, y1, 0, base, stand_rot, none, none, true);
    if (!p) return;
    // Mid-fall it is still a whole tree, so its body is the WHOLE TRUNK,
    // cut to top -- which is also exactly what is drawn until the crown
    // sheds. It used to be the bole plus a sphere for the crown, and that
    // sphere is 5.7 m across on a big tree and sits 15 m up, right where
    // its neighbours' own trunk capsules are: a tree in a wood was created
    // already jammed against three of them. As a free body it shoved its
    // way out, which was half of why the butt jumped; pinned to its stump
    // it simply could not fall at all (3 degrees after ten seconds). A
    // trunk-thick capsule passes between its neighbours' crowns, which
    // carry no collision anyway, and still meets their TRUNKS, which is
    // the collision worth having. The length is the whole tree's, so it
    // swings on the hinge at a 24 m tree's rate rather than a 6 m bole's --
    // but the MASS over the bole is only crown_mass_share of it, because
    // branches and leaves are mostly air and a solid trunk-thick crown put
    // the centre of mass at half the height instead of a third.
    physics_->remove_body(p->body);
    p->crowned = true;
    const std::vector<float>& prof = m.lod[0].trunk_y;
    const float trunk_top = prof.empty() ? m.top : prof.back();
    p->body = physics_->add_log(p->pos, p->rot, p->r, y0 * s, y1 * s,
                                (std::max)(trunk_top, y1) * s, tune_.crown_mass_share,
                                spin, false);
    // Watched, so the first thing its crown meets takes the crown off.
    if (p->body) physics_->watch_contacts(p->body, true);
    // ... and it PIVOTS ON ITS STUMP while it goes. Without this the log is
    // a free body turning about its own centre of mass, halfway up the
    // trunk, so the butt swings out from under it the instant it is cut.
    if (!knocked && st.body && p->body) {
        p->hinge = physics_->add_hinge(p->body, st.body, base, axis);
        // ... and nothing damps a tree going over. The 0.6 a resting log
        // wants is the dominant term near the vertical, where gravity's
        // torque is almost nothing: it held a 24 m tree at 3 degrees for
        // ten seconds. `release_hinge` puts it back.
        if (p->hinge) physics_->set_body_damping(p->body, 0.05f, 0.02f);
    }
}

bool TreeFelling::fell_toward(uint32_t id, const float dir[3]) {
    if (!renderer_ || !physics_ || !renderer_->trees().ready()) return false;
    const TreeRenderer& tr = renderer_->trees();
    const std::vector<worldgen::TreeInstance>& st = renderer_->tree_field().standing();
    for (size_t i = 0; i < st.size(); ++i) {
        if (st[i].id != id) continue;
        const worldgen::TreeInstance t = st[i];
        const float s = t.height / tr.look(t.species).nominal_height;
        float away[3] = {dir[0], 0.0f, dir[2]};
        const float al = std::sqrt(away[0] * away[0] + away[2] * away[2]);
        if (al > 1e-4f) { away[0] /= al; away[2] /= al; } else { away[0] = 1.0f; }
        float rot[4];
        tree_standing_rotation(t, rot);
        const float base[3] = {t.x, t.y, t.z};
        fell(t.id, t.species * kTreeVariants + (t.variant % kTreeVariants), s, base, rot, away, true);
        return true;
    }
    return false;
}

// A falling trunk against everything else: the trees still standing, and
// the logs already lying about.
//
// ENERGY, not force -- and the energy the blow actually delivers, which is
// two things the first version had wrong. The mass is what the body
// PRESENTS AT THE POINT that touched, along the line the blow pushes
// (`PhysicsWorld::body_impact_mass`), not a flat half of the trunk: a crown
// is a tenth of the tree's mass eight metres off its centre of mass, so it
// meets a few tens of kilogrammes, and the middle of a bole meets the whole
// log. And the speed is the CLOSING speed along that line, so a crown
// sweeping PAST a trunk does nothing, where its full speed counted before.
// Between them they are what the eye reads as "it came down hard on it"
// rather than "it was in the way", and they are why a wood no longer goes
// over like dominoes.
//
// A log on the ground takes the same blow against its own health: enough
// and a whole one breaks in two, enough again and a half comes apart into
// rounds of firewood. It needs no knock -- it is a real body and the
// collision has already shoved it.
//
// One blow per pair, so a log that comes to rest leaning on a trunk does
// not saw it down.
void TreeFelling::impacts_frame() {
    if (pieces_.empty()) return;
    const TreeRenderer& tr = renderer_->trees();
    const std::vector<worldgen::TreeInstance>& st = renderer_->tree_field().standing();
    struct Knock { uint32_t id = 0; float away[3] = {0, 0, 0}; };
    std::vector<Knock> knocked;
    struct Hurt { uint64_t key = 0; float damage = 0.0f; };
    std::vector<Hurt> hurt;
    auto hurt_so_far = [&](uint64_t key) {
        float d = 0.0f;
        for (size_t i = 0; i < hurt.size(); ++i) if (hurt[i].key == key) d += hurt[i].damage;
        return d;
    };
    for (std::unordered_map<uint64_t, Piece>::iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        Piece& p = it->second;
        // A half or a billet carries nothing worth passing on to a TREE --
        // but a round of firewood rolling down a slope is exactly the sort
        // of thing that should catch someone's legs, so every stage is
        // tested against the player. The energy floor is what decides
        // whether it is worth anything: a 111 kg round at 4 m/s is under a
        // kilojoule and does nothing at all.
        bool active = false;
        float pos[3], rot[4];
        if (!physics_->body_pose(p.body, pos, rot, active) || !active) continue;
        float vel[3], spin[3];
        if (!physics_->body_velocity(p.body, vel, spin)) continue;
        const float sp = std::sqrt(vel[0] * vel[0] + vel[1] * vel[1] + vel[2] * vel[2]);
        const float wl = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]);
        const float len = (p.hi - p.lo) * p.scale;
        if (sp + wl * len < tune_.impact_min_speed) continue;
        // Leaning, not falling: see impact_lean_deg. A log on the ground is
        // on its side, so this never holds one back.
        const float up_y = 1.0f - 2.0f * (rot[0] * rot[0] + rot[2] * rot[2]);
        if (std::acos((std::max)(-1.0f, (std::min)(1.0f, up_y))) * 57.29578f < tune_.impact_lean_deg)
            continue;
        // While it still has its crown the whole TREE is swinging, and the
        // trunk runs on past the bole: a 23 m tree whose bole is 7 m would
        // otherwise sweep nothing past 7 m and never reach what it is
        // falling on. Once shed, the trunk is all there is.
        float a[3], b[3];
        const TreeModel& fm = tr.model(p.model / kTreeVariants, p.model % kTreeVariants);
        const float reach = p.crowned ? (std::max)(p.hi, fm.top) : p.hi;
        float rx = p.ax_hi[0], rz = p.ax_hi[1], rr = 0.0f;
        if (p.crowned) trunk_axis_at(fm, reach, rx, rz, rr);
        model_point(pos, rot, p.scale, p.ax_lo[0], p.lo, p.ax_lo[1], a);
        model_point(pos, rot, p.scale, rx, reach, rz, b);
        float crown_c[3] = {0, 0, 0};
        const float crown_r = p.crowned ? fm.crown_r * p.scale : 0.0f;
        if (p.crowned) model_point(pos, rot, p.scale, 0.0f, fm.crown_centre, 0.0f, crown_c);

        // Does the faller meet the segment [c, d] of something `target_r`
        // thick, and if so with what? `at` is where it touched, `n` the way
        // the blow pushes, and the energy is the closing speed along n
        // against the mass the body meets there.
        struct Hit { float at[3]; float n[3]; float speed; float mass; float energy; };
        auto strike = [&](const float c[3], const float d[3], float target_r, Hit& h) -> bool {
            float u = 0.0f, v = 0.0f;
            const float d2 = seg_seg(a, b, c, d, u, v);
            const float hit_r = p.r + target_r;
            bool touched = d2 <= hit_r * hit_r;
            float onto[3];
            if (touched) {
                for (int k = 0; k < 3; ++k) h.at[k] = a[k] + (b[k] - a[k]) * u;
                for (int k = 0; k < 3; ++k) onto[k] = c[k] + (d[k] - c[k]) * v;
            } else if (crown_r > 0.05f) {
                // The CROWN is what usually reaches: a tree toppling about
                // its butt comes down on its branches, and they stop it at
                // about 15 degrees past the diagonal, so the trunk line
                // never descends onto a neighbour's own trunk at all.
                float w = 0.0f;
                const float cd2 = point_seg(crown_c, c, d, w);
                if (cd2 > (crown_r + target_r) * (crown_r + target_r)) return false;
                touched = true;
                for (int k = 0; k < 3; ++k) h.at[k] = crown_c[k];
                for (int k = 0; k < 3; ++k) onto[k] = c[k] + (d[k] - c[k]) * w;
            }
            if (!touched) return false;
            // How fast it was going THERE -- a tree turning about its base
            // has a still butt and a fast top -- and how much of that is
            // INTO what it hit rather than past it.
            float pv[3];
            if (!physics_->body_point_velocity(p.body, h.at, pv)) return false;
            // The blow is the part of that going INTO what it met: the
            // closing speed along the line between them. A crown sweeping
            // PAST a trunk at the same height closes on it hardly at all,
            // where its full speed used to count in full; a crown coming
            // down on a shorter tree finds the top of that trunk below it,
            // so the line tilts upward and the fall's own speed is the blow.
            float n[3];
            for (int k = 0; k < 3; ++k) n[k] = onto[k] - h.at[k];
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl < 1e-3f) {
                // Dead centre: take the way it is travelling as the push.
                const float pl = std::sqrt(pv[0] * pv[0] + pv[1] * pv[1] + pv[2] * pv[2]);
                if (pl < 1e-3f) return false;
                for (int k = 0; k < 3; ++k) n[k] = pv[k] / pl;
            } else {
                for (int k = 0; k < 3; ++k) n[k] /= nl;
            }
            h.speed = pv[0] * n[0] + pv[1] * n[1] + pv[2] * n[2];
            if (h.speed < tune_.impact_min_speed) return false;
            // Which way it is SHOVED is the way the faller is travelling,
            // flattened -- a tree goes over sideways, never down.
            const float hl = std::sqrt(pv[0] * pv[0] + pv[2] * pv[2]);
            if (hl > 1e-3f) { h.n[0] = pv[0] / hl; h.n[1] = 0.0f; h.n[2] = pv[2] / hl; }
            else { h.n[0] = n[0]; h.n[1] = 0.0f; h.n[2] = n[2]; }
            // The mass it meets there. The fallback is the old flat half of
            // the trunk, so a body physics cannot read still does something
            // -- and the report prints the mass, so it shows if it happens.
            if (!physics_->body_impact_mass(p.body, h.at, n, h.mass) || h.mass <= 0.0f)
                h.mass = 0.5f * p.mass;
            h.energy = 0.5f * h.mass * h.speed * h.speed;
            // A GRAZE does nothing, and -- this is the point -- does not use
            // up the pair's one blow. A crown is six metres across and is
            // already pressed against its neighbours while the tree stands,
            // so the first contact of a fall is never the blow: it is the
            // tree leaning on what it was leaning on. Marked struck there,
            // every tree spent its blow at 2-3 m/s in the first fifth of a
            // second and the real landing counted for nothing. A blow worth
            // less than ONE SWING OF THE AXE is that graze -- bark, twigs
            // and spring take it -- so it is not recorded at all, and the
            // same pair is asked again next frame, when the crown is coming
            // down faster.
            return h.energy >= tune_.impact_per_chop;
        };
        // One entry per pair, added to as the blow grows.
        auto record = [&](uint64_t struck, bool is_piece, int stage, const Hit& h, float dmg,
                          bool done) {
            Impact* im = nullptr;
            for (size_t i = impacts_.size(); i-- > 0;)
                if (impacts_[i].faller == p.tree && impacts_[i].struck == struck) {
                    im = &impacts_[i];
                    break;
                }
            if (!im) {
                if (impacts_.size() > 64) impacts_.erase(impacts_.begin());
                impacts_.push_back(Impact());
                im = &impacts_.back();
                im->faller = p.tree;
                im->struck = struck;
                im->piece = is_piece;
                im->stage = stage;
            }
            im->speed = (std::max)(im->speed, h.speed);
            im->mass = (std::max)(im->mass, h.mass);
            im->energy = (std::max)(im->energy, h.energy);
            im->damage += dmg;
            im->felled = im->felled || (!is_piece && done);
            im->broke = im->broke || (is_piece && done);
        };

        // The trees still standing, and the logs already down: a whole
        // bole only.
        const float lo_x = (std::min)(a[0], b[0]), hi_x = (std::max)(a[0], b[0]);
        const float lo_z = (std::min)(a[2], b[2]), hi_z = (std::max)(a[2], b[2]);
        for (size_t i = 0; p.stage == 0 && i < st.size(); ++i) {
            const worldgen::TreeInstance& t = st[i];
            const TreeModel& m = tr.model(t.species, t.variant);
            const float ts = t.height / tr.look(t.species).nominal_height;
            const float tr_r = m.trunk_radius * ts;
            const float pad = p.r + crown_r + tr_r + 0.5f;
            if (t.x < lo_x - pad || t.x > hi_x + pad || t.z < lo_z - pad || t.z > hi_z + pad) continue;
            // The standing trunk, from its base to where its crown begins.
            const float c[3] = {t.x, t.y, t.z};
            const float d[3] = {t.x, t.y + (std::max)(m.crown_base, 0.5f * m.top) * ts, t.z};
            Hit h;
            if (!strike(c, d, tr_r, h)) continue;
            float& peak = p.struck[uint64_t(t.id)];
            if (h.energy <= peak) continue;
            const float dmg = (h.energy - peak) / tune_.impact_per_chop * tune_.chop_damage;
            peak = h.energy;
            const float health = tree_health(tr_r);
            const float took = damage_[t.id] + dmg;
            damage_[t.id] = took;
            const bool down = took >= health;
            record(uint64_t(t.id), false, 0, h, dmg, down);
            if (!down) continue;
            // It goes down the way it was pushed, and the faller carries on
            // with what it has left.
            Knock k;
            k.id = t.id;
            const float hl = std::sqrt(h.n[0] * h.n[0] + h.n[2] * h.n[2]);
            if (hl > 1e-3f) { k.away[0] = h.n[0] / hl; k.away[2] = h.n[2] / hl; }
            else { k.away[0] = 1.0f; }
            knocked.push_back(k);
            float lv[3], av[3];
            for (int kk = 0; kk < 3; ++kk) { lv[kk] = vel[kk] * tune_.impact_keep; av[kk] = spin[kk] * tune_.impact_keep; }
            physics_->set_body_velocity(p.body, lv, av);
        }

        // ... and the logs already on the ground. A round of firewood is
        // left out: it is already the thing a log breaks down into, so
        // there is nothing for a blow to do to it but shove it, which the
        // contact has done.
        for (std::unordered_map<uint64_t, Piece>::const_iterator jt = pieces_.begin();
             p.stage == 0 && jt != pieces_.end(); ++jt) {
            const Piece& q = jt->second;
            if (q.key == p.key || q.stage > 1) continue;
            float c[3], d[3];
            model_point(q.pos, q.rot, q.scale, q.ax_lo[0], q.lo, q.ax_lo[1], c);
            model_point(q.pos, q.rot, q.scale, q.ax_hi[0], q.hi, q.ax_hi[1], d);
            Hit h;
            if (!strike(c, d, q.r, h)) continue;
            float& peak = p.struck[q.key];
            if (h.energy <= peak) continue;
            const float dmg = (h.energy - peak) / tune_.impact_per_chop * tune_.chop_damage;
            peak = h.energy;
            Hurt hu;
            hu.key = q.key;
            hu.damage = dmg;
            const bool breaks = q.health - hurt_so_far(q.key) - dmg <= 0.0f;
            hurt.push_back(hu);
            record(q.key, true, q.stage, h, dmg, breaks);
        }

        // ... and the player, standing in the way. One blow per pair as
        // ever, by the same peak-of-the-load rule: a tree bearing down on
        // them goes on hurting, a log lying on them does not.
        if (player_h_ > 0.1f) {
            const float foot = (std::min)(player_r_, 0.45f * player_h_);
            const float c[3] = {player_[0], player_[1] + foot, player_[2]};
            const float d[3] = {player_[0], player_[1] + player_h_ - foot, player_[2]};
            Hit h;
            if (strike(c, d, player_r_, h)) {
                float& peak = p.struck[kPlayerKey];
                if (h.energy > peak) {
                    PlayerHit* ph = nullptr;
                    for (size_t i = player_hits_.size(); i-- > 0;)
                        if (player_hits_[i].tree == p.tree && player_hits_[i].stage == p.stage) {
                            ph = &player_hits_[i];
                            break;
                        }
                    if (!ph) {
                        if (player_hits_.size() > 32) player_hits_.erase(player_hits_.begin());
                        player_hits_.push_back(PlayerHit());
                        ph = &player_hits_.back();
                        ph->tree = p.tree;
                        ph->stage = p.stage;
                    }
                    ph->speed = (std::max)(ph->speed, h.speed);
                    ph->mass = (std::max)(ph->mass, h.mass);
                    ph->energy = (std::max)(ph->energy, h.energy);
                    ph->damage += (h.energy - peak) / tune_.impact_per_chop * tune_.chop_damage;
                    peak = h.energy;
                }
            }
        }
    }

    // What the blows did, once nothing is iterating the pieces any more:
    // split() and billet() both rebuild the map.
    for (size_t i = 0; i < hurt.size(); ++i) {
        std::unordered_map<uint64_t, Piece>::iterator q = pieces_.find(hurt[i].key);
        if (q == pieces_.end()) continue;
        q->second.health -= hurt[i].damage;
        if (q->second.health > 0.0f) continue;
        // No hit flash: impacts_frame has no clock, and a log broken by a
        // tree landing on it does not need the axe's.
        if (q->second.stage == 0) split(q->second, -100.0f);
        else billet(q->second, -100.0f);
    }
    for (size_t i = 0; i < knocked.size(); ++i) {
        const TreeRenderer& trr = renderer_->trees();
        const std::vector<worldgen::TreeInstance>& now = renderer_->tree_field().standing();
        for (size_t j = 0; j < now.size(); ++j) {
            if (now[j].id != knocked[i].id) continue;
            const worldgen::TreeInstance t = now[j];
            const float s = t.height / trr.look(t.species).nominal_height;
            float rot[4];
            tree_standing_rotation(t, rot);
            const float base[3] = {t.x, t.y, t.z};
            fell(t.id, t.species * kTreeVariants + (t.variant % kTreeVariants), s, base, rot,
                 knocked[i].away, true);
            break;
        }
    }
}

int TreeFelling::chop(const float eye[3], const float dir[3], float time, float damage, Blow* out) {
    Blow blow;
    if (out) *out = blow;
    if (!renderer_ || !physics_ || !renderer_->trees().ready()) return kChopNothing;
    const TreeRenderer& tr = renderer_->trees();
    const std::vector<worldgen::TreeInstance>& st = renderer_->tree_field().standing();

    // Nearest trunk the ray meets within reach: a vertical cylinder per tree,
    // from the ground to where its crown begins.
    float best_t = kReach;
    const worldgen::TreeInstance* best = nullptr;
    uint64_t best_piece = 0;
    const float hx = dir[0], hz = dir[2];
    const float hl2 = hx * hx + hz * hz;
    for (size_t i = 0; i < st.size(); ++i) {
        const worldgen::TreeInstance& t = st[i];
        const float ox = eye[0] - t.x, oz = eye[2] - t.z;
        if (ox * ox + oz * oz > 64.0f) continue;
        const TreeModel& m = tr.model(t.species, t.variant);
        const float s = t.height / tr.look(t.species).nominal_height;
        const float r = m.trunk_radius * s * 1.15f;   // a little generous: bark, flare
        if (hl2 < 1e-6f) continue;
        // |o + d t|^2 = r^2 in the plane.
        const float a = hl2, b = 2.0f * (ox * hx + oz * hz), c = ox * ox + oz * oz - r * r;
        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) continue;
        float tt = (-b - std::sqrt(disc)) / (2.0f * a);
        if (tt < 0.0f) tt = 0.0f;   // standing inside the bark counts
        if (tt > best_t) continue;
        const float y = eye[1] + dir[1] * tt - t.y;
        if (y < -0.3f || y > std::fmax(m.crown_base, 0.5f * m.top) * s) continue;
        best_t = tt;
        best = &t;
    }
    // ... and the logs lying about, which are capsules. A round of firewood
    // is not a chop target: E picks it up, and an axe swung at a pile of it
    // should reach the tree behind.
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        const Piece& p = it->second;
        if (p.stage >= 2) continue;
        float a[3], b[3];
        model_point(p.pos, p.rot, p.scale, p.ax_lo[0], p.lo, p.ax_lo[1], a);
        model_point(p.pos, p.rot, p.scale, p.ax_hi[0], p.hi, p.ax_hi[1], b);
        const float e2[3] = {eye[0] + dir[0] * kReach, eye[1] + dir[1] * kReach, eye[2] + dir[2] * kReach};
        float u = 0.0f, v = 0.0f;
        const float d2 = seg_seg(eye, e2, a, b, u, v);
        const float hit_r = p.r + 0.25f;    // the axe has a head, not a point
        if (d2 > hit_r * hit_r) continue;
        const float tt = u * kReach;
        if (tt > best_t) continue;
        best_t = tt;
        best = nullptr;
        best_piece = p.key;
    }

    if (best) {
        const worldgen::TreeInstance t = *best;   // standing() changes if it falls
        const TreeModel& m = tr.model(t.species, t.variant);
        const float s = t.height / tr.look(t.species).nominal_height;
        const float trunk_r = m.trunk_radius * s;
        const float health = tree_health(trunk_r);
        const float took = damage_[t.id] + damage;
        damage_[t.id] = took;
        renderer_->tree_hit(t.id, time);
        blow.tree = t.id;
        blow.need = hits_needed(trunk_r);
        blow.hits = (std::min)(blow.need, int(took / (std::max)(0.01f, tune_.chop_damage) + 0.999f));
        if (took < health) {
            blow.result = kChopHit;
            if (out) *out = blow;
            return kChopHit;
        }
        float rot[4];
        tree_standing_rotation(t, rot);
        // Where it goes is the TREE's business, not the axe's: the way it
        // already leans, and a heading of its own from its id if it stands
        // too straight to have one. Being able to aim a fall by walking
        // round the trunk was most of what made it read as a kick.
        float away[3];
        if (!topple_lean(rot, away)) topple_heading(t.id, away);
        const float base[3] = {t.x, t.y, t.z};
        fell(t.id, t.species * kTreeVariants + (t.variant % kTreeVariants), s, base, rot, away, false);
        blow.result = kChopFelled;
        blow.hits = blow.need;
        if (out) *out = blow;
        return kChopFelled;
    }

    if (!best_piece) return kChopNothing;
    std::unordered_map<uint64_t, Piece>::iterator pi = pieces_.find(best_piece);
    if (pi == pieces_.end()) return kChopNothing;
    Piece& p = pi->second;
    p.health -= damage;
    p.hit_time = time;
    blow.tree = p.tree;
    blow.piece = true;
    blow.stage = p.stage;
    blow.need = (std::max)(1, int(p.max_health / (std::max)(0.01f, tune_.chop_damage) + 0.999f));
    blow.hits = (std::min)(blow.need,
                           int((p.max_health - p.health) / (std::max)(0.01f, tune_.chop_damage) + 0.999f));
    if (p.health > 0.0f) {
        blow.result = kChopHit;
        if (out) *out = blow;
        return kChopHit;
    }
    blow.hits = blow.need;
    if (p.stage == 0) {
        split(p, time);
        blow.result = kChopSplit;
    } else {
        blow.wood = billet(p, time);
        blow.result = kChopBilleted;
    }
    if (out) *out = blow;
    return blow.result;
}

// A whole log, chopped through: two halves, each carrying the motion the
// log had there, so one lying on a slope does not simply sit still.
void TreeFelling::split(Piece& p, float time) {
    release_hinge(p);                // it may still be mid-fall on its stump
    const Piece parent = p;          // copied: pieces_ is about to change
    const float at = parent.lo + (parent.hi - parent.lo) * tune_.split_at;
    float vel[3] = {0, 0, 0}, spin[3] = {0, 0, 0};
    physics_->body_velocity(parent.body, vel, spin);
    physics_->remove_body(parent.body);
    pieces_.erase(parent.key);
    const float bands[2][2] = {{parent.lo, at}, {at, parent.hi}};
    for (int i = 0; i < 2; ++i) {
        // A half is nudged apart a little along the trunk, so the two do not
        // start inside each other and shove.
        const float push = i == 0 ? -0.35f : 0.35f;
        float axis[3];
        const float up[3] = {0.0f, 1.0f, 0.0f};
        qrot(parent.rot, up, axis);
        float v[3];
        for (int k = 0; k < 3; ++k) v[k] = vel[k] + axis[k] * push;
        Piece* q = make_piece(parent, bands[i][0], bands[i][1], 1, parent.pos, parent.rot, v, spin, false);
        if (q) q->hit_time = time;
    }
}

// Half a log, chopped through: rounds of firewood. As many as it takes for
// none to be longer than billet_max_m, never fewer than `billets`. Returns
// the wood they are worth between them.
int TreeFelling::billet(Piece& p, float time) {
    release_hinge(p);
    const Piece parent = p;
    const float len = (parent.hi - parent.lo) * parent.scale;
    int n = (std::max)(1, tune_.billets);
    while (float(n) * tune_.billet_max_m < len && n < 6) ++n;
    float vel[3] = {0, 0, 0}, spin[3] = {0, 0, 0};
    physics_->body_velocity(parent.body, vel, spin);
    physics_->remove_body(parent.body);
    pieces_.erase(parent.key);
    int wood = 0;
    for (int i = 0; i < n; ++i) {
        const float lo = parent.lo + (parent.hi - parent.lo) * float(i) / float(n);
        const float hi = parent.lo + (parent.hi - parent.lo) * float(i + 1) / float(n);
        Piece* q = make_piece(parent, lo, hi, 2, parent.pos, parent.rot, vel, spin, false);
        if (q) { q->hit_time = time; wood += q->wood; }
    }
    return wood;
}

bool TreeFelling::aim_wood(const float eye[3], const float dir[3], float reach, Pick& out) const {
    bool found = false;
    float best = reach;
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        const Piece& p = it->second;
        if (p.stage < 2) continue;
        float a[3], b[3];
        model_point(p.pos, p.rot, p.scale, p.ax_lo[0], p.lo, p.ax_lo[1], a);
        model_point(p.pos, p.rot, p.scale, p.ax_hi[0], p.hi, p.ax_hi[1], b);
        const float e2[3] = {eye[0] + dir[0] * reach, eye[1] + dir[1] * reach, eye[2] + dir[2] * reach};
        float u = 0.0f, v = 0.0f;
        const float d2 = seg_seg(eye, e2, a, b, u, v);
        const float hit_r = p.r + 0.2f;
        if (d2 > hit_r * hit_r) continue;
        const float tt = u * reach;
        if (tt > best) continue;
        best = tt;
        out.key = p.key;
        out.wood = p.wood;
        out.dist = tt;
        found = true;
    }
    return found;
}

int TreeFelling::take_wood(uint64_t key) {
    std::unordered_map<uint64_t, Piece>::iterator it = pieces_.find(key);
    if (it == pieces_.end() || it->second.stage < 2) return 0;
    const int wood = it->second.wood;
    physics_->remove_body(it->second.body);
    pieces_.erase(it);
    return wood;
}

void TreeFelling::shapes(std::vector<Shape>& out) const {
    out.clear();
    for (std::unordered_map<uint32_t, Stump>::const_iterator it = stumps_.begin(); it != stumps_.end(); ++it) {
        const Stump& s = it->second;
        Shape st;
        st.id = s.id;
        st.capsule = false;
        st.a[0] = s.base[0]; st.a[1] = s.base[1] - 0.5f; st.a[2] = s.base[2];
        st.b[0] = s.base[0]; st.b[1] = s.base[1] + s.top; st.b[2] = s.base[2];
        st.r = s.r / 0.85f;
        out.push_back(st);
    }
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        const Piece& p = it->second;
        float pos[3], rot[4];
        bool active = false;
        if (!physics_->body_pose(p.body, pos, rot, active)) continue;
        Shape lg;
        lg.id = p.key;
        lg.capsule = true;
        model_point(pos, rot, p.scale, p.ax_lo[0], p.lo, p.ax_lo[1], lg.a);
        model_point(pos, rot, p.scale, p.ax_hi[0], p.hi, p.ax_hi[1], lg.b);
        lg.r = p.r;
        lg.resting = !active;
        out.push_back(lg);
    }
}

bool TreeFelling::log_pose(uint32_t id, float pos[3], float rot[4], bool& active) const {
    const Piece* best = nullptr;
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        if (it->second.tree != id) continue;
        if (!best || it->second.key < best->key) best = &it->second;
    }
    if (!best) return false;
    return physics_->body_pose(best->body, pos, rot, active);
}

bool TreeFelling::piece_state(uint32_t id, int& stage, float& health, float& max_health) const {
    const Piece* best = nullptr;
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        if (it->second.tree != id) continue;
        if (!best || it->second.key < best->key) best = &it->second;
    }
    if (!best) return false;
    stage = best->stage;
    health = best->health;
    max_health = best->max_health;
    return true;
}

bool TreeFelling::piece_motion(uint32_t id, float& lin, float& ang) const {
    const Piece* best = nullptr;
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        if (it->second.tree != id) continue;
        if (!best || it->second.key < best->key) best = &it->second;
    }
    if (!best) return false;
    float v[3] = {0, 0, 0}, w[3] = {0, 0, 0};
    if (!physics_->body_velocity(best->body, v, w)) return false;
    lin = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    ang = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    return true;
}

bool TreeFelling::tree_damage(uint32_t id, float& taken, float& health) const {
    float base[3], h = 0.0f, r = 0.0f;
    int hits = 0;
    if (!tree_info(id, base, h, r, hits)) return false;
    std::unordered_map<uint32_t, float>::const_iterator it = damage_.find(id);
    taken = it == damage_.end() ? 0.0f : it->second;
    health = tree_health(r);
    return true;
}

bool TreeFelling::tree_info(uint32_t id, float base[3], float& height, float& trunk_r, int& hits) const {
    const std::vector<worldgen::TreeInstance>& st = renderer_->tree_field().standing();
    for (size_t i = 0; i < st.size(); ++i) {
        if (st[i].id != id) continue;
        const TreeRenderer& tr = renderer_->trees();
        const TreeModel& m = tr.model(st[i].species, st[i].variant);
        const float s = st[i].height / tr.look(st[i].species).nominal_height;
        base[0] = st[i].x; base[1] = st[i].y; base[2] = st[i].z;
        height = st[i].height;
        trunk_r = m.trunk_radius * s;
        hits = hits_needed(trunk_r);
        return true;
    }
    return false;
}

bool TreeFelling::save(const char* path, uint32_t field_id, std::string& error) const {
    FILE* f = std::fopen(path, "wb");
    if (!f) { error = std::string("could not open ") + path; return false; }
    const uint32_t ns = static_cast<uint32_t>(stumps_.size());
    const uint32_t np = static_cast<uint32_t>(pieces_.size());
    std::fwrite(&kMagic, 4, 1, f);
    std::fwrite(&kFormat, 4, 1, f);
    std::fwrite(&field_id, 4, 1, f);
    std::fwrite(&ns, 4, 1, f);
    std::fwrite(&np, 4, 1, f);
    for (std::unordered_map<uint32_t, Stump>::const_iterator it = stumps_.begin(); it != stumps_.end(); ++it) {
        const Stump& s = it->second;
        std::fwrite(&s.id, 4, 1, f);
        std::fwrite(&s.model, 4, 1, f);
        std::fwrite(&s.scale, 4, 1, f);
        std::fwrite(s.base, 4, 3, f);
        std::fwrite(s.rot, 4, 4, f);
    }
    for (std::unordered_map<uint64_t, Piece>::const_iterator it = pieces_.begin(); it != pieces_.end(); ++it) {
        const Piece& p = it->second;
        const int32_t stage = p.stage;
        std::fwrite(&p.tree, 4, 1, f);
        std::fwrite(&p.model, 4, 1, f);
        std::fwrite(&p.scale, 4, 1, f);
        std::fwrite(&stage, 4, 1, f);
        std::fwrite(&p.lo, 4, 1, f);
        std::fwrite(&p.hi, 4, 1, f);
        std::fwrite(&p.health, 4, 1, f);
        std::fwrite(p.pos, 4, 3, f);
        std::fwrite(p.rot, 4, 4, f);
    }
    std::fclose(f);
    return true;
}

bool TreeFelling::load(const char* path, uint32_t field_id, std::string& error, bool& mismatch) {
    mismatch = false;
    FILE* f = std::fopen(path, "rb");
    if (!f) { error = std::string("could not open ") + path; return false; }
    uint32_t magic = 0, fmt = 0, fid = 0, ns = 0, np = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || std::fread(&fmt, 4, 1, f) != 1 ||
        std::fread(&fid, 4, 1, f) != 1 || std::fread(&ns, 4, 1, f) != 1 ||
        magic != kMagic || fmt != kFormat) {
        std::fclose(f);
        error = std::string("not a tree save: ") + path;
        return false;
    }
    if (std::fread(&np, 4, 1, f) != 1) { std::fclose(f); error = "truncated tree save"; return false; }
    if (fid != field_id) {
        // A different world: its tree ids name other trees.
        std::fclose(f);
        mismatch = true;
        error = "tree save is for a different world";
        return false;
    }
    const TreeRenderer& tr = renderer_->trees();
    for (uint32_t i = 0; i < ns; ++i) {
        Stump s;
        if (std::fread(&s.id, 4, 1, f) != 1 || std::fread(&s.model, 4, 1, f) != 1 ||
            std::fread(&s.scale, 4, 1, f) != 1 || std::fread(s.base, 4, 3, f) != 3 ||
            std::fread(s.rot, 4, 4, f) != 4) {
            std::fclose(f);
            error = "truncated tree save";
            return false;
        }
        if (stumps_.count(s.id)) continue;
        const TreeModel& m = tr.model(s.model / kTreeVariants, s.model % kTreeVariants);
        s.r = m.trunk_radius * s.scale * 0.85f;
        s.top = kCutHeight * s.scale;
        renderer_->tree_field().set_felled(s.id, true);
        damage_.erase(s.id);
        std::unordered_map<uint32_t, uint32_t>::iterator c = colliders_.find(s.id);
        if (c != colliders_.end()) { physics_->remove_body(c->second); colliders_.erase(c); }
        const float a[3] = {s.base[0], s.base[1] - 0.4f, s.base[2]};
        const float b[3] = {s.base[0], s.base[1] + s.top - s.r, s.base[2]};
        s.body = physics_->add_static_capsule(a, b, s.r);
        stumps_[s.id] = s;
    }
    for (uint32_t i = 0; i < np; ++i) {
        Piece seed;
        int32_t stage = 0;
        float lo = 0.0f, hi = 0.0f, health = 0.0f, pos[3], rot[4];
        if (std::fread(&seed.tree, 4, 1, f) != 1 || std::fread(&seed.model, 4, 1, f) != 1 ||
            std::fread(&seed.scale, 4, 1, f) != 1 || std::fread(&stage, 4, 1, f) != 1 ||
            std::fread(&lo, 4, 1, f) != 1 || std::fread(&hi, 4, 1, f) != 1 ||
            std::fread(&health, 4, 1, f) != 1 || std::fread(pos, 4, 3, f) != 3 ||
            std::fread(rot, 4, 4, f) != 4) {
            std::fclose(f);
            error = "truncated tree save";
            return false;
        }
        const float none[3] = {0, 0, 0};
        // Asleep where it came to rest; the first touch wakes it. Its crown
        // went when it fell, whenever that was.
        Piece* p = make_piece(seed, lo, hi, stage < 0 ? 0 : (stage > 2 ? 2 : stage), pos, rot,
                              none, none, true);
        if (p && health > 0.0f) p->health = (std::min)(p->max_health, health);
    }
    std::fclose(f);
    return true;
}

}  // namespace engine
