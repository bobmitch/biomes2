#include "engine/trees/felling.h"
#include "engine/physics/physics_world.h"
#include "engine/render/renderer.h"

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

const uint32_t kMagic = 0x45455254u;   // "TREE"
const uint32_t kFormat = 1u;

}  // namespace

int TreeFelling::hits_needed(float trunk_r) const {
    // Two swings and one more per 6 cm of diameter: a birch sapling in a
    // couple, an old oak in about a dozen.
    int n = int(2.0f + (2.0f * trunk_r) / 0.06f + 0.5f);
    return n < 2 ? 2 : (n > 14 ? 14 : n);
}

void TreeFelling::update(float x, float z) {
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
            if (it != colliders_.end()) { keep[t.id] = it->second; colliders_.erase(it); continue; }
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
            if (h) keep[t.id] = h;
        }
        for (std::unordered_map<uint32_t, uint32_t>::iterator it = colliders_.begin(); it != colliders_.end(); ++it)
            physics_->remove_body(it->second);
        colliders_.swap(keep);
    }

    // Logs and stumps for the renderer, posed as physics has them.
    std::vector<TreeGpuInstance> extra;
    extra.reserve(logs_.size() * 2);
    for (std::unordered_map<uint32_t, Log>::iterator it = logs_.begin(); it != logs_.end(); ++it) {
        Log& lg = it->second;
        bool active = false;
        physics_->body_pose(lg.body, lg.pos, lg.rot, active);
        TreeGpuInstance g = {};
        for (int k = 0; k < 3; ++k) g.pos[k] = lg.pos[k];
        for (int k = 0; k < 4; ++k) g.rot[k] = lg.rot[k];
        g.scale = lg.scale;
        g.model = lg.model;
        g.mode = 1;
        g.hit_time = -100.0f;
        g.id = lg.id;
        extra.push_back(g);
        TreeGpuInstance st = g;
        for (int k = 0; k < 3; ++k) st.pos[k] = lg.base[k];
        for (int k = 0; k < 4; ++k) st.rot[k] = lg.stand_rot[k];
        st.mode = 2;
        extra.push_back(st);
    }
    renderer_->set_tree_extras(extra);
}

int TreeFelling::chop(const float eye[3], const float dir[3], float time, uint32_t* id_out,
                      int* hits_out, int* need_out) {
    if (!renderer_ || !physics_ || !renderer_->trees().ready()) return 0;
    const TreeRenderer& tr = renderer_->trees();
    const std::vector<worldgen::TreeInstance>& st = renderer_->tree_field().standing();
    // Nearest trunk the ray meets within reach: a vertical cylinder per tree,
    // from the ground to where its crown begins.
    float best_t = kReach;
    const worldgen::TreeInstance* best = nullptr;
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
    if (!best) return 0;
    const worldgen::TreeInstance t = *best;   // standing() changes if it falls
    const TreeModel& m = tr.model(t.species, t.variant);
    const float s = t.height / tr.look(t.species).nominal_height;
    const int need = hits_needed(m.trunk_radius * s);
    const int got = ++hits_[t.id];
    renderer_->tree_hit(t.id, time);
    if (id_out) *id_out = t.id;
    if (hits_out) *hits_out = got;
    if (need_out) *need_out = need;
    if (got < need) return 1;

    hits_.erase(t.id);
    Log lg;
    lg.id = t.id;
    lg.model = t.species * kTreeVariants + (t.variant % kTreeVariants);
    lg.scale = s;
    lg.base[0] = t.x; lg.base[1] = t.y; lg.base[2] = t.z;
    tree_standing_rotation(t, lg.stand_rot);
    for (int k = 0; k < 3; ++k) lg.pos[k] = lg.base[k];
    for (int k = 0; k < 4; ++k) lg.rot[k] = lg.stand_rot[k];
    // It falls away from the axe.
    float away[3] = {dir[0], 0.0f, dir[2]};
    const float al = std::sqrt(away[0] * away[0] + away[2] * away[2]);
    if (al > 1e-4f) { away[0] /= al; away[2] /= al; } else { away[0] = 1.0f; }
    fell(lg, away, false);
    return 2;
}

void TreeFelling::fell(const Log& seed, const float away[3], bool asleep) {
    Log lg = seed;
    const TreeRenderer& tr = renderer_->trees();
    const uint32_t species = lg.model / kTreeVariants, variant = lg.model % kTreeVariants;
    const TreeModel& m = tr.model(species, variant);
    const float s = lg.scale;
    const float r = m.trunk_radius * s * 0.85f;

    // The tree stops standing: out of the field's list, its collider gone.
    renderer_->tree_field().set_felled(lg.id, true);
    std::unordered_map<uint32_t, uint32_t>::iterator c = colliders_.find(lg.id);
    if (c != colliders_.end()) { physics_->remove_body(c->second); colliders_.erase(c); }

    // The stump: a short static capsule whose top is the cut.
    {
        const float a[3] = {lg.base[0], lg.base[1] - 0.4f, lg.base[2]};
        const float b[3] = {lg.base[0], lg.base[1] + kCutHeight * s - r, lg.base[2]};
        lg.stump_body = physics_->add_static_capsule(a, b, r);
    }

    // The log. Its trunk capsule starts a radius above the cut so it rests
    // ON the stump rather than inside it, and runs to where the crown begins
    // or half the height, whichever is higher -- a fir's crown starts low.
    const float y0 = kCutHeight * s + r + 0.02f;
    const float y1 = std::fmax(m.crown_base * s, 0.5f * m.top * s);
    // Spin about the horizontal axis that tips the top away from the axe:
    // up x away. Slow -- gravity does the rest, which is what makes it read
    // as falling rather than as being thrown.
    const float spin[3] = {away[2] * 0.35f, 0.0f, -away[0] * 0.35f};
    lg.body = physics_->add_log(lg.pos, lg.rot, (std::fmax)(r, 0.04f), y0, (std::fmax)(y1, y0 + 0.5f),
                                m.crown_centre * s, m.crown_r * s * 0.85f, spin, asleep);
    logs_[lg.id] = lg;
}

bool TreeFelling::log_pose(uint32_t id, float pos[3], float rot[4], bool& active) const {
    std::unordered_map<uint32_t, Log>::const_iterator it = logs_.find(id);
    if (it == logs_.end()) return false;
    return physics_->body_pose(it->second.body, pos, rot, active);
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
    const uint32_t n = static_cast<uint32_t>(logs_.size());
    std::fwrite(&kMagic, 4, 1, f);
    std::fwrite(&kFormat, 4, 1, f);
    std::fwrite(&field_id, 4, 1, f);
    std::fwrite(&n, 4, 1, f);
    for (std::unordered_map<uint32_t, Log>::const_iterator it = logs_.begin(); it != logs_.end(); ++it) {
        const Log& l = it->second;
        std::fwrite(&l.id, 4, 1, f);
        std::fwrite(&l.model, 4, 1, f);
        std::fwrite(&l.scale, 4, 1, f);
        std::fwrite(l.base, 4, 3, f);
        std::fwrite(l.stand_rot, 4, 4, f);
        std::fwrite(l.pos, 4, 3, f);
        std::fwrite(l.rot, 4, 4, f);
    }
    std::fclose(f);
    return true;
}

bool TreeFelling::load(const char* path, uint32_t field_id, std::string& error, bool& mismatch) {
    mismatch = false;
    FILE* f = std::fopen(path, "rb");
    if (!f) { error = std::string("could not open ") + path; return false; }
    uint32_t magic = 0, fmt = 0, fid = 0, n = 0;
    if (std::fread(&magic, 4, 1, f) != 1 || std::fread(&fmt, 4, 1, f) != 1 ||
        std::fread(&fid, 4, 1, f) != 1 || std::fread(&n, 4, 1, f) != 1 ||
        magic != kMagic || fmt != kFormat) {
        std::fclose(f);
        error = std::string("not a tree save: ") + path;
        return false;
    }
    if (fid != field_id) {
        // A different world: its tree ids name other trees.
        std::fclose(f);
        mismatch = true;
        error = "tree save is for a different world";
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        Log l;
        if (std::fread(&l.id, 4, 1, f) != 1 || std::fread(&l.model, 4, 1, f) != 1 ||
            std::fread(&l.scale, 4, 1, f) != 1 || std::fread(l.base, 4, 3, f) != 3 ||
            std::fread(l.stand_rot, 4, 4, f) != 4 || std::fread(l.pos, 4, 3, f) != 3 ||
            std::fread(l.rot, 4, 4, f) != 4) {
            std::fclose(f);
            error = "truncated tree save";
            return false;
        }
        if (logs_.count(l.id)) continue;
        const float none[3] = {0.0f, 0.0f, 0.0f};
        // Asleep where it came to rest; the first touch wakes it.
        fell(l, none, true);
    }
    std::fclose(f);
    return true;
}

}  // namespace engine
