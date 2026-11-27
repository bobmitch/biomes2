// ---------------------------------------------------------------------------
// The player as something to hear and see (ADR-007, decision 3): every frame
// a SOURCE LEVEL (dB at 1 m) and a VISIBILITY (0..1), and the light on them.
// Creatures judge these through engine/mobs/perception.h.
//
// Sound is a continuous level by gait -- silent standing, a crouch walk,
// walk, jog and sprint by speed, swimming, wading louder -- and the max of
// decaying SPIKES for events: a jump, a landing (louder the further the
// fall), a swing of the axe, a dig. Visibility is posture and speed: crouched
// and still is hard to see, a sprinter easy. The numbers are items.json's
// `stealth` block.
//
// No GPU, no window, no physics: the window passes in what the body did and
// the light the renderer measured on it, so a headless test drives it.
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_STEALTH_H
#define ENGINE_PLAYER_STEALTH_H

#include "engine/mobs/perception.h"

namespace engine {
namespace player {

struct StealthTuning {
    float crouch_speed = 1.4f;         // m/s on foot crouched (no sprint)
    // Source levels, dB at 1 m. Gaits by speed: walk at 1.4 m/s, jog at 5,
    // sprint at 8, interpolated in dB; still is silent.
    float crouch_db = 35.0f, walk_db = 50.0f, jog_db = 60.0f, sprint_db = 70.0f;
    float swim_db = 55.0f, swim_idle_db = 40.0f;
    float wade_add_db = 4.0f;          // on the gait while wading
    // Event spikes, decaying.
    float jump_db = 62.0f;
    float land_db = 64.0f, land_db_per_m = 2.0f;
    float axe_db = 82.0f, dig_db = 78.0f;
    float spike_decay_db_s = 30.0f;
    // Visibility, 0..1.
    float vis_still = 0.70f, vis_walk = 0.85f, vis_jog = 0.95f, vis_sprint = 1.0f;
    float vis_crouch_still = 0.35f, vis_crouch_move = 0.50f, vis_swim = 0.50f;
    // Light on the player -> sight (perception::light_factor).
    perception::LightCurve light;
};

// What the body did this frame.
struct StealthInput {
    bool  body = false;          // a character exists (walking, not flying)
    float speed = 0.0f;          // horizontal m/s, measured
    bool  crouching = false;
    bool  swimming = false;
    bool  wading = false;
    bool  grounded = true;
    // Events this frame.
    bool  jumped = false;
    float landed_m = -1.0f;      // a landing after a fall of this many metres; < 0 none
    bool  swung = false;         // the axe
    bool  dug = false;
    float lux = 0.0f;            // illuminance on the player (Renderer::illuminance_at)
};

class Stealth {
public:
    void set_tuning(const StealthTuning& t) { t_ = t; }
    const StealthTuning& tuning() const { return t_; }

    void update(float dt, const StealthInput& in);

    // dB at 1 m, perception::kSilent for none.
    float level_db() const { return level_; }
    float gait_db() const { return gait_; }
    float visibility() const { return vis_; }
    float lux() const { return lux_; }
    // The light share an ordinary eye (dark_gain 1) gets on the player.
    float light() const { return perception::light_factor(lux_, 1.0f, t_.light); }
    // For the HUD: how far a creature of this range and acuity would hear,
    // and see, the player right now.
    float heard_at(float range_m, float threshold_db) const {
        return perception::hear_distance(level_, range_m, threshold_db);
    }
    float seen_at(float view_range_m, float dark_gain = 1.0f) const {
        return perception::sight_range(view_range_m, vis_, perception::light_factor(lux_, dark_gain, t_.light));
    }

    // The levels by gait, for a test or a table.
    float gait_level(float speed, bool crouching, bool swimming, bool wading) const;
    float gait_visibility(float speed, bool crouching, bool swimming) const;

private:
    StealthTuning t_;
    float spike_ = perception::kSilent;
    float gait_ = perception::kSilent;
    float level_ = perception::kSilent;
    float vis_ = 0.7f;
    float lux_ = 0.0f;
};

}  // namespace player
}  // namespace engine

#endif
