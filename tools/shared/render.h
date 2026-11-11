// ---------------------------------------------------------------------------
// Presentation. Shared by the validation harness and the native viewer, which
// draw the same worlds -- duplicating the colour ramps would guarantee they
// drift apart.
//
// Nothing here generates terrain. These read a built SeedWorld and nothing
// else, and they are free to use whatever maths they like: none of it reaches
// the golden hash, so CLAUDE.md invariant 9 does not apply on this side.
// ---------------------------------------------------------------------------
#pragma once

#include "worldgen/world.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace view {

using namespace worldgen;

enum ViewMode {
    VIEW_BIOME = 0,
    VIEW_RELIEF = 1,
    VIEW_DANGER = 2
};

inline void biome_colour(int b, unsigned char* c) {
    static const unsigned char kP[9][3] = {
        {126, 200,  80}, { 34, 110,  50}, { 90, 125,  95},
        {150, 140, 120}, {196, 124,  58}, { 66, 132, 124},
        {152,  54,  44}, {219, 232, 245}, {112,  60, 146}
    };
    if (b < 0) { c[0] = 70; c[1] = 70; c[2] = 70; return; }  // land in an ocean cell
    if (b < 9) { c[0] = kP[b][0]; c[1] = kP[b][1]; c[2] = kP[b][2]; return; }
    // Fallback for tables larger than the hand-picked set: golden-angle hues.
    const float hue = static_cast<float>(b) * 137.507f;
    const float hh  = (hue - std::floor(hue / 360.0f) * 360.0f) / 60.0f;
    const float xx  = 1.0f - std::fabs(hh - std::floor(hh / 2.0f) * 2.0f - 1.0f);
    float r = 0, g = 0, bl = 0;
    const int s = static_cast<int>(hh);
    if (s == 0) { r = 1; g = xx; } else if (s == 1) { r = xx; g = 1; }
    else if (s == 2) { g = 1; bl = xx; } else if (s == 3) { g = xx; bl = 1; }
    else if (s == 4) { r = xx; bl = 1; } else { r = 1; bl = xx; }
    c[0] = static_cast<unsigned char>(40 + 190 * r);
    c[1] = static_cast<unsigned char>(40 + 190 * g);
    c[2] = static_cast<unsigned char>(40 + 190 * bl);
}

// Shaded relief. The A4 exit criterion is whether this reads as a world
// rather than as noise, and that is not a thing an assertion can answer.
inline void render_relief_tile(const SeedWorld& w, const WorldParams& wp,
                               const ArchetypeTable& table,
                               std::vector<unsigned char>& img, int img_w,
                               int ox, int oy, int tile) {
    const float R = wp.radius;
    const float step = 2.0f * R / tile;
    const float e = 6.0f;                       // metres, for the normal
    const float lx = -0.55f, lz = -0.55f, ly = 0.63f;   // light from the north-west
    for (int py = 0; py < tile; ++py) {
        const float z = -R + (py + 0.5f) * step;
        for (int px = 0; px < tile; ++px) {
            const float x  = -R + (px + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            unsigned char c[3];
            if (rr > 1.0f) {
                c[0] = 10; c[1] = 12; c[2] = 18;
            } else {
                const float h0 = terrain_height(w, wp, table, x, z, rr);
                float rw = 0.0f, rd = 0.0f;
                const float rdist = nearest_river(w, wp, x, z, rw, rd);
                if (h0 <= 0.0f) {
                    const float d = wg_saturate(-h0 / 160.0f);
                    c[0] = static_cast<unsigned char>(46 - 30 * d);
                    c[1] = static_cast<unsigned char>(78 - 46 * d);
                    c[2] = static_cast<unsigned char>(130 - 62 * d);
                } else if (rdist < rw * 0.42f) {
                    // Inside the channel: draw the water, not the riverbed.
                    c[0] = 52; c[1] = 96; c[2] = 150;
                } else {
                    const float hx = terrain_height(w, wp, table, x + e, z,
                                                    std::sqrt((x + e) * (x + e) + z * z) / R);
                    const float hz = terrain_height(w, wp, table, x, z + e,
                                                    std::sqrt(x * x + (z + e) * (z + e)) / R);
                    const float gx = (hx - h0) / e, gz = (hz - h0) / e;
                    const float inv = 1.0f / std::sqrt(gx * gx + gz * gz + 1.0f);
                    const float shade = wg_saturate((-gx * lx - gz * lz + ly) * inv * 1.25f);
                    const float t = wg_saturate(h0 / 220.0f);
                    float r3, g3, b3;
                    if (t < 0.4f) {
                        const float u = t / 0.4f;
                        r3 = 0.36f + 0.18f * u; g3 = 0.54f - 0.06f * u; b3 = 0.30f + 0.02f * u;
                    } else if (t < 0.78f) {
                        const float u = (t - 0.4f) / 0.38f;
                        r3 = 0.54f + 0.08f * u; g3 = 0.48f - 0.04f * u; b3 = 0.32f + 0.04f * u;
                    } else {
                        const float u = (t - 0.78f) / 0.22f;
                        r3 = 0.62f + 0.30f * u; g3 = 0.44f + 0.50f * u; b3 = 0.36f + 0.58f * u;
                    }
                    const float s = 0.30f + 0.80f * shade;
                    c[0] = static_cast<unsigned char>(wg_saturate(r3 * s) * 255.0f);
                    c[1] = static_cast<unsigned char>(wg_saturate(g3 * s) * 255.0f);
                    c[2] = static_cast<unsigned char>(wg_saturate(b3 * s) * 255.0f);
                }
            }
            const size_t d = (static_cast<size_t>(oy + py) * img_w + (ox + px)) * 3;
            img[d + 0] = c[0]; img[d + 1] = c[1]; img[d + 2] = c[2];
        }
    }
}

inline void render_tile(const SeedWorld& w, const WorldParams& wp,
                        std::vector<unsigned char>& img, int img_w,
                        int ox, int oy, int tile) {
    const float R    = wp.radius;
    const float step = 2.0f * R / tile;
    for (int py = 0; py < tile; ++py) {
        const float z = -R + (py + 0.5f) * step;
        for (int px = 0; px < tile; ++px) {
            const float x  = -R + (px + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            unsigned char c[3];
            if (rr > 1.0f) {
                c[0] = 10; c[1] = 12; c[2] = 18;              // void past the terminator
            } else {
                const float h = layer0_height(x, z, rr, wp, w.s_cont);
                if (h <= wp.sea_level) {
                    const float d = wg_saturate(-h * 1.2f);      // deeper water, darker
                    c[0] = static_cast<unsigned char>(46 - 30 * d);
                    c[1] = static_cast<unsigned char>(78 - 46 * d);
                    c[2] = static_cast<unsigned char>(130 - 62 * d);
                } else {
                    const float wx = x + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wx);
                    const float wz = z + wp.warp_amp * fbm(x * wp.warp_freq, z * wp.warp_freq, 2, 2.0f, 0.5f, w.s_wz);
                    const int   ci = w.lookup.nearest(w.pts, wx, wz);
                    biome_colour(ci >= 0 ? w.biome[ci] : -1, c);
                }
            }
            const size_t d = (static_cast<size_t>(oy + py) * img_w + (ox + px)) * 3;
            img[d + 0] = c[0]; img[d + 1] = c[1]; img[d + 2] = c[2];
        }
    }
}

// 64 seeds, one biome map each, one image. Automated checks cannot tell you
// whether a world is interesting -- and Spearman reads 1.00 in every config we
// have tried, so it will never tell us whether w is right either.

// The danger field itself, which is the one thing on screen whose shape is a
// pure feel judgement. The ramp is deliberately harsh so radial banding is
// obvious rather than tasteful.
inline void danger_colour(float d, unsigned char* c) {
    float r, g, b;
    if (d < 0.5f) {
        const float u = d / 0.5f;
        r = 0.16f + 0.69f * u; g = 0.34f + 0.48f * u; b = 0.62f - 0.17f * u;
    } else {
        const float u = (d - 0.5f) / 0.5f;
        r = 0.85f - 0.15f * u; g = 0.82f - 0.65f * u; b = 0.45f - 0.29f * u;
    }
    c[0] = static_cast<unsigned char>(r * 255.0f);
    c[1] = static_cast<unsigned char>(g * 255.0f);
    c[2] = static_cast<unsigned char>(b * 255.0f);
}

inline void render_danger_tile(const SeedWorld& w, const WorldParams& wp,
                               std::vector<unsigned char>& img, int img_w,
                               int ox, int oy, int tile) {
    const float R = wp.radius;
    const float step = 2.0f * R / tile;
    for (int py = 0; py < tile; ++py) {
        const float z = -R + (py + 0.5f) * step;
        for (int px = 0; px < tile; ++px) {
            const float x = -R + (px + 0.5f) * step;
            const float rr = std::sqrt(x * x + z * z) / R;
            unsigned char c[3];
            if (rr > 1.0f) {
                c[0] = 10; c[1] = 12; c[2] = 18;
            } else {
                danger_colour(layer1_danger(x, z, rr, wp, w.s_dang), c);
            }
            const size_t d = (static_cast<size_t>(oy + py) * img_w + (ox + px)) * 3;
            img[d + 0] = c[0]; img[d + 1] = c[1]; img[d + 2] = c[2];
        }
    }
}

// One entry point, so a caller picks a mode rather than a function.
inline void render_view(const SeedWorld& w, const WorldParams& wp,
                        const ArchetypeTable& table, ViewMode mode,
                        std::vector<unsigned char>& img, int img_w,
                        int ox, int oy, int tile) {
    if (mode == VIEW_RELIEF)      render_relief_tile(w, wp, table, img, img_w, ox, oy, tile);
    else if (mode == VIEW_DANGER) render_danger_tile(w, wp, img, img_w, ox, oy, tile);
    else                          render_tile(w, wp, img, img_w, ox, oy, tile);
}

}  // namespace view
