// ---------------------------------------------------------------------------
// Felling: chopping trees down, and what is left afterwards.
//
// Standing trees within kColliderRadius of the player get a static trunk
// capsule, so you walk round them rather than through. A chop is a ray from
// the eye; enough damage on one trunk (more for a thicker one) fells it.
// Felling marks the tree's layer-8 id as felled -- it stops standing -- and
// replaces it with a stump, static, drawn from the bottom of the same mesh,
// and a LOG, a Jolt rigid body drawn from the top of it, given a nudge away
// from the axe and left to gravity.
//
// What falls is a tree; what stays is a trunk. A tree the player cuts goes
// over on a HINGE at its stump, down its own lean, slowly at first and
// faster as it goes -- a free body turns about its centre of mass halfway
// up and throws its butt out sideways, which read as a kick. One knocked
// down by another tree is thrown instead, because a one-axis hinge will not
// go against a lean. Either way it keeps its crown all the way down -- that
// is what makes the fall read, and it is the tree's whole length that
// sweeps whatever it lands on -- and sheds it against the FIRST THING ITS
// CROWN MEETS, which is what branches do: the body is swapped for the
// bole's shape alone, and the crown becomes DEBRIS, a few clumps of
// branches and leaves that spill off the top, settle, lie there and fade
// out. Nothing permanent in the world is ever more than a trunk.
//
// While it is a whole tree it is two capsules, not one, and that is about
// MASS rather than shape: the bole is solid wood, and everything above it
// -- branches, twigs and leaves, mostly air -- carries
// `logs.crown_mass_share` of the body and no more.
//
// A trunk is firewood in waiting. Every piece of one carries health in the
// same currency the axe does (items.json `logs`): chop a whole log through
// and it breaks in two at `split_at`; chop a half through and it comes apart
// into `billets`, short rounds that E picks up as wood. A piece is drawn as a
// BAND of the species' own mesh (kTreePiece), so no new geometry exists
// anywhere -- one tree mesh, cut by its own heights.
//
// And a tree that falls on another hurts it, by the energy it carried where
// it struck: enough and the second tree comes down too, away from the blow,
// carrying its own momentum into whatever it lands on.
//
// Pieces and stumps persist. A save holds each of them, anchored to the same
// field id the dig edits are, so a log comes back where it came to rest and
// the tree it was never stands again. Debris is not saved: it is a moment,
// not a thing.
// ---------------------------------------------------------------------------
#ifndef ENGINE_FELLING_H
#define ENGINE_FELLING_H

#include "engine/player/items.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace engine {

class Renderer;
class PhysicsWorld;

class TreeFelling {
public:
    static constexpr float kReach = 3.2f;            // metres from the eye
    static constexpr float kColliderRadius = 40.0f;  // trunks near the player
    // A clump of crown is done falling when it has been under kDebrisSlow for
    // kDebrisSlowFor, or after kDebrisAir whatever it is doing. Its body then
    // goes and it lies where it is: a light sphere on a slope rolls for ever,
    // and while one is rolling it keeps knocking the log awake.
    static constexpr float kDebrisSlow = 1.2f;       // m/s
    static constexpr float kDebrisSlowFor = 0.3f;
    static constexpr float kDebrisAir = 5.0f;
    // ... and a log has arrived once it has been under kRestSpeed for
    // kRestFor. Same reason: a bare trunk is a capsule, and it would creep
    // down any slope for ever.
    static constexpr float kRestSpeed = 0.35f;
    static constexpr float kRestFor = 0.5f;

    // What one swing did.
    enum ChopResult {
        kChopNothing = 0,   // nothing in reach
        kChopHit,           // a tree or a log took it and held
        kChopFelled,        // a standing tree came down
        kChopSplit,         // a whole log broke in two
        kChopBilleted       // half a log came apart into firewood
    };
    struct Blow {
        int      result = kChopNothing;
        uint32_t tree = 0;        // the layer-8 tree it belongs to
        bool     piece = false;   // a felled log, not a standing tree
        int      stage = 0;       // of the piece: 0 the whole bole, 1 a half
        int      hits = 0;        // chops in, and ...
        int      need = 0;        // ... chops the whole thing takes
        int      wood = 0;        // wood in the billets kChopBilleted made
    };

    void init(Renderer* r, PhysicsWorld* p) { renderer_ = r; physics_ = p; }
    // items.json's `logs` block. Without it LogTuning's own defaults stand.
    void set_tuning(const player::LogTuning& t) { tune_ = t; }
    const player::LogTuning& tuning() const { return tune_; }

    // Keep trunk colliders round (x, z), read every piece's pose from
    // physics, shed the crown of a log that has tipped, age the debris, let
    // a falling trunk hurt what it lands on, and hand the lot to the
    // renderer. Every frame; `dt` ages the debris and nothing else.
    void update(float x, float z, float dt = 0.0f);

    // One swing from `eye` along `dir`, taking `damage` health off whatever
    // it meets -- items.json's tool.damage, of which the reference axe's
    // `logs.chop_damage` is one chop of a standing tree.
    int chop(const float eye[3], const float dir[3], float time, float damage, Blow* out = nullptr);

    // E: the nearest round of firewood the ray meets within `reach`.
    struct Pick { uint64_t key = 0; int wood = 0; float dist = 0.0f; };
    bool aim_wood(const float eye[3], const float dir[3], float reach, Pick& out) const;
    // Take it: the wood it was worth, 0 if it has gone.
    int take_wood(uint64_t key);

    // Persistence, next to the dig edits and with the same field id.
    bool save(const char* path, uint32_t field_id, std::string& error) const;
    bool load(const char* path, uint32_t field_id, std::string& error, bool& mismatch);

    // Diagnostics.
    size_t colliders() const { return colliders_.size(); }
    size_t felled() const { return stumps_.size(); }
    size_t pieces() const { return pieces_.size(); }
    size_t debris() const { return debris_.size(); }
    bool log_pose(uint32_t id, float pos[3], float rot[4], bool& active) const;
    // That tree's oldest surviving piece: what stage it is and its health.
    bool piece_state(uint32_t id, int& stage, float& health, float& max_health) const;
    // ... and how fast it is going, which is what "still moving" means.
    bool piece_motion(uint32_t id, float& lin, float& ang) const;
    // Damage a STANDING tree has taken, and the health it has.
    bool tree_damage(uint32_t id, float& taken, float& health) const;

    // A falling trunk landing on something, since the list was cleared: a
    // standing tree, or a log already on the ground. ONE PER PAIR, added to
    // as the blow bears down -- `damage` is what it has cost in all, and
    // `speed`, `mass` and `energy` are the hardest moment of it. Frame by
    // frame it is thirty entries of a health apiece, which is neither what
    // happened nor readable.
    struct Impact {
        uint32_t faller = 0;
        uint64_t struck = 0;       // a tree id, or a piece key if `piece`
        bool     piece = false;    // what it landed on was a felled log
        int      stage = 0;        // ... and which stage of one
        float    speed = 0.0f;     // the CLOSING speed where it struck, m/s
        float    mass = 0.0f;      // the mass the blow met there (not the tree's)
        float    energy = 0.0f;    // joules it carried there
        float    damage = 0.0f;    // health it took off
        bool     felled = false;   // enough to bring the tree down ...
        bool     broke = false;    // ... or to break the log it landed on
    };
    const std::vector<Impact>& impacts() const { return impacts_; }
    void clear_impacts() { impacts_.clear(); }

    // Where the crown's clumps ended up, measured as each one stops: how far
    // it travelled from where it came off the tree, and how long it took.
    // A clump that rolls away down the hill is the thing this watches.
    struct DebrisStats {
        int   stopped = 0;
        float travel_sum = 0.0f, travel_max = 0.0f, stop_sum = 0.0f;
        float mean_travel() const { return stopped ? travel_sum / float(stopped) : 0.0f; }
        float mean_stop_s() const { return stopped ? stop_sum / float(stopped) : 0.0f; }
    };
    const DebrisStats& debris_stats() const { return debris_stats_; }
    void clear_debris_stats() { debris_stats_ = DebrisStats(); }

    // Every felled tree as solid shapes, for the navmesh (ADR-007): its
    // stump, a short vertical cylinder, and every piece of its trunk as a
    // capsule where physics has it now, with whether it has come to rest.
    // Debris is left out: it is gone in seconds and not worth a rebuild.
    struct Shape {
        uint64_t id = 0;
        bool capsule = false;
        float a[3] = {0, 0, 0}, b[3] = {0, 0, 0};
        float r = 0.0f;
        bool resting = true;
    };
    void shapes(std::vector<Shape>& out) const;
    // Base, height, trunk radius and the chops a tree needs, for a test that
    // wants one to fell.
    bool tree_info(uint32_t id, float base[3], float& height, float& trunk_r, int& hits) const;
    // Fell a standing tree outright, toward `dir` (horizontal). For the
    // chain test, which needs a tree to go a particular way.
    bool fell_toward(uint32_t id, const float dir[3]);

private:
    // One section of a felled trunk: the band [lo, hi] of the species' mesh,
    // in model metres, as a dynamic capsule. Stage 0 is the whole bole, 1 a
    // half of it, 2 a round of firewood.
    struct Piece {
        uint64_t key = 0;
        uint32_t tree = 0;
        uint32_t model = 0;
        float    scale = 1.0f;
        int      stage = 0;
        bool     crowned = false;       // still carrying the crown, mid-fall
        float    lo = 0.0f, hi = 0.0f;  // model-local y
        float    ax_lo[2] = {0, 0}, ax_hi[2] = {0, 0};   // the trunk's axis there
        float    cap_lo = 0.0f, cap_hi = 0.0f;           // the end discs' scale
        float    r = 0.0f;              // capsule radius, world metres
        float    mass = 0.0f;           // kg of wood in it
        float    health = 0.0f, max_health = 0.0f;
        int      wood = 0;              // stage 2: what E gets for it
        float    pos[3] = {0, 0, 0};
        float    rot[4] = {0, 0, 0, 1};
        float    hit_time = -100.0f;
        float    age = 0.0f;
        float    slow_s = 0.0f;    // how long it has been creeping rather than moving
        uint32_t body = 0;
        uint32_t hinge = 0;        // the stump it is still pivoting on, while it falls
        // What each thing it has met has already taken from it, as the
        // ENERGY of the hardest blow so far. A pair is not done after one
        // touch: a tree that brushes a neighbour at twenty degrees goes on
        // bearing down on it, and what snaps the trunk is the peak of that,
        // not the first contact. Only the INCREASE counts, so a log lying
        // against a trunk -- whose energy never rises -- cannot saw it down.
        // Tree ids and piece keys share the map; piece keys start above
        // every tree id, so the two can never collide.
        std::unordered_map<uint64_t, float> struck;
    };
    // The crown, coming apart: a band of the mesh on a light sphere, which
    // falls, settles, lies there and fades. Never saved.
    struct Debris {
        uint32_t tree = 0, model = 0;
        float    scale = 1.0f;
        float    lo = 0.0f, hi = 0.0f;
        float    pos[3] = {0, 0, 0};
        float    rot[4] = {0, 0, 0, 1};
        float    age = 0.0f;       // since it came off the tree
        float    slow_s = 0.0f;    // ... and how long it has been barely moving
        float    still_s = 0.0f;   // ... and how long it has lain where it is
        float    fade = 1.0f;
        float    spawn[3] = {0, 0, 0};   // where it came off the tree
        uint32_t body = 0;         // 0 once it has come to rest: see update()
    };
    struct Stump {
        uint32_t id = 0, model = 0;
        float    scale = 1.0f;
        float    base[3] = {0, 0, 0};
        float    rot[4] = {0, 0, 0, 1};
        float    r = 0.0f, top = 0.0f;
        uint32_t body = 0;
    };

    // `knocked`: hit by another falling tree rather than cut. A cut tree
    // goes over slowly on a hinge at its stump, down its own lean; a
    // knocked one is being shoved sideways, so it gets a free body and a
    // real kick -- which is what being hit by three tonnes of oak is.
    void   fell(uint32_t id, uint32_t model, float scale, const float base[3],
                const float stand_rot[4], const float away[3], bool knocked);
    bool   crown_touched(const Piece& p) const;
    void   shed_crown(Piece& p);
    void   release_hinge(Piece& p);
    void   split(Piece& p, float time);
    int    billet(Piece& p, float time);
    void   impacts_frame();
    Piece* make_piece(const Piece& from, float lo, float hi, int stage, const float pos[3],
                      const float rot[4], const float vel[3], const float spin[3], bool asleep);
    void   piece_geometry(Piece& p) const;
    int    hits_needed(float trunk_r) const;
    float  tree_health(float trunk_r) const { return float(hits_needed(trunk_r)) * tune_.chop_damage; }

    Renderer*     renderer_ = nullptr;
    PhysicsWorld* physics_ = nullptr;
    player::LogTuning tune_;
    std::unordered_map<uint32_t, uint32_t> colliders_;   // tree id -> body
    std::unordered_map<uint32_t, float>    collider_y_;  // the base each was built at
    std::unordered_map<uint32_t, float>    damage_;      // health off a standing tree
    std::unordered_map<uint64_t, Piece>    pieces_;
    std::unordered_map<uint32_t, Stump>    stumps_;
    std::vector<Debris>                    debris_;
    DebrisStats                            debris_stats_;
    std::vector<Impact>                    impacts_;
    // Above every tree id, so a piece's Shape key can never be a stump's.
    uint64_t next_piece_ = 0x100000000ull;
    unsigned long long field_version_ = 0;
    float last_x_ = 1e30f, last_z_ = 1e30f;
};

}  // namespace engine

#endif  // ENGINE_FELLING_H
