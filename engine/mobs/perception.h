// ---------------------------------------------------------------------------
// How a creature perceives the player: the arithmetic of hearing and sight
// (docs/adr-007-creatures-perception-navigation.md, decision 3). Pure
// functions: no state, no GPU, no physics. The caller supplies the
// distances and the line-of-sight ray's verdict.
//
// HEARING. The player is a source of L dB at 1 m (engine/player/stealth.h).
// Inverse square: at d metres the level is L - 20 log10(max(d, 1)). A
// creature hears it within its hear_range when that level reaches its
// threshold (its acuity, lower is keener) -- so what it needs from the source
// rises with distance: at the edge of its range only a loud source carries,
// right beside it almost anything does.
//
// SIGHT. Within its cone (the TOTAL width) and within view_range x the
// player's visibility (posture, speed) x the light on the player, scaled by
// the creature's dark_gain (night vision) on a log scale.
// ---------------------------------------------------------------------------
#ifndef ENGINE_MOBS_PERCEPTION_H
#define ENGINE_MOBS_PERCEPTION_H

#include <cmath>

namespace engine {
namespace perception {

// No sound at all (standing still): below every threshold.
const float kSilent = -100.0f;

// The level, dB, at d metres from a source of `source_db` at 1 m.
inline float level_at(float source_db, float d) {
    if (source_db <= kSilent) return kSilent;
    return source_db - 20.0f * std::log10(d > 1.0f ? d : 1.0f);
}

// Heard: within the range and loud enough there. `ambient_db` masks
// (wind, rain -- later); 0 masks nothing a creature could hear.
inline bool hears(float source_db, float d, float range_m, float threshold_db, float ambient_db = 0.0f) {
    if (source_db <= kSilent || range_m <= 0.0f || d > range_m) return false;
    const float need = threshold_db > ambient_db ? threshold_db : ambient_db;
    return level_at(source_db, d) >= need;
}

// How far a source of `source_db` is heard (metres; 0 for silence).
inline float hear_distance(float source_db, float range_m, float threshold_db, float ambient_db = 0.0f) {
    if (source_db <= kSilent || range_m <= 0.0f) return 0.0f;
    const float need = threshold_db > ambient_db ? threshold_db : ambient_db;
    if (source_db < need) return 0.0f;
    const float d = std::pow(10.0f, (source_db - need) / 20.0f);
    return d < range_m ? d : range_m;
}

// Light on the player -> a share of a creature's sight, 0..1. Log scale
// between dark_lux and full_lux, floored: full light (an overcast day and
// brighter) is full acuity; moonlight (0.19 lx) 0.15; a cave the floor.
struct LightCurve {
    float dark_lux = 0.05f;
    float full_lux = 400.0f;
    float floor = 0.1f;
};

inline float light_factor(float lux, float dark_gain, const LightCurve& c) {
    const float e = lux * (dark_gain > 0.0f ? dark_gain : 0.0f);
    if (e <= c.dark_lux) return c.floor;
    if (e >= c.full_lux) return 1.0f;
    const float t = (std::log10(e) - std::log10(c.dark_lux)) / (std::log10(c.full_lux) - std::log10(c.dark_lux));
    return t > c.floor ? t : c.floor;
}

// How far a creature sees the player: its range x their visibility x light.
inline float sight_range(float view_range_m, float visibility, float light) {
    return view_range_m * visibility * light;
}

// `to` (target minus eye) within a cone `cone_deg` WIDE round `fwd` (unit).
// A cone of 360 or more sees everywhere; 0 or less, nothing.
inline bool in_cone(const float fwd[3], const float to[3], float cone_deg) {
    if (cone_deg >= 360.0f) return true;
    if (cone_deg <= 0.0f) return false;
    const float l = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (l < 1e-4f) return true;
    const float c = (fwd[0] * to[0] + fwd[1] * to[1] + fwd[2] * to[2]) / l;
    return c >= std::cos(cone_deg * 0.5f * 0.017453293f);
}

// Seen: in the cone, within the sight range, and the caller's ray clear.
inline bool sees(const float eye[3], const float fwd[3], const float target[3], float view_range_m,
                 float cone_deg, float visibility, float light, bool clear) {
    if (!clear) return false;
    const float to[3] = {target[0] - eye[0], target[1] - eye[1], target[2] - eye[2]};
    const float d = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    return d <= sight_range(view_range_m, visibility, light) && in_cone(fwd, to, cone_deg);
}

}  // namespace perception
}  // namespace engine

#endif
