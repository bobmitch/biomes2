#include "engine/mobs/combat.h"

#include <cmath>

namespace engine {
namespace mobs {

void body_capsule(Size s, float& radius, float& height) {
    switch (s) {
        case Size::Small: radius = 0.3f; height = 0.8f; break;
        case Size::Large: radius = 0.6f; height = 2.4f; break;
        case Size::Human:
        default: radius = 0.4f; height = 1.8f; break;
    }
}

const Combat::Swing* Combat::find(uint32_t id) const {
    const auto it = swings_.find(id);
    return it == swings_.end() ? nullptr : &it->second;
}

bool Combat::winding_up(uint32_t id) const {
    const Swing* s = find(id);
    return s && s->windup > 0.0f;
}

bool Combat::busy(uint32_t id) const {
    const Swing* s = find(id);
    return s && (s->windup > 0.0f || s->recover > 0.0f);
}

float Combat::windup_left(uint32_t id) const {
    const Swing* s = find(id);
    return s && s->windup > 0.0f ? s->windup : 0.0f;
}

bool Combat::begin(const Mob& m) {
    if (!table_ || m.kind < 0 || size_t(m.kind) >= table_->kinds.size()) return false;
    const CreatureKind::Attack& a = table_->kinds[size_t(m.kind)].attack;
    if (a.damage <= 0.0f) return false;
    if (busy(m.id)) return false;
    Swing& s = swings_[m.id];
    // A windup of zero would land in this same frame's update, which is
    // fine: the player simply has no telegraph.
    s.windup = a.windup_s > 0.0f ? a.windup_s : 1e-6f;
    s.recover = 0.0f;
    started_.push_back(m.id);
    return true;
}

void Combat::update(float dt, const std::vector<Mob>& live, const Target& p, const HitFn& hit) {
    if (!table_) return;
    for (const Mob& m : live) {
        const auto it = swings_.find(m.id);
        if (it == swings_.end()) continue;
        Swing& s = it->second;
        if (s.windup > 0.0f) {
            s.windup -= dt;
            if (s.windup > 0.0f) continue;
            // The blow lands. Still in reach, and still faced?
            const CreatureKind& K = table_->kinds[size_t(m.kind)];
            s.windup = 0.0f;
            s.recover = K.attack.recover_s;
            bool lands = false;
            if (p.present) {
                // Chest to chest: its own is half its body's height.
                float br = 0.4f, bh = 1.8f;
                body_capsule(K.size, br, bh);
                const float to[3] = {p.feet[0] - m.pos[0],
                                     (p.feet[1] + p.chest) - (m.pos[1] + bh * 0.5f),
                                     p.feet[2] - m.pos[2]};
                const float d = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
                const float h = std::sqrt(to[0] * to[0] + to[2] * to[2]);
                lands = d <= K.attack.reach_m;
                if (lands && h > 1e-4f) {
                    const float fwd[3] = {std::sin(m.yaw), 0.0f, std::cos(m.yaw)};
                    const float dot = (fwd[0] * to[0] + fwd[2] * to[2]) / h;
                    lands = dot >= std::cos(K.attack.arc_deg * 0.5f * 0.017453293f);
                }
            }
            if (lands) {
                ++landed_;
                Blow b;
                b.id = m.id;
                b.kind = m.kind;
                b.damage = K.attack.damage;
                b.stamina = K.attack.stamina;
                for (int k = 0; k < 3; ++k) b.from[k] = m.pos[k];
                if (hit) hit(b);
            } else {
                ++missed_;
            }
        } else if (s.recover > 0.0f) {
            s.recover -= dt;
            if (s.recover < 0.0f) s.recover = 0.0f;
        }
    }
    // Forget what is no longer live (killed, or frozen with its zone).
    if (swings_.empty()) return;
    std::unordered_map<uint32_t, bool> alive;
    for (const Mob& m : live) alive[m.id] = true;
    for (auto it = swings_.begin(); it != swings_.end();)
        it = alive.count(it->first) ? std::next(it) : swings_.erase(it);
}

uint32_t Combat::strike(std::vector<Mob>& live, const float eye[3], const float dir[3], float reach,
                        float damage, bool& died, float point[3]) {
    died = false;
    if (!table_ || damage <= 0.0f) return 0;
    uint32_t best = 0;
    float best_t = reach;
    for (const Mob& m : live) {
        if (m.kind < 0 || size_t(m.kind) >= table_->kinds.size()) continue;
        float r = 0.4f, h = 1.8f;
        body_capsule(table_->kinds[size_t(m.kind)].size, r, h);
        // The ray against a vertical segment (the capsule's axis) swollen by
        // r: the nearest point of approach, clamped to the segment.
        const float a[3] = {m.pos[0], m.pos[1] + r, m.pos[2]};
        const float len = h - 2.0f * r > 0.0f ? h - 2.0f * r : 0.0f;
        // Walk the ray; a closed form for ray-vs-capsule is not worth it at
        // this scale, and the march is exact enough at 2 cm over 3 m.
        const float step = 0.02f;
        for (float t = 0.0f; t <= best_t; t += step) {
            const float p[3] = {eye[0] + dir[0] * t, eye[1] + dir[1] * t, eye[2] + dir[2] * t};
            float y = p[1] - a[1];
            if (y < 0.0f) y = 0.0f;
            if (y > len) y = len;
            const float q[3] = {a[0], a[1] + y, a[2]};
            const float d[3] = {p[0] - q[0], p[1] - q[1], p[2] - q[2]};
            if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= r * r) {
                best_t = t;
                best = m.id;
                if (point) {
                    for (int k = 0; k < 3; ++k) point[k] = p[k];
                }
                break;
            }
        }
    }
    if (!best) return 0;
    for (Mob& m : live) {
        if (m.id != best) continue;
        m.health -= damage;
        if (m.health <= 0.0f) {
            m.health = 0.0f;
            died = true;
        }
        break;
    }
    return best;
}

std::vector<uint32_t> Combat::take_started() {
    std::vector<uint32_t> out;
    out.swap(started_);
    return out;
}

void Combat::clear() {
    swings_.clear();
    started_.clear();
    landed_ = missed_ = 0;
}

}  // namespace mobs
}  // namespace engine
