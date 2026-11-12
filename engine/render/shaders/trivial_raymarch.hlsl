// ---------------------------------------------------------------------------
// B1: put the trivial analytic field on screen.
//
// This raymarches the field rather than meshing it. B2 replaces the marching
// with surface nets, and the image this produces is the reference the meshed
// version has to match -- a mesh that disagrees with the field it was built
// from is the bug B2 is most likely to have, and it is much easier to see than
// to assert.
//
// The field comes in through the ADR-001 dialect header, unmodified.
// ---------------------------------------------------------------------------

#include "engine/field/trivial_field.h"

cbuffer View : register(b0) {
    float3 cam_pos;    float time;
    float3 cam_fwd;    float aspect;
    float3 cam_right;  float tan_half_fov;
    float3 cam_up;     float pad0;
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

// Fullscreen triangle from the vertex id alone: no vertex buffer, no input
// layout, nothing to keep in sync with a struct on the C++ side.
VSOut vs_main(uint vid : SV_VertexID) {
    VSOut o;
    const float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

static const float kMaxDist  = 420.0f;
static const int   kMaxSteps = 320;

// Central differences on the field. The same gradient surface nets will use
// for its normals, so a shading artefact here is a real field artefact.
//
// NEGATED, and that is the whole subtlety. Density is positive inside rock, so
// the gradient points the way density increases, which is INTO the solid. The
// outward surface normal is minus the gradient. Getting this backwards lights
// every surface from behind: the first run of B1 rendered the overhang sphere
// lit on its underside and the ground almost black, which is what that looks
// like. B2 inherits this convention for its mesh normals.
float3 field_normal(float3 p) {
    const float e = 0.12f;
    const float dx = trivial_density(p.x + e, p.y, p.z) - trivial_density(p.x - e, p.y, p.z);
    const float dy = trivial_density(p.x, p.y + e, p.z) - trivial_density(p.x, p.y - e, p.z);
    const float dz = trivial_density(p.x, p.y, p.z + e) - trivial_density(p.x, p.y, p.z - e);
    return normalize(-float3(dx, dy, dz));
}

float4 ps_main(VSOut i) : SV_Target {
    // Pixel to ray. uv is 0..1 with y down, so the vertical term is negated.
    const float2 ndc = float2(i.uv.x * 2.0f - 1.0f, 1.0f - i.uv.y * 2.0f);
    const float3 dir = normalize(cam_fwd
                               + cam_right * (ndc.x * aspect * tan_half_fov)
                               + cam_up    * (ndc.y * tan_half_fov));

    // Fixed-step march. The field is not a true SDF -- `ground` is a height
    // difference, not a distance -- so sphere tracing would overshoot through
    // thin rock. Uniform steps are slower and correct; B2 stops caring.
    float t = 0.0f;
    bool  hit = false;
    float3 p = cam_pos;
    const float step = kMaxDist / kMaxSteps;

    for (int s = 0; s < kMaxSteps; ++s) {
        p = cam_pos + dir * t;
        if (trivial_density(p.x, p.y, p.z) > 0.0f) { hit = true; break; }
        t += step;
    }

    if (!hit) {
        // Sky: a plain vertical gradient. Nothing here is art direction, it is
        // just something that is obviously not terrain.
        const float h = saturate(dir.y * 0.5f + 0.5f);
        return float4(lerp(float3(0.52f, 0.60f, 0.72f),
                           float3(0.18f, 0.32f, 0.58f), h), 1.0f);
    }

    // One bisection pass to pull the hit back onto the surface. Without it the
    // silhouette carries the step size as visible banding.
    float lo = t - step, hi = t;
    for (int b = 0; b < 6; ++b) {
        const float mid = 0.5f * (lo + hi);
        const float3 q = cam_pos + dir * mid;
        if (trivial_density(q.x, q.y, q.z) > 0.0f) { hi = mid; } else { lo = mid; }
    }
    p = cam_pos + dir * hi;

    const float3 n = field_normal(p);
    const float3 sun = normalize(float3(0.45f, 0.78f, 0.35f));
    const float  lam = saturate(dot(n, sun));
    const float  sky = saturate(n.y * 0.5f + 0.5f);

    float3 rock = float3(0.46f, 0.40f, 0.34f);
    // Tint the upward faces so the terrain reads as a surface rather than a
    // grey blob, and so the overhang's underside is obviously an underside.
    rock = lerp(rock, float3(0.34f, 0.46f, 0.28f), saturate(n.y * n.y));

    float3 col = rock * (0.22f + 0.78f * lam) + float3(0.10f, 0.13f, 0.18f) * sky;

    // Distance fade into the sky colour, so the far edge of the march does not
    // end in a hard line.
    const float fog = saturate(hi / kMaxDist);
    col = lerp(col, float3(0.40f, 0.50f, 0.66f), fog * fog);

    return float4(col, 1.0f);
}
