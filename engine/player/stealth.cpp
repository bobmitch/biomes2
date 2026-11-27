#include "engine/player/stealth.h"

namespace engine {
namespace player {

namespace {

const float kMoving = 0.25f;   // m/s: slower is standing (a body settling, not a step)

float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// The gaits' speeds, as the window drives them (main.cpp kWalkSpeed etc.).
const float kWalkV = 1.4f, kJogV = 5.0f, kSprintV = 8.0f;

// A value by speed through still -> walk -> jog -> sprint.
float by_speed(float v, float still, float walk, float jog, float sprint) {
    if (v < kMoving) return still;
    if (v <= kWalkV) return walk;
    if (v <= kJogV) return lerpf(walk, jog, (v - kWalkV) / (kJogV - kWalkV));
    return lerpf(jog, sprint, clamp01((v - kJogV) / (kSprintV - kJogV)));
}

}  // namespace

float Stealth::gait_level(float speed, bool crouching, bool swimming, bool wading) const {
    float l;
    if (swimming) {
        l = speed >= kMoving ? t_.swim_db : t_.swim_idle_db;
    } else if (speed < kMoving) {
        return perception::kSilent;
    } else if (crouching) {
        l = t_.crouch_db;
    } else {
        l = by_speed(speed, perception::kSilent, t_.walk_db, t_.jog_db, t_.sprint_db);
    }
    if (wading && !swimming) l += t_.wade_add_db;
    return l;
}

float Stealth::gait_visibility(float speed, bool crouching, bool swimming) const {
    if (swimming) return t_.vis_swim;
    if (crouching) return speed >= kMoving ? t_.vis_crouch_move : t_.vis_crouch_still;
    return by_speed(speed, t_.vis_still, t_.vis_walk, t_.vis_jog, t_.vis_sprint);
}

void Stealth::update(float dt, const StealthInput& in) {
    lux_ = in.lux;
    // Spikes decay first, then this frame's events raise them.
    if (spike_ > perception::kSilent) {
        spike_ -= t_.spike_decay_db_s * dt;
        if (spike_ < 0.0f) spike_ = perception::kSilent;
    }
    if (!in.body) {
        gait_ = perception::kSilent;
        level_ = spike_;
        vis_ = t_.vis_still;
        return;
    }
    auto raise = [&](float db) { if (db > spike_) spike_ = db; };
    if (in.jumped) raise(t_.jump_db);
    if (in.landed_m >= 0.0f) raise(t_.land_db + t_.land_db_per_m * in.landed_m);
    if (in.swung) raise(t_.axe_db);
    if (in.dug) raise(t_.dig_db);
    // Footsteps only on the ground (or in the water): in the air the body
    // makes no sound of its own, and the landing is a spike.
    const bool footing = in.grounded || in.swimming || in.wading;
    gait_ = footing ? gait_level(in.speed, in.crouching, in.swimming, in.wading) : perception::kSilent;
    level_ = gait_ > spike_ ? gait_ : spike_;
    vis_ = gait_visibility(in.speed, in.crouching, in.swimming);
}

}  // namespace player
}  // namespace engine
