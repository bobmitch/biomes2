// ---------------------------------------------------------------------------
// A capsule that walks on the streamed terrain.
//
// B4's exit criterion. Deliberately the smallest thing that can be checked
// rather than a character controller worth keeping: gravity, a ground query
// under the capsule, and a resolve. Jolt's CharacterVirtual replaces all of
// it, and the point of writing it out here is that the interesting part is
// what it stands on -- a height field assembled from GPU readbacks that arrive
// two frames late and come and go as chunks stream.
//
// The one thing it must get right is the case where the ground has not
// streamed in yet. Treating "no patch" as "no ground" drops the capsule
// through the world, and because it lands again as soon as a chunk arrives,
// the symptom is an occasional twitch rather than an obvious fall.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CHARACTER_H
#define ENGINE_CHARACTER_H

#include "engine/physics/height_field.h"

namespace engine {

struct Character {
    // Capsule centre.
    float x = 0.0f, y = 40.0f, z = 0.0f;
    float vy = 0.0f;
    float radius = 0.35f;
    float half_height = 0.80f;   // centre to the start of the cap
    float eye = 0.65f;           // eye above the capsule centre

    bool  grounded = false;
    bool  waiting_for_ground = false;   // nothing streamed in under us yet
    float ground_y = 0.0f;
    int   ground_lod = 0;

    // How far the capsule will reach down for ground it was already on. A
    // step-down budget, not a teleport: larger than a frame's worth of slope
    // at walking speed, smaller than anything that should read as a drop.
    static const int kSnapDownMm = 400;

    int   lod_changes = 0;   // how often the LOD under the feet switched

    float feet() const { return y - (half_height + radius); }

    void update(const HeightField& hf, float dt, float vx, float vz, bool jump) {
        if (dt > 0.05f) dt = 0.05f;   // a hitch must not launch the capsule
        const float kSnapDown = float(kSnapDownMm) * 0.001f;
        const bool was_grounded = grounded;
        const int  was_lod = ground_lod;

        x += vx * dt;
        z += vz * dt;

        vy -= 22.0f * dt;             // a little heavier than Earth; it reads better
        y += vy * dt;

        float g = 0.0f;
        int   lod = 0;
        if (!hf.sample(x, z, g, lod)) {
            // No terrain under us yet. Hold height rather than fall: the chunk
            // is coming, and falling through for a few frames then popping
            // back up is far harder to notice than it is to prevent.
            waiting_for_ground = true;
            grounded = false;
            if (vy < 0.0f) { y -= vy * dt; vy = 0.0f; }
            return;
        }
        waiting_for_ground = false;
        ground_y = g;
        ground_lod = lod;
        // A LOD switch under the feet moves the ground, because a coarser
        // height field is a different surface. Counted rather than smoothed:
        // how visible that is, is the question B3's LOD choice answers.
        if (was_grounded && lod != was_lod) ++lod_changes;

        const float rest = g + half_height + radius;
        if (y <= rest) {
            y = rest;
            if (vy < 0.0f) vy = 0.0f;
            grounded = true;
        } else if (was_grounded && vy <= 0.0f && (y - rest) <= kSnapDown) {
            // Snap down to ground we were already standing on.
            //
            // Without this the capsule is genuinely airborne over every crest:
            // walking at 6 m/s off a 45-degree slope, the ground falls away at
            // 6 m/s while gravity needs a quarter second to match it, so it
            // leaves the surface and lands again, repeatedly. Measured on the
            // scripted walk: 54% of frames off the ground. That is not wrong
            // physics for a point mass, it is the wrong model for walking, and
            // it is what every character controller's step-down exists to fix.
            y = rest;
            vy = 0.0f;
            grounded = true;
        } else {
            grounded = false;
        }

        if (jump && grounded) { vy = 7.5f; grounded = false; }
    }
};

}  // namespace engine

#endif  // ENGINE_CHARACTER_H
