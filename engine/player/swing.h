// ---------------------------------------------------------------------------
// The player's swing, in two parts (the creatures' side is engine/mobs/
// combat.h). A blow that lands the instant the button goes down cannot be
// read by anyone -- not by a creature stepping out of it, and not by the
// player, who sees the axe bite a quarter of a second after the tree has
// already lost a chop. So `begin` starts the swing and its animation, and
// the blow LANDS impact_s later, at the point in the clip where the head
// meets the wood. The caller does the striking then, from where the player
// is standing and looking THEN -- so a turn away mid-swing is a miss, as a
// step out of a creature's windup is. Then recover_s before the next.
//
// Per TOOL, from game/data/items.json: a heavy weapon lands late and
// recovers slowly, a knife lands at once. No GPU, no window: a headless
// test drives it as the window loop does (--combat-test).
//
// A click arriving while a swing is in flight would otherwise be thrown
// away, and chopping a tree is click after click: one press is REMEMBERED
// (`buffer`) if the swing has `swing_buffer_s` or less left to run, and
// `take_buffered` hands it back the moment the swing frees up -- the
// caller runs its own gates (the tool in hand, the stamina) on it then,
// since half a second has passed since the click. Never a queue: a second
// press inside the window replaces the first, so holding the button down
// cannot stack up swings.
//
// Nothing here is saved: a swing in flight is session state, like a
// creature's windup.
// ---------------------------------------------------------------------------
#ifndef ENGINE_PLAYER_SWING_H
#define ENGINE_PLAYER_SWING_H

#include "engine/player/items.h"

namespace engine {
namespace player {

class Swing {
public:
    // Start one. False if a swing is still in flight or recovering: the
    // caller should neither animate nor spend stamina on it.
    bool begin(const ToolStats& t) {
        if (active_) return false;
        tool_ = t;
        time_ = 0.0f;
        landed_ = false;
        held_ = false;
        active_ = true;
        return true;
    }

    // A frame. True EXACTLY once per swing: on the frame the blow lands.
    bool update(float dt) {
        if (!active_) return false;
        time_ += dt;
        bool land = false;
        if (!landed_ && time_ >= tool_.impact_s) {
            landed_ = true;
            land = true;
        }
        if (landed_ && time_ >= tool_.impact_s + tool_.recover_s) active_ = false;
        return land;
    }

    // A press that came too early: keep it if the swing is within
    // `window_s` of ending. True if it was kept.
    bool buffer(float window_s) {
        if (!active_) return false;
        if (left() > window_s) return false;
        held_ = true;
        return true;
    }
    // The kept press, once the swing is over. True once; the caller then
    // runs its gates and calls begin().
    bool take_buffered() {
        if (active_ || !held_) return false;
        held_ = false;
        return true;
    }
    bool buffered() const { return held_; }
    // Seconds this swing still has to run, 0 when idle.
    float left() const { return active_ ? (tool_.impact_s + tool_.recover_s) - time_ : 0.0f; }

    bool busy() const { return active_; }               // swinging or recovering
    bool before_impact() const { return active_ && !landed_; }
    // Seconds since the press, or -1 when idle. For a report.
    float time() const { return active_ ? time_ : -1.0f; }
    const ToolStats& tool() const { return tool_; }
    void clear() { active_ = false; landed_ = false; held_ = false; time_ = 0.0f; }

private:
    ToolStats tool_;
    float time_ = 0.0f;
    bool  active_ = false;
    bool  landed_ = false;
    bool  held_ = false;        // a press kept for when this swing ends
};

}  // namespace player
}  // namespace engine

#endif
