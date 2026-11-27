// ---------------------------------------------------------------------------
// Felling: chopping trees down, and what is left afterwards.
//
// Standing trees within kColliderRadius of the player get a static trunk
// capsule, so you walk round them rather than through. A chop is a ray from
// the eye; enough hits on one trunk (more for a thicker one) fells it. Felling
// marks the tree's layer-8 id as felled -- it stops standing -- and replaces
// it with two things: a stump, static, drawn from the bottom of the same mesh,
// and a log, a Jolt rigid body drawn from the top of it, given a nudge away
// from the axe and left to gravity.
//
// Both persist. A save holds each felled id with its log's pose, anchored to
// the same field id the dig edits are, so a log comes back where it came to
// rest and the tree it was never stands again. Logs are destructible for
// materials later; this is the hook, not that system.
// ---------------------------------------------------------------------------
#ifndef ENGINE_FELLING_H
#define ENGINE_FELLING_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class Renderer;
class PhysicsWorld;

class TreeFelling {
public:
    static constexpr float kReach = 3.2f;            // metres from the eye
    static constexpr float kColliderRadius = 40.0f;  // trunks near the player

    void init(Renderer* r, PhysicsWorld* p) { renderer_ = r; physics_ = p; }

    // Keep trunk colliders round (x, z), read every log's pose from physics,
    // and hand logs and stumps to the renderer. Every frame.
    void update(float x, float z);

    // One swing from `eye` along `dir`. 0 nothing in reach, 1 a hit, 2 that
    // hit felled it. `id_out` names the tree; `hits_out` and `need_out` say
    // how far along it is.
    int chop(const float eye[3], const float dir[3], float time, uint32_t* id_out = nullptr,
             int* hits_out = nullptr, int* need_out = nullptr);

    // Persistence, next to the dig edits and with the same field id.
    bool save(const char* path, uint32_t field_id, std::string& error) const;
    bool load(const char* path, uint32_t field_id, std::string& error, bool& mismatch);

    // Diagnostics.
    size_t colliders() const { return colliders_.size(); }
    size_t felled() const { return logs_.size(); }
    bool log_pose(uint32_t id, float pos[3], float rot[4], bool& active) const;
    // Every felled tree as solid shapes, for the navmesh (ADR-007): its stump,
    // a short vertical cylinder, and its log, a capsule where physics has it
    // now, with whether it has come to rest.
    struct Shape {
        uint32_t id = 0;
        bool capsule = false;
        float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        float r = 0.0f;
        bool resting = true;
    };
    void shapes(std::vector<Shape>& out) const;
    // Base, height and hits a tree needs, for a test that wants one to fell.
    bool tree_info(uint32_t id, float base[3], float& height, float& trunk_r, int& hits) const;

private:
    struct Log {
        uint32_t id = 0;
        uint32_t model = 0;
        float    scale = 1.0f;
        float    base[3] = {0, 0, 0};
        float    stand_rot[4] = {0, 0, 0, 1};
        float    pos[3] = {0, 0, 0};
        float    rot[4] = {0, 0, 0, 1};
        uint32_t body = 0;
        uint32_t stump_body = 0;
        float    r = 0.0f, y0 = 0.0f, y1 = 0.0f;   // the log's capsule along its local y
        float    stump_top = 0.0f;                 // over the base
    };
    void fell(const Log& seed, const float away[3], bool asleep);
    int  hits_needed(float trunk_r) const;

    Renderer*     renderer_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    std::unordered_map<uint32_t, uint32_t> colliders_;   // tree id -> body
    std::unordered_map<uint32_t, float>    collider_y_;  // the base each was built at
    std::unordered_map<uint32_t, int>      hits_;
    std::unordered_map<uint32_t, Log>      logs_;
    unsigned long long field_version_ = 0;
    float last_x_ = 1e30f, last_z_ = 1e30f;
};

}  // namespace engine

#endif  // ENGINE_FELLING_H
