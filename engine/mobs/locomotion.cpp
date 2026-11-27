#include "engine/mobs/locomotion.h"

#include "engine/nav/nav_world.h"
#include "engine/physics/physics_world.h"

#include <algorithm>
#include <cmath>

namespace engine {
namespace mobs {

namespace {

float hdist(const float a[3], const float b[3]) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
float u01(uint64_t h) { return float(h >> 40) / float(1ull << 24); }

}  // namespace

unsigned short Locomotion::include_for(int kind) const {
    using namespace nav;
    if (!table_ || kind < 0 || size_t(kind) >= table_->kinds.size()) return kFlagWalk | kFlagWade;
    switch (table_->kinds[size_t(kind)].move) {
        case Move::Amphibious: return kFlagWalk | kFlagWade | kFlagSwim;
        case Move::WaterSurface: return kFlagSwim;
        default: return kFlagWalk | kFlagWade;
    }
}

const Locomotion::Body* Locomotion::body(uint32_t id) const {
    const auto it = bodies_.find(id);
    return it == bodies_.end() ? nullptr : &it->second;
}

size_t Locomotion::near_count() const {
    size_t n = 0;
    for (const auto& b : bodies_) n += b.second.close ? 1 : 0;
    return n;
}

void Locomotion::clear() {
    for (auto& b : bodies_)
        if (b.second.walker && physics_) physics_->remove_walker(b.second.walker);
    bodies_.clear();
}

void Locomotion::update(float dt, std::vector<Mob>& live, const float player[3]) {
    if (!table_) return;
    // Bodies of creatures no longer live (frozen, gone) go.
    std::unordered_map<uint32_t, bool> alive;
    for (const Mob& m : live) alive[m.id] = true;
    for (auto it = bodies_.begin(); it != bodies_.end();) {
        if (alive.count(it->first)) { ++it; continue; }
        if (it->second.walker && physics_) physics_->remove_walker(it->second.walker);
        it = bodies_.erase(it);
    }
    if (dt <= 0.0f) return;

    for (size_t i = 0; i < live.size(); ++i) {
        Mob& m = live[i];
        Body& b = bodies_[m.id];
        const CreatureKind& K = table_->kinds[size_t(m.kind)];
        const unsigned short include = include_for(m.kind);

        // --- tier: physics near the player, the mesh further out
        const float dp = hdist(m.pos, player);
        const bool want_near = physics_ && (b.close ? dp < tuning_.near_out : dp < tuning_.near_in);
        if (want_near && !b.close) {
            b.walker = physics_->add_walker(m.pos[0], m.pos[1] + 0.05f, m.pos[2]);
            b.close = b.walker != 0;
        } else if (!want_near && b.close) {
            physics_->remove_walker(b.walker);
            b.walker = 0;
            b.close = false;
        }

        // --- steering: along the path to the goal
        float want[2] = {0.0f, 0.0f};
        if (m.gait > 0 && nav_) {
            const float gdx = m.goal[0] - b.path_goal[0], gdz = m.goal[2] - b.path_goal[2];
            b.repath -= dt;
            if (b.corners.empty() || gdx * gdx + gdz * gdz > 0.25f || b.repath <= 0.0f) {
                float len = 0.0f;
                b.path_complete = false;
                nav_->path(m.pos, m.goal, include, b.corners, &len, &b.path_complete);
                b.next = b.corners.size() >= 6 ? 1 : 0;
                for (int k = 0; k < 3; ++k) b.path_goal[k] = m.goal[k];
                b.repath = b.close ? tuning_.repath_near : tuning_.repath_mid;
            }
            const size_t n = b.corners.size() / 3;
            while (b.next < n) {
                const bool last = b.next + 1 == n;
                if (hdist(m.pos, &b.corners[b.next * 3]) > (last ? tuning_.arrive : tuning_.corner)) break;
                ++b.next;
            }
            if (n == 0 || b.next >= n) {
                // There, or as near as the mesh goes (or nowhere to go): stand.
                m.gait = 0;
                b.corners.clear();
            } else {
                const float* c = &b.corners[b.next * 3];
                float dx = c[0] - m.pos[0], dz = c[2] - m.pos[2];
                const float l = std::sqrt(dx * dx + dz * dz);
                if (l > 1e-4f) { dx /= l; dz /= l; }
                float spd = m.gait >= 2 ? K.run_speed : K.walk_speed;
                // Ease into the goal, or a body overshoots and circles it.
                if (b.next + 1 == n) spd *= (std::min)(1.0f, (std::max)(0.35f, l / 1.5f));
                want[0] = dx * spd;
                want[1] = dz * spd;
            }
        }
        // Apart from the others (the near ones' bodies collide as well).
        for (size_t j = 0; j < live.size(); ++j) {
            if (j == i) continue;
            const float dx = m.pos[0] - live[j].pos[0], dz = m.pos[2] - live[j].pos[2];
            const float d2 = dx * dx + dz * dz;
            const float s = tuning_.separation;
            if (d2 < s * s && d2 > 1e-6f) {
                const float d = std::sqrt(d2), push = (s - d) / s * 1.5f;
                want[0] += dx / d * push;
                want[1] += dz / d * push;
            }
        }

        // --- moving
        const float before[3] = {m.pos[0], m.pos[1], m.pos[2]};
        if (b.close) {
            physics_->step_walker(b.walker, dt, want[0], want[1], false, 0.0f);
            float p[3];
            if (physics_->walker_position(b.walker, p)) {
                m.pos[0] = p[0];
                m.pos[1] = p[1];
                m.pos[2] = p[2];
            }
            b.grounded = physics_->walker_supported(b.walker);
            b.swimming = physics_->walker_medium(b.walker) == Medium::Swimming;
            // Fell through the world (no terrain body under it yet): back
            // onto the mesh.
            if (nav_) {
                float q[3];
                if (nav_->move_along(m.pos, m.pos, include, q) && m.pos[1] < q[1] - 4.0f) {
                    physics_->place_walker(b.walker, q[0], q[1] + 0.1f, q[2]);
                    m.pos[0] = q[0]; m.pos[1] = q[1]; m.pos[2] = q[2];
                }
            }
        } else if (nav_ && (want[0] != 0.0f || want[1] != 0.0f)) {
            const float sw = b.swimming ? tuning_.swim_speed : 1.0f;
            const float to[3] = {m.pos[0] + want[0] * sw * dt, m.pos[1], m.pos[2] + want[1] * sw * dt};
            float q[3];
            unsigned char area = 0;
            if (nav_->move_along(m.pos, to, include, q, &area)) {
                b.swimming = area == nav::kAreaSwim;
                m.pos[0] = q[0];
                m.pos[2] = q[2];
                m.pos[1] = b.swimming ? sea_ - tuning_.float_depth : q[1];
            }
            b.grounded = !b.swimming;
        }

        // --- what it did
        const float moved = hdist(m.pos, before);
        const float v = moved / dt;
        b.speed += (v - b.speed) * (1.0f - std::exp(-12.0f * dt));
        b.vertical = (m.pos[1] - before[1]) / dt;
        const bool asked = want[0] * want[0] + want[1] * want[1] > 0.04f;
        b.stuck = asked && v < 0.1f ? b.stuck + dt : 0.0f;
        if (b.stuck > 3.0f) {
            // Wedged: give up on this goal; the behaviour picks another.
            m.gait = 0;
            b.corners.clear();
            b.stuck = 0.0f;
        }
        // Face where it is going; standing, where it was told to look.
        // Only a creature asked to move turns to its travel: a body settling
        // on a slope slid fast enough to swing a standing mimic 8 degrees.
        const bool travelling = asked && v > 0.2f;
        if (travelling || (b.facing && m.gait == 0)) {
            const float want_yaw = travelling ? std::atan2(m.pos[0] - before[0], m.pos[2] - before[2]) : b.face_yaw;
            if (travelling) b.facing = false;
            float d = want_yaw - m.yaw;
            d -= 6.2831853f * std::floor((d + 3.14159265f) / 6.2831853f);
            const float step = tuning_.turn_rate * dt;
            m.yaw += d > step ? step : (d < -step ? -step : d);
        }
        m.state_t += dt;   // the behaviour's clock: time in its state
        b.idle = m.gait == 0 ? b.idle + dt : 0.0f;
    }
}

void Locomotion::face(const Mob& m, float x, float z) {
    Body& b = bodies_[m.id];
    const float dx = x - m.pos[0], dz = z - m.pos[2];
    if (dx * dx + dz * dz < 1e-4f) return;
    b.face_yaw = std::atan2(dx, dz);
    b.facing = true;
}

void Locomotion::wander(float dt, std::vector<Mob>& live, double now) {
    (void)dt;
    if (!nav_) return;
    for (Mob& m : live) {
        if (m.gait != 0) continue;
        if (m.kind >= 0 && size_t(m.kind) < skip_wander.size() && skip_wander[size_t(m.kind)]) continue;
        const float wait = 5.0f + 10.0f * u01(mix(m.id));
        Body& b = bodies_[m.id];
        if (b.idle < wait) continue;
        const uint64_t h = mix(uint64_t(m.id) * 0x9E37u ^ uint64_t(now));
        const float a = u01(h) * 6.2831853f, r = 3.0f + 12.0f * u01(mix(h));
        const float p[3] = {m.home[0] + std::sin(a) * r, m.home[1], m.home[2] + std::cos(a) * r};
        const float ext[3] = {2.0f, 6.0f, 2.0f};
        float q[3];
        b.idle = 0.0f;
        if (!nav_->nearest(p, ext, include_for(m.kind) & nav::kFlagWalk, q)) continue;
        m.goal[0] = q[0];
        m.goal[1] = q[1];
        m.goal[2] = q[2];
        m.gait = 1;
    }
}

}  // namespace mobs
}  // namespace engine
