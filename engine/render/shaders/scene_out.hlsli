// ---------------------------------------------------------------------------
// What every scene shader writes: the sun's direct light and everything else
// (sky, bounce, fog) in two targets, so the post chain can darken each with
// the right thing -- screen-space contact shadows multiply the direct light,
// ambient occlusion the ambient. Multiplying one combined colour by both
// would darken sunlit ground in creases that the sky cannot reach, which is
// the classic AO look of everything sitting in dirt.
//
// Fog belongs to the ambient side: it is light scattered in from the sky, and
// a fogged pixel should not be shadowed by the geometry it hides.
//
// Since ADR-004 L1 both targets hold RADIANCE in nits (lighting_units.h):
// auto-exposure and the tonemap turn it into a picture in post.hlsl.
// ---------------------------------------------------------------------------
#ifndef ENGINE_SCENE_OUT_HLSLI
#define ENGINE_SCENE_OUT_HLSLI

#include "engine/render/shaders/lighting.hlsli"

struct SceneOut {
    float4 direct  : SV_Target0;
    float4 ambient : SV_Target1;
};

// No fog here since L2: the atmosphere's aerial perspective is applied to
// every pixel by depth in the post chain's composite, so a surface writes
// the light that leaves it and nothing else. `world`, `cam` and `far_plane`
// are kept so no caller changed.
// `contact` (the direct target's alpha) says how much screen-space contact
// shadows may darken this surface: 1 for ground and bark, little or none for
// leaves and grass, which shadow each other everywhere within the march and
// are already shadowed at their own scale by the cascades. Without it the
// first version turned every crown nearly black.
// `ao` (the ambient target's alpha) does the same for ambient occlusion:
// dense grass occludes itself in depth everywhere and went to a dark carpet
// at full strength.
SceneOut scene_out(float3 direct, float3 ambient, float3 world, float3 cam, float far_plane,
                   float contact = 1.0f, float ao = 1.0f) {
    SceneOut o;
    o.direct = float4(min(direct, kSceneMaxNits), contact);
    o.ambient = float4(min(ambient, kSceneMaxNits), ao);
    return o;
}

// A debug colour, shown as is: no exposure, no tonemap, no AO, no contact
// shadow. A NEGATIVE ambient alpha marks it, and post.hlsl carries the mark
// through the composite and TAA to the final pass, which passes the colour
// straight to the screen. Without the mark auto-exposure would meter a
// magenta of 1 nit against a sky of 7000 and show it as black.
SceneOut scene_flat(float3 c) {
    SceneOut o;
    o.direct = float4(0.0f, 0.0f, 0.0f, 0.0f);
    o.ambient = float4(c, -1.0f);
    return o;
}

#endif
