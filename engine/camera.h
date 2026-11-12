// ---------------------------------------------------------------------------
// Free flight. B3's exit criterion is holding frame rate while moving through
// a streamed world, and a fixed orbit cannot test streaming at all -- nothing
// enters or leaves residency on a closed path around the origin.
//
// Yaw and pitch rather than a free quaternion: an unconstrained roll makes it
// impossible to tell a streaming artefact from being upside down, and nothing
// in this project needs to fly inverted.
// ---------------------------------------------------------------------------
#ifndef ENGINE_CAMERA_H
#define ENGINE_CAMERA_H

#include <windows.h>
#include <cmath>

namespace engine {

struct Camera {
    float x = 0.0f, y = 40.0f, z = -60.0f;
    float yaw = 0.0f;      // radians, 0 looks down +z
    float pitch = -0.35f;  // radians, negative looks down
    float speed = 40.0f;   // metres per second

    void look(float dyaw, float dpitch) {
        yaw += dyaw;
        pitch += dpitch;
        // Stop just short of vertical. At exactly +/-90 degrees the forward
        // vector is parallel to world up and the right vector collapses.
        const float lim = 1.5533f;   // 89 degrees
        if (pitch > lim) pitch = lim;
        if (pitch < -lim) pitch = -lim;
    }

    void forward(float out[3]) const {
        const float cp = std::cos(pitch);
        out[0] = std::sin(yaw) * cp;
        out[1] = std::sin(pitch);
        out[2] = std::cos(yaw) * cp;
    }

    void basis(float fwd[3], float right[3], float up[3]) const {
        forward(fwd);
        // right = normalize(cross(fwd, world_up))
        float rx = -fwd[2], ry = 0.0f, rz = fwd[0];
        const float rl = std::sqrt(rx * rx + rz * rz);
        if (rl > 1e-6f) { rx /= rl; rz /= rl; }
        right[0] = rx; right[1] = ry; right[2] = rz;
        // up = cross(right, fwd)
        up[0] = right[1] * fwd[2] - right[2] * fwd[1];
        up[1] = right[2] * fwd[0] - right[0] * fwd[2];
        up[2] = right[0] * fwd[1] - right[1] * fwd[0];
    }

    // WASD over the ground plane, Q/E vertical, shift to sprint. dt-scaled so
    // the flight path does not depend on frame rate -- which it must not,
    // since frame rate is the thing being measured.
    void update(float dt) {
        float fwd[3], right[3], up[3];
        basis(fwd, right, up);

        float mx = 0.0f, mz = 0.0f, my = 0.0f;
        if (GetAsyncKeyState('W') & 0x8000) mz += 1.0f;
        if (GetAsyncKeyState('S') & 0x8000) mz -= 1.0f;
        if (GetAsyncKeyState('D') & 0x8000) mx += 1.0f;
        if (GetAsyncKeyState('A') & 0x8000) mx -= 1.0f;
        if (GetAsyncKeyState('E') & 0x8000) my += 1.0f;
        if (GetAsyncKeyState('Q') & 0x8000) my -= 1.0f;

        float s = speed * dt;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) s *= 5.0f;

        x += (fwd[0] * mz + right[0] * mx) * s;
        y += (fwd[1] * mz + right[1] * mx) * s + my * s;
        z += (fwd[2] * mz + right[2] * mx) * s;
    }
};

}  // namespace engine

#endif  // ENGINE_CAMERA_H
